namespace RecGo {

    [DBus (name = "io.github.Antux912012.RecGo.Control")]
    public class ControlService : GLib.Object {
        private weak Application app;
        private GLib.DBusConnection? conn;
        private string object_path;

        public ControlService (Application app, GLib.DBusConnection conn, string object_path) {
            this.app = app;
            this.conn = conn;
            this.object_path = object_path;
        }

        public bool start () throws GLib.Error {
            var win = app.get_or_create_window ();
            win.start_recording_action ();
            return true;
        }

        public bool pause () throws GLib.Error {
            var win = app.active_recgo_window ();
            if (win != null) {
                win.toggle_pause_action ();
                return win.is_paused;
            }
            return false;
        }

        public bool stop () throws GLib.Error {
            var win = app.active_recgo_window ();
            if (win != null) {
                win.stop_recording_action ();
                return true;
            }
            return false;
        }

        public string state {
            owned get {
                var win = app.active_recgo_window ();
                return (win != null) ? win.get_control_state () : "idle";
            }
        }

        public uint64 elapsed_seconds {
            get {
                var win = app.active_recgo_window ();
                return (win != null) ? win.elapsed_seconds : 0;
            }
        }

        [DBus (visible = false)]
        public void notify_state_changed () {
            if (conn == null) return;
            var changed = new GLib.VariantBuilder (new GLib.VariantType ("a{sv}"));
            changed.add ("{sv}", "State", new GLib.Variant.string (state));
            changed.add ("{sv}", "ElapsedSeconds", new GLib.Variant.uint64 (elapsed_seconds));
            var invalidated = new GLib.VariantBuilder (new GLib.VariantType ("as"));
            try {
                conn.emit_signal (
                    null,
                    object_path,
                    "org.freedesktop.DBus.Properties",
                    "PropertiesChanged",
                    new GLib.Variant ("(sa{sv}as)", "io.github.Antux912012.RecGo.Control", changed, invalidated)
                );
            } catch (GLib.Error e) {
                // Ignore D-Bus emit failure
            }
        }
    }

    public class Application : Adw.Application {
        private ControlService? control_service;
        private GlobalShortcutsClient? shortcuts_client;
        private uint dbus_reg_id = 0;

        public Application () {
            Object (
                application_id: Config.APP_ID,
                flags: GLib.ApplicationFlags.DEFAULT_FLAGS
            );
        }

        public Window? active_recgo_window () {
            return get_active_window () as Window;
        }

        public Window get_or_create_window () {
            var win = active_recgo_window ();
            if (win == null) {
                win = new Window (this);
                win.control_state_changed.connect (() => {
                    if (control_service != null) {
                        control_service.notify_state_changed ();
                    }
                });
            }
            return win;
        }

        protected override void startup () {
            base.startup ();

            var display = Gdk.Display.get_default ();
            if (display != null) {
                var icon_theme = Gtk.IconTheme.get_for_display (display);
                icon_theme.add_resource_path ("/io/github/Antux912012/RecGo/icons/papirus");

                var css = new Gtk.CssProvider ();
                css.load_from_resource ("/io/github/Antux912012/RecGo/style.css");
                Gtk.StyleContext.add_provider_for_display (
                    display,
                    css,
                    Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
                );
            }

            shortcuts_client = new GlobalShortcutsClient ();
            shortcuts_client.shortcut_activated.connect ((id) => {
                var win = get_or_create_window ();
                if (id == "start-stop-recording") {
                    if (win.is_recording) win.stop_recording_action ();
                    else win.start_recording_action ();
                } else if (id == "pause-resume-recording") {
                    win.toggle_pause_action ();
                } else if (id == "cycle-webcam-shape") {
                    win.cycle_webcam_shape_action ();
                }
            });
            shortcuts_client.start_async.begin ();
        }

        protected override void activate () {
            var win = get_or_create_window ();
            win.present ();
        }

        protected override bool dbus_register (GLib.DBusConnection connection, string object_path) throws GLib.Error {
            if (!base.dbus_register (connection, object_path))
                return false;

            control_service = new ControlService (this, connection, object_path);
            dbus_reg_id = connection.register_object (object_path, control_service);
            return true;
        }

        protected override void dbus_unregister (GLib.DBusConnection connection, string object_path) {
            if (dbus_reg_id != 0) {
                connection.unregister_object (dbus_reg_id);
                dbus_reg_id = 0;
            }
            control_service = null;
            base.dbus_unregister (connection, object_path);
        }
    }
}
