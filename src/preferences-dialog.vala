namespace RecGo {

    public class PreferencesDialog : Adw.PreferencesDialog {
        private GLib.Settings settings;
        private Gst.DeviceManager device_manager;
        private Adw.PreferencesGroup cam_group;
        private Adw.PreferencesGroup mic_group;
        private GLib.GenericArray<Gtk.Widget> cam_rows = new GLib.GenericArray<Gtk.Widget> ();
        private GLib.GenericArray<Gtk.Widget> mic_rows = new GLib.GenericArray<Gtk.Widget> ();

        public signal void codec_changed (int codec_index);
        public signal void countdown_changed (uint seconds, uint custom_seconds);
        public signal void devices_selection_changed ();

        public PreferencesDialog (GLib.Settings settings,
                                  Gst.DeviceManager device_manager,
                                  int current_codec_idx) {
            this.settings = settings;
            this.device_manager = device_manager;

            set_title ("Preferences & Diagnostics");

            build_recording_page (current_codec_idx);
            build_devices_page ();
            build_encoders_page ();

            device_manager.devices_changed.connect (() => {
                populate_device_rows ();
            });
        }

        private void build_recording_page (int current_codec_idx) {
            var rec_page = new Adw.PreferencesPage ();
            rec_page.set_title ("Recording");
            rec_page.set_icon_name ("media-record-symbolic");

            // Countdown group
            var cd_group = new Adw.PreferencesGroup ();
            cd_group.set_title ("Countdown Before Recording");
            cd_group.set_description ("Prepare before capturing starts with an automatic countdown timer.");

            string[] cd_options = {
                "No Delay (0s)",
                "3 seconds",
                "5 seconds",
                "10 seconds",
                "Custom Duration (up to 5 min)"
            };
            var cd_list = new Gtk.StringList (cd_options);
            var cd_combo = new Adw.ComboRow ();
            cd_combo.set_title ("Countdown Preset");
            cd_combo.set_subtitle ("Time delay after pressing Record");
            cd_combo.set_model (cd_list);

            uint cd_secs = settings.get_uint ("countdown-seconds");
            uint custom_secs = settings.get_uint ("custom-countdown-seconds");

            uint initial_idx;
            switch (cd_secs) {
            case 0:  initial_idx = 0; break;
            case 3:  initial_idx = 1; break;
            case 5:  initial_idx = 2; break;
            case 10: initial_idx = 3; break;
            default: initial_idx = 4; break;
            }
            cd_combo.set_selected (initial_idx);

            var spin_row = new Adw.SpinRow.with_range (1.0, 300.0, 1.0);
            spin_row.set_title ("Custom Duration (seconds)");
            spin_row.set_subtitle ("Configurable from 1 second up to 300 seconds (5 minutes)");
            double init_spin = (initial_idx == 4) ? (double) cd_secs : (double) custom_secs;
            spin_row.set_value (init_spin.clamp (1.0, 300.0));
            spin_row.set_visible (initial_idx == 4);

            cd_combo.notify["selected"].connect (() => {
                uint sel = cd_combo.get_selected ();
                spin_row.set_visible (sel == 4);
                uint secs;
                switch (sel) {
                case 0: secs = 0; break;
                case 1: secs = 3; break;
                case 2: secs = 5; break;
                case 3: secs = 10; break;
                default: secs = (uint) spin_row.get_value (); break;
                }
                countdown_changed (secs, (uint) spin_row.get_value ());
            });

            spin_row.get_adjustment ().value_changed.connect (() => {
                if (cd_combo.get_selected () == 4) {
                    uint val = (uint) spin_row.get_value ();
                    countdown_changed (val, val);
                }
            });

            cd_group.add (cd_combo);
            cd_group.add (spin_row);
            rec_page.add (cd_group);

            // Encoder selection group
            var enc_group = new Adw.PreferencesGroup ();
            enc_group.set_title ("Video Encoder & Container Format");
            enc_group.set_description ("Select the video format and hardware-accelerated encoder used for recordings.");

            var codecs = Gst.get_supported_codecs ();
            string[] titles = new string[codecs.length];
            for (uint i = 0; i < codecs.length; i++) {
                titles[i] = codecs.get (i).display_name;
            }
            var str_list = new Gtk.StringList (titles);
            var enc_combo = new Adw.ComboRow ();
            enc_combo.set_title ("Active Video Encoder");
            enc_combo.set_subtitle ("Hardware GPU encoders (VA-API / Vulkan) minimize CPU overhead");
            enc_combo.set_model (str_list);
            if (current_codec_idx >= 0 && (uint) current_codec_idx < codecs.length) {
                enc_combo.set_selected ((uint) current_codec_idx);
            }
            enc_combo.notify["selected"].connect (() => {
                codec_changed ((int) enc_combo.get_selected ());
            });
            enc_group.add (enc_combo);

            var gpu_comp_row = new Adw.SwitchRow ();
            gpu_comp_row.set_title ("OpenGL GPU Compositor");
            gpu_comp_row.set_subtitle ("Composite multi-camera and screen layers on GPU via glvideomixer");
            settings.bind ("gpu-compositing", gpu_comp_row, "active", GLib.SettingsBindFlags.DEFAULT);
            enc_group.add (gpu_comp_row);

            rec_page.add (enc_group);

            // Hardware Acceleration Status
            string accel_summary;
            Gst.detect_gpu_acceleration (out accel_summary);
            var gpu_group = new Adw.PreferencesGroup ();
            gpu_group.set_title ("Hardware Acceleration Status");
            gpu_group.set_description ("Active Pipelines: %s".printf (accel_summary));

            add_backend_row (gpu_group,
                "VA-API Hardware Video Engine (Intel / AMD)",
                Gst.has_gst_element ("vapostproc") || Gst.has_gst_element ("vah264enc"),
                "vah264enc / vah265enc / vavp8enc / vapostproc");

            add_backend_row (gpu_group,
                "Vulkan Compute & Video Acceleration",
                Gst.has_gst_element ("vulkanupload") && Gst.has_gst_element ("vulkancolorconvert"),
                "vulkanupload / vulkancolorconvert / vulkanh264enc");

            add_backend_row (gpu_group,
                "OpenGL / GLSL Shader Compositor",
                Gst.has_gst_element ("glupload") && Gst.has_gst_element ("glvideomixer"),
                "glupload / glcolorconvert / glvideomixer / glshader");

            rec_page.add (gpu_group);
            add (rec_page);
        }

        private void build_devices_page () {
            var dev_page = new Adw.PreferencesPage ();
            dev_page.set_title ("Cameras & Audio");
            dev_page.set_icon_name ("camera-web-symbolic");

            cam_group = new Adw.PreferencesGroup ();
            cam_group.set_title ("Cameras (Multi-Cam)");
            cam_group.set_description ("Enable one or more cameras for Picture-in-Picture, Side-by-Side, or Multi-Cam Grid layouts.");
            dev_page.add (cam_group);

            mic_group = new Adw.PreferencesGroup ();
            mic_group.set_title ("Microphones (Multi-Mic Mixer)");
            mic_group.set_description ("Enable one or more microphones. Multiple active microphones are mixed automatically.");
            dev_page.add (mic_group);

            populate_device_rows ();
            add (dev_page);
        }

        private static bool str_in_array (string[] arr, string val) {
            foreach (unowned string s in arr) {
                if (s == val)
                    return true;
            }
            return false;
        }

        private void clear_group_rows (Adw.PreferencesGroup group, GLib.GenericArray<Gtk.Widget> rows) {
            for (uint i = 0; i < rows.length; i++) {
                group.remove (rows.get (i));
            }
            rows.remove_range (0, rows.length);
        }

        private void populate_device_rows () {
            clear_group_rows (cam_group, cam_rows);
            clear_group_rows (mic_group, mic_rows);

            var cameras = device_manager.get_cameras ();
            string[] enabled_cams = settings.get_strv ("enabled-cameras");

            if (cameras.length == 0) {
                var empty_row = new Adw.ActionRow ();
                empty_row.set_title ("No cameras detected");
                empty_row.set_subtitle ("Connect a USB or PipeWire webcam to use camera overlays");
                cam_group.add (empty_row);
                cam_rows.add (empty_row);
            } else {
                for (uint i = 0; i < cameras.length; i++) {
                    unowned var cam = cameras.get (i);
                    string cam_id = cam.id;
                    var row = new Adw.SwitchRow ();
                    row.set_title (cam.display_name);
                    row.set_subtitle ("%s (%s)".printf (cam.id, cam.gst_element));
                    bool active = (enabled_cams.length == 0 && i == 0) || str_in_array (enabled_cams, cam_id);
                    row.set_active (active);

                    row.notify["active"].connect (() => {
                        update_strv_setting ("enabled-cameras", cam_id, row.get_active (), cameras);
                    });

                    cam_group.add (row);
                    cam_rows.add (row);
                }
            }

            var mics = device_manager.get_microphones ();
            string[] enabled_mics = settings.get_strv ("enabled-microphones");

            if (mics.length == 0) {
                var empty_row = new Adw.ActionRow ();
                empty_row.set_title ("No microphones detected");
                empty_row.set_subtitle ("Default PulseAudio / PipeWire input will be used if available");
                mic_group.add (empty_row);
                mic_rows.add (empty_row);
            } else {
                for (uint i = 0; i < mics.length; i++) {
                    unowned var mic = mics.get (i);
                    string mic_id = mic.id;
                    var row = new Adw.SwitchRow ();
                    row.set_title (mic.display_name);
                    row.set_subtitle ("%s (%s)".printf (mic.id, mic.gst_element));
                    bool active = (enabled_mics.length == 0 && (mic.is_default || i == 0))
                                  || str_in_array (enabled_mics, mic_id);
                    row.set_active (active);

                    row.notify["active"].connect (() => {
                        update_strv_setting ("enabled-microphones", mic_id, row.get_active (), mics);
                    });

                    mic_group.add (row);
                    mic_rows.add (row);
                }
            }
        }

        private void update_strv_setting (string key,
                                          string target_id,
                                          bool enabled,
                                          GLib.GenericArray<Gst.DeviceInfo> all_devices) {
            string[] current = settings.get_strv (key);
            var list = new GLib.GenericArray<string> ();

            if (current.length == 0 && all_devices.length > 0) {
                // Materialize implicit default (index 0) before toggling
                for (uint i = 0; i < all_devices.length; i++) {
                    unowned var dev = all_devices.get (i);
                    bool def = (key == "enabled-microphones") ? (dev.is_default || i == 0) : (i == 0);
                    if (def && dev.id != target_id) {
                        list.add (dev.id);
                    }
                }
            } else {
                foreach (unowned string s in current) {
                    if (s != target_id) {
                        list.add (s);
                    }
                }
            }

            if (enabled) {
                list.add (target_id);
            }

            string[] out_arr = new string[list.length];
            for (uint i = 0; i < list.length; i++) {
                out_arr[i] = list.get (i);
            }
            settings.set_strv (key, out_arr);
            devices_selection_changed ();
        }

        private void build_encoders_page () {
            var diag_page = new Adw.PreferencesPage ();
            diag_page.set_title ("Encoders");
            diag_page.set_icon_name ("video-display-symbolic");

            var enc_group = new Adw.PreferencesGroup ();
            enc_group.set_title ("Available GStreamer Elements");
            enc_group.set_description ("Hardware encoders run directly on your GPU with near-zero CPU load.");

            var codecs = Gst.get_supported_codecs ();
            for (uint i = 0; i < codecs.length; i++) {
                unowned var info = codecs.get (i);
                var row = new Adw.ActionRow ();
                row.set_title (info.display_name);
                row.set_subtitle ("GStreamer element: %s — %s".printf (info.element_name, info.backend_label));
                var badge = new Gtk.Label (info.is_hardware ? "GPU Hardware" : "CPU Software");
                badge.add_css_class (info.is_hardware ? "accent" : "dim-label");
                row.add_suffix (badge);
                enc_group.add (row);
            }

            diag_page.add (enc_group);
            add (diag_page);
        }

        private static void add_backend_row (Adw.PreferencesGroup group,
                                             string title,
                                             bool active,
                                             string subtitle) {
            var row = new Adw.ActionRow ();
            row.set_title (title);
            row.set_subtitle (subtitle);
            var badge = new Gtk.Label (active ? "ACTIVE (GPU)" : "UNAVAILABLE");
            badge.add_css_class (active ? "success" : "dim-label");
            row.add_suffix (badge);
            group.add (row);
        }
    }
}
