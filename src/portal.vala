namespace RecGo {

    public class CaptureStream : GLib.Object {
        public int fd { get; private set; default = -1; }
        public uint32 node_id { get; private set; }
        public int width { get; private set; }
        public int height { get; private set; }

        private GLib.DBusConnection? conn;
        private string? session_handle;

        public CaptureStream (GLib.DBusConnection conn,
                              string session_handle,
                              int fd,
                              uint32 node_id,
                              int width,
                              int height) {
            this.conn = conn;
            this.session_handle = session_handle;
            this.fd = fd;
            this.node_id = node_id;
            this.width = width;
            this.height = height;
        }

        ~CaptureStream () {
            if (fd >= 0) {
                Posix.close (fd);
                fd = -1;
            }
            if (conn != null && session_handle != null) {
                try {
                    conn.call_sync (
                        "org.freedesktop.portal.Desktop",
                        session_handle,
                        "org.freedesktop.portal.Session",
                        "Close",
                        null,
                        null,
                        GLib.DBusCallFlags.NONE,
                        2000,
                        null
                    );
                } catch (GLib.Error e) {
                    // Session may already be closed
                }
            }
        }
    }

    public class PortalClient : GLib.Object {
        private static int token_counter = 0;

        public static string next_token (string prefix) {
            token_counter++;
            return "%s_%d_%d".printf (prefix, (int) Posix.getpid (), token_counter);
        }

        public static string expected_request_path (GLib.DBusConnection conn, string token) {
            string sender = conn.get_unique_name () ?? ":1.0";
            string clean = sender.substring (1).replace (".", "_");
            return "/org/freedesktop/portal/desktop/request/%s/%s".printf (clean, token);
        }

        public static async GLib.Variant call_portal_request (GLib.DBusConnection conn,
                                                              string iface,
                                                              string method,
                                                              GLib.Variant args,
                                                              string request_path) throws GLib.Error {
            GLib.Variant? result_variant = null;
            uint32 response_code = 2;
            bool done = false;
            GLib.SourceFunc cb = call_portal_request.callback;

            uint sub_id = conn.signal_subscribe (
                "org.freedesktop.portal.Desktop",
                "org.freedesktop.portal.Request",
                "Response",
                request_path,
                null,
                GLib.DBusSignalFlags.NONE,
                (c, sender, obj_path, sig_iface, sig_name, parameters) => {
                    if (done)
                        return;
                    done = true;
                    parameters.get ("(u@a{sv})", out response_code, out result_variant);
                    cb ();
                }
            );

            try {
                yield conn.call (
                    "org.freedesktop.portal.Desktop",
                    "/org/freedesktop/portal/desktop",
                    iface,
                    method,
                    args,
                    new GLib.VariantType ("(o)"),
                    GLib.DBusCallFlags.NONE,
                    -1,
                    null
                );
            } catch (GLib.Error e) {
                conn.signal_unsubscribe (sub_id);
                throw e;
            }

            if (!done) {
                yield;
            }
            conn.signal_unsubscribe (sub_id);

            if (response_code != 0 || result_variant == null) {
                throw new GLib.IOError.CANCELLED ("Portal request cancelled or failed (code %u)", response_code);
            }
            return result_variant;
        }

        public static async CaptureStream start_screencast (uint32 source_type) throws GLib.Error {
            var conn = yield GLib.Bus.get (GLib.BusType.SESSION);

            // 1. CreateSession
            string create_tok = next_token ("recgo_req");
            string sess_tok = next_token ("recgo_sess");
            string req_path = expected_request_path (conn, create_tok);

            var create_opts = new GLib.VariantBuilder (new GLib.VariantType ("a{sv}"));
            create_opts.add ("{sv}", "handle_token", new GLib.Variant.string (create_tok));
            create_opts.add ("{sv}", "session_handle_token", new GLib.Variant.string (sess_tok));

            var create_res = yield call_portal_request (
                conn,
                "org.freedesktop.portal.ScreenCast",
                "CreateSession",
                new GLib.Variant ("(a{sv})", create_opts),
                req_path
            );

            string session_handle = "";
            if (!create_res.lookup ("session_handle", "s", out session_handle)) {
                throw new GLib.IOError.FAILED ("Portal CreateSession missing session_handle");
            }

            // 2. SelectSources (types: 1=Monitor, 2=Window; cursor_mode: 2=Embedded)
            string sel_tok = next_token ("recgo_sel");
            string sel_req_path = expected_request_path (conn, sel_tok);
            var sel_opts = new GLib.VariantBuilder (new GLib.VariantType ("a{sv}"));
            sel_opts.add ("{sv}", "handle_token", new GLib.Variant.string (sel_tok));
            sel_opts.add ("{sv}", "types", new GLib.Variant.uint32 (source_type));
            sel_opts.add ("{sv}", "multiple", new GLib.Variant.boolean (false));
            sel_opts.add ("{sv}", "cursor_mode", new GLib.Variant.uint32 (2));
            sel_opts.add ("{sv}", "persist_mode", new GLib.Variant.uint32 (2));

            yield call_portal_request (
                conn,
                "org.freedesktop.portal.ScreenCast",
                "SelectSources",
                new GLib.Variant ("(oa{sv})", session_handle, sel_opts),
                sel_req_path
            );

            // 3. Start
            string start_tok = next_token ("recgo_start");
            string start_req_path = expected_request_path (conn, start_tok);
            var start_opts = new GLib.VariantBuilder (new GLib.VariantType ("a{sv}"));
            start_opts.add ("{sv}", "handle_token", new GLib.Variant.string (start_tok));

            var start_res = yield call_portal_request (
                conn,
                "org.freedesktop.portal.ScreenCast",
                "Start",
                new GLib.Variant ("(osa{sv})", session_handle, "", start_opts),
                start_req_path
            );

            GLib.Variant? streams_var = start_res.lookup_value ("streams", new GLib.VariantType ("a(ua{sv})"));
            if (streams_var == null || streams_var.n_children () == 0) {
                throw new GLib.IOError.CANCELLED ("No screencast stream selected");
            }

            uint32 node_id = 0;
            GLib.Variant? stream_props = null;
            streams_var.get_child (0, "(u@a{sv})", out node_id, out stream_props);

            int width = 1920;
            int height = 1080;
            if (stream_props != null) {
                GLib.Variant? size_var = stream_props.lookup_value ("size", new GLib.VariantType ("(ii)"));
                if (size_var != null) {
                    size_var.get ("(ii)", out width, out height);
                }
            }

            // 4. OpenPipeWireRemote
            var open_opts = new GLib.VariantBuilder (new GLib.VariantType ("a{sv}"));
            GLib.UnixFDList out_fd_list;
            var fd_ret = yield conn.call_with_unix_fd_list (
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.ScreenCast",
                "OpenPipeWireRemote",
                new GLib.Variant ("(oa{sv})", session_handle, open_opts),
                new GLib.VariantType ("(h)"),
                GLib.DBusCallFlags.NONE,
                -1,
                null,
                null,
                out out_fd_list
            );

            int32 fd_idx = 0;
            fd_ret.get ("(h)", out fd_idx);
            int pw_fd = out_fd_list.get (fd_idx);

            return new CaptureStream (conn, session_handle, pw_fd, node_id, width, height);
        }
    }

    public class GlobalShortcutsClient : GLib.Object {
        public signal void shortcut_activated (string shortcut_id);

        private GLib.DBusConnection? conn;
        private string? session_handle;
        private uint sub_id = 0;

        public async void start_async () {
            try {
                conn = yield GLib.Bus.get (GLib.BusType.SESSION);

                string tok = PortalClient.next_token ("recgo_gs");
                string req_path = PortalClient.expected_request_path (conn, tok);

                var opts = new GLib.VariantBuilder (new GLib.VariantType ("a{sv}"));
                opts.add ("{sv}", "handle_token", new GLib.Variant.string (tok));
                opts.add ("{sv}", "session_handle_token", new GLib.Variant.string ("recgo_gs_sess"));

                var resp_dict = yield PortalClient.call_portal_request (
                    conn,
                    "org.freedesktop.portal.GlobalShortcuts",
                    "CreateSession",
                    new GLib.Variant ("(a{sv})", opts),
                    req_path
                );

                string sess = "";
                if (!resp_dict.lookup ("session_handle", "s", out sess))
                    return;
                session_handle = sess;

                sub_id = conn.signal_subscribe (
                    "org.freedesktop.portal.Desktop",
                    "org.freedesktop.portal.GlobalShortcuts",
                    "Activated",
                    "/org/freedesktop/portal/desktop",
                    null,
                    GLib.DBusSignalFlags.NONE,
                    (c, s, o, iface, sig, parameters) => {
                        string act_sess = "";
                        string shortcut_id = "";
                        uint64 ts = 0;
                        GLib.Variant? act_opts = null;
                        parameters.get ("(ost@a{sv})", out act_sess, out shortcut_id, out ts, out act_opts);
                        if (act_sess == session_handle) {
                            shortcut_activated (shortcut_id);
                        }
                    }
                );

                var shortcuts = new GLib.VariantBuilder (new GLib.VariantType ("a(sa{sv})"));
                add_shortcut (shortcuts, "start-stop-recording", "Start or Stop Recording", "<Control><Alt><Shift>R");
                add_shortcut (shortcuts, "pause-resume-recording", "Pause or Resume Recording", "<Control><Alt><Shift>P");
                add_shortcut (shortcuts, "cycle-webcam-shape", "Change Webcam Shape", "<Control><Alt><Shift>C");

                string bind_tok = PortalClient.next_token ("recgo_gs_bind");
                var bind_opts = new GLib.VariantBuilder (new GLib.VariantType ("a{sv}"));
                bind_opts.add ("{sv}", "handle_token", new GLib.Variant.string (bind_tok));

                yield conn.call (
                    "org.freedesktop.portal.Desktop",
                    "/org/freedesktop/portal/desktop",
                    "org.freedesktop.portal.GlobalShortcuts",
                    "BindShortcuts",
                    new GLib.Variant ("(oa(sa{sv})sa{sv})", session_handle, shortcuts, "", bind_opts),
                    new GLib.VariantType ("(o)"),
                    GLib.DBusCallFlags.NONE,
                    -1,
                    null
                );
            } catch (GLib.Error e) {
                // GlobalShortcuts portal is optional on some compositors
            }
        }

        private static void add_shortcut (GLib.VariantBuilder b, string id, string desc, string trigger) {
            var d = new GLib.VariantBuilder (new GLib.VariantType ("a{sv}"));
            d.add ("{sv}", "description", new GLib.Variant.string (desc));
            d.add ("{sv}", "preferred_trigger", new GLib.Variant.string (trigger));
            b.add ("(sa{sv})", id, d);
        }

        ~GlobalShortcutsClient () {
            if (conn != null && sub_id != 0) {
                conn.signal_unsubscribe (sub_id);
                sub_id = 0;
            }
        }
    }
}
