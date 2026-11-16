namespace RecGo {

    public class Window : Adw.ApplicationWindow {
        private GLib.Settings settings;
        private Gst.DeviceManager device_manager;
        private PreviewWidget preview;
        private Adw.ToastOverlay toast_overlay;
        private Adw.WindowTitle win_title;
        private Gtk.Label status_label;

        private Gtk.DropDown source_dropdown;
        private Gtk.DropDown layout_dropdown;
        private Gtk.DropDown shape_dropdown;
        private Gtk.DropDown countdown_dropdown;
        private Gtk.ToggleButton cam_toggle;
        private Gtk.ToggleButton mic_toggle;
        private Gtk.Scale size_scale;

        private Gtk.Button btn_preview_screen;
        private Gtk.Button btn_record;
        private Gtk.Image rec_icon;
        private Gtk.Label rec_label;
        private Gtk.ToggleButton btn_pause;
        private Gtk.Button btn_stop;
        private Gtk.Label osd_dot;
        private Gtk.Label osd_text;

        private GLib.GenericArray<Gst.CodecInfo> supported_codecs;
        private int selected_codec_idx = 0;

        private GLib.GenericArray<Gst.PreviewFeed> webcam_feeds = new GLib.GenericArray<Gst.PreviewFeed> ();
        private Gst.PreviewFeed? screen_preview_feed = null;
        private CaptureStream? active_stream = null;
        private Gst.Recorder? recorder = null;

        public bool is_recording { get; private set; default = false; }
        public bool is_paused { get; private set; default = false; }
        public uint64 elapsed_seconds { get; private set; default = 0; }

        private bool is_counting_down = false;
        private uint countdown_source_id = 0;
        private uint osd_timer_id = 0;
        private bool guard_cam_toggle = false;
        private bool guard_cd_dd = false;
        private bool guard_scale = false;
        private string gpu_summary = "Software CPU";

        public signal void control_state_changed ();

        public Window (Gtk.Application app) {
            Object (
                application: app,
                title: "RecGo",
                default_width: 1180,
                default_height: 740
            );
        }

        construct {
            settings = new GLib.Settings (Config.APP_ID);
            device_manager = new Gst.DeviceManager ();
            Gst.detect_gpu_acceleration (out gpu_summary);
            supported_codecs = Gst.get_supported_codecs ();
            selected_codec_idx = find_saved_codec_index ();

            build_ui ();
            load_settings_into_ui ();
            start_osd_timer ();

            if (cam_toggle.get_active ()) {
                start_camera_previews ();
            }

            device_manager.devices_changed.connect (() => {
                if (!is_recording && cam_toggle.get_active ()) {
                    start_camera_previews ();
                }
            });

            close_request.connect (() => {
                cleanup_on_close ();
                return false;
            });

            maybe_run_screenshot_mode ();
        }

        private int find_saved_codec_index () {
            string saved_id = settings.get_string ("video-codec");
            for (uint i = 0; i < supported_codecs.length; i++) {
                if (supported_codecs.get (i).id == saved_id) {
                    return (int) i;
                }
            }
            return 0;
        }

        private void build_ui () {
            toast_overlay = new Adw.ToastOverlay ();
            var toolbar_view = new Adw.ToolbarView ();

            string codec_desc = (supported_codecs.length > 0 && selected_codec_idx < supported_codecs.length)
                ? supported_codecs.get ((uint) selected_codec_idx).display_name
                : "H.264";

            status_label = new Gtk.Label ("Ready to record");
            status_label.add_css_class ("dim-label");
            status_label.set_ellipsize (Pango.EllipsizeMode.END);
            status_label.set_max_width_chars (36);

            // HeaderBar
            var header = new Adw.HeaderBar ();
            win_title = new Adw.WindowTitle ("RecGo", "%s · %s".printf (gpu_summary, codec_desc));
            header.set_title_widget (win_title);

            source_dropdown = new Gtk.DropDown.from_strings ({
                "Screen / Monitor",
                "Application Window"
            });
            source_dropdown.set_tooltip_text ("Capture Target");
            header.pack_start (source_dropdown);

            layout_dropdown = new Gtk.DropDown.from_strings ({
                "Desktop + PiP",
                "Side by Side",
                "Screen Only",
                "Camera Only"
            });
            layout_dropdown.set_tooltip_text ("Recording Layout");
            header.pack_start (layout_dropdown);

            shape_dropdown = new Gtk.DropDown.from_strings ({
                "Circle (Shadow)",
                "Square (Shadow)",
                "Rectangle (Shadow)"
            });
            shape_dropdown.set_tooltip_text ("Webcam Enclosure Shape");

            countdown_dropdown = new Gtk.DropDown.from_strings ({
                "Countdown: 3s",
                "Countdown: 5s",
                "Countdown: 10s",
                "No Delay",
                "Custom..."
            });
            countdown_dropdown.set_tooltip_text ("Countdown timer before recording");

            var app_menu = new GLib.Menu ();
            app_menu.append ("Preferences & Encoders", "win.preferences");
            app_menu.append ("About RecGo", "win.about");
            var menu_btn = new Gtk.MenuButton ();
            menu_btn.set_icon_name ("open-menu-symbolic");
            menu_btn.set_tooltip_text ("Main Menu");
            menu_btn.set_menu_model (app_menu);
            header.pack_end (menu_btn);

            var framing_box = new Gtk.Box (Gtk.Orientation.VERTICAL, 10);
            framing_box.set_margin_top (12);
            framing_box.set_margin_bottom (12);
            framing_box.set_margin_start (12);
            framing_box.set_margin_end (12);

            var shape_lbl = new Gtk.Label ("Webcam Shape");
            shape_lbl.set_xalign (0.0f);
            shape_lbl.add_css_class ("heading");
            framing_box.append (shape_lbl);
            framing_box.append (shape_dropdown);

            var cd_lbl = new Gtk.Label ("Recording Delay");
            cd_lbl.set_xalign (0.0f);
            cd_lbl.add_css_class ("heading");
            cd_lbl.set_margin_top (4);
            framing_box.append (cd_lbl);
            framing_box.append (countdown_dropdown);

            var framing_popover = new Gtk.Popover ();
            framing_popover.set_child (framing_box);
            var framing_btn = new Gtk.MenuButton ();
            framing_btn.set_icon_name ("preferences-other-symbolic");
            framing_btn.set_tooltip_text ("Webcam Shape & Countdown Timer");
            framing_btn.set_popover (framing_popover);
            header.pack_end (framing_btn);

            cam_toggle = new Gtk.ToggleButton ();
            cam_toggle.set_icon_name ("camera-web-symbolic");
            cam_toggle.set_tooltip_text ("Toggle Live Webcam Feed");
            cam_toggle.set_active (true);
            header.pack_end (cam_toggle);

            mic_toggle = new Gtk.ToggleButton ();
            mic_toggle.set_icon_name ("audio-input-microphone-symbolic");
            mic_toggle.set_tooltip_text ("Toggle Microphone Audio Recording");
            header.pack_end (mic_toggle);

            register_window_actions ();
            toolbar_view.add_top_bar (header);

            // Center Preview Stage
            preview = new PreviewWidget ();
            var content_box = new Gtk.Box (Gtk.Orientation.VERTICAL, 0);
            content_box.append (preview);
            toolbar_view.set_content (content_box);

            // Bottom Action Bar
            var action_bar = new Gtk.ActionBar ();

            btn_record = new Gtk.Button ();
            var rec_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8);
            rec_icon = new Gtk.Image.from_icon_name ("media-record-symbolic");
            rec_icon.set_pixel_size (18);
            rec_label = new Gtk.Label ("Record");
            rec_label.add_css_class ("camera-btn-label");
            rec_box.append (rec_icon);
            rec_box.append (rec_label);
            btn_record.set_child (rec_box);
            btn_record.set_css_classes ({ "pill", "camera-record-btn" });
            btn_record.set_tooltip_text ("Start Recording (Camera Shutter)");

            btn_pause = new Gtk.ToggleButton ();
            var pause_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8);
            var pause_icon = new Gtk.Image.from_icon_name ("media-playback-pause-symbolic");
            pause_icon.set_pixel_size (18);
            var pause_label = new Gtk.Label ("Pause");
            pause_label.add_css_class ("camera-btn-label");
            pause_box.append (pause_icon);
            pause_box.append (pause_label);
            btn_pause.set_child (pause_box);
            btn_pause.set_css_classes ({ "pill", "camera-pause-btn" });
            btn_pause.set_tooltip_text ("Pause / Resume");
            btn_pause.set_sensitive (false);

            btn_stop = new Gtk.Button ();
            var stop_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8);
            var stop_icon = new Gtk.Image.from_icon_name ("media-playback-stop-symbolic");
            stop_icon.set_pixel_size (18);
            var stop_label = new Gtk.Label ("Stop");
            stop_label.add_css_class ("camera-btn-label");
            stop_box.append (stop_icon);
            stop_box.append (stop_label);
            btn_stop.set_child (stop_box);
            btn_stop.set_css_classes ({ "pill", "camera-stop-btn" });
            btn_stop.set_tooltip_text ("Stop & Save Recording");
            btn_stop.set_sensitive (false);

            var osd_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8);
            osd_box.add_css_class ("camera-osd-badge");
            osd_dot = new Gtk.Label ("●");
            osd_dot.set_css_classes ({ "camera-osd-dot", "standby" });
            osd_text = new Gtk.Label ("STBY  00:00:00");
            osd_text.add_css_class ("camera-osd-time");
            osd_box.append (osd_dot);
            osd_box.append (osd_text);

            btn_preview_screen = new Gtk.Button ();
            var prev_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8);
            var prev_icon = new Gtk.Image.from_icon_name ("video-display-symbolic");
            prev_icon.set_pixel_size (18);
            var prev_label = new Gtk.Label ("Select Source");
            prev_label.add_css_class ("camera-btn-label");
            prev_box.append (prev_icon);
            prev_box.append (prev_label);
            btn_preview_screen.set_child (prev_box);
            btn_preview_screen.set_css_classes ({ "pill", "camera-preview-btn" });
            btn_preview_screen.set_tooltip_text ("Select Desktop or Window to Preview (ScreenCast)");

            var left_status_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 12);
            left_status_box.append (osd_box);
            left_status_box.append (status_label);
            action_bar.pack_start (left_status_box);

            var box_controls = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 12);
            box_controls.set_halign (Gtk.Align.CENTER);
            box_controls.append (btn_preview_screen);
            box_controls.append (btn_record);
            box_controls.append (btn_pause);
            box_controls.append (btn_stop);
            action_bar.set_center_widget (box_controls);

            var size_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8);
            var size_label = new Gtk.Label ("Webcam Size");
            size_label.add_css_class ("dim-label");
            size_scale = new Gtk.Scale.with_range (Gtk.Orientation.HORIZONTAL, 0.10, 0.65, 0.01);
            size_scale.set_size_request (150, -1);
            size_scale.set_tooltip_text ("Resize Webcam Overlay");
            size_box.append (size_label);
            size_box.append (size_scale);
            action_bar.pack_end (size_box);

            toolbar_view.add_bottom_bar (action_bar);
            toast_overlay.set_child (toolbar_view);
            set_content (toast_overlay);

            connect_ui_signals ();
        }

        private void register_window_actions () {
            var pref_action = new GLib.SimpleAction ("preferences", null);
            pref_action.activate.connect (() => {
                var dialog = new PreferencesDialog (settings, device_manager, selected_codec_idx);
                dialog.codec_changed.connect ((new_idx) => {
                    selected_codec_idx = new_idx;
                    if (new_idx >= 0 && (uint) new_idx < supported_codecs.length) {
                        unowned var c = supported_codecs.get ((uint) new_idx);
                        settings.set_string ("video-codec", c.id);
                        settings.set_string ("container-format", Gst.get_extension_for_container (c.container));
                        toast_overlay.add_toast (new Adw.Toast ("Active encoder: %s".printf (c.display_name)));
                        status_label.set_text ("Encoder: %s".printf (c.display_name));
                        win_title.set_subtitle ("%s · %s".printf (gpu_summary, c.display_name));
                    }
                });
                dialog.countdown_changed.connect ((secs, custom_secs) => {
                    settings.set_uint ("countdown-seconds", secs);
                    settings.set_uint ("custom-countdown-seconds", custom_secs);
                    sync_countdown_dropdown (secs);
                });
                dialog.devices_selection_changed.connect (() => {
                    if (!is_recording && cam_toggle.get_active ()) {
                        start_camera_previews ();
                    }
                });
                dialog.present (this);
            });
            add_action (pref_action);

            var about_action = new GLib.SimpleAction ("about", null);
            about_action.activate.connect (() => {
                var about = new Adw.AboutDialog ();
                about.set_application_name ("RecGo");
                about.set_application_icon ("io.github.Antux912012.RecGo");
                about.set_developer_name ("Antonio De Gaetano");
                about.set_version (Config.VERSION);
                about.set_comments ("High-performance GPU-accelerated multi-cam and desktop recorder for GNOME 51.");
                about.set_website ("https://github.com/Antux912012/RecGo");
                about.set_issue_url ("https://github.com/Antux912012/RecGo/issues");
                about.set_license_type (Gtk.License.GPL_3_0);
                about.set_copyright ("© 2026 Antonio De Gaetano");
                about.present (this);
            });
            add_action (about_action);
        }

        private void load_settings_into_ui () {
            string layout_str = settings.get_string ("layout-preset");
            uint layout_idx = 0;
            if (layout_str == "side-by-side") layout_idx = 1;
            else if (layout_str == "screen-only") layout_idx = 2;
            else if (layout_str == "camera-only") layout_idx = 3;
            layout_dropdown.set_selected (layout_idx);
            preview.layout = (Gst.LayoutPreset) layout_idx;

            string shape_str = settings.get_string ("webcam-shape");
            uint shape_idx = 0;
            if (shape_str == "square") shape_idx = 1;
            else if (shape_str == "rectangle") shape_idx = 2;
            shape_dropdown.set_selected (shape_idx);
            preview.shape = shape_from_index (shape_idx);

            preview.overlay_x = settings.get_double ("webcam-pos-x");
            preview.overlay_y = settings.get_double ("webcam-pos-y");
            double sc = settings.get_double ("webcam-scale").clamp (0.10, 0.65);
            preview.overlay_scale = sc;
            guard_scale = true;
            size_scale.set_value (sc);
            guard_scale = false;

            mic_toggle.set_active (settings.get_boolean ("mic-enabled"));
            sync_countdown_dropdown (settings.get_uint ("countdown-seconds"));
        }

        private static Gst.WebcamShape shape_from_index (uint idx) {
            switch (idx) {
            case 1:  return Gst.WebcamShape.SQUARE;
            case 2:  return Gst.WebcamShape.RECTANGLE;
            default: return Gst.WebcamShape.CIRCLE;
            }
        }

        private void sync_countdown_dropdown (uint secs) {
            uint idx = 4;
            if (secs == 3) idx = 0;
            else if (secs == 5) idx = 1;
            else if (secs == 10) idx = 2;
            else if (secs == 0) idx = 3;

            guard_cd_dd = true;
            countdown_dropdown.set_selected (idx);
            guard_cd_dd = false;
        }

        private void connect_ui_signals () {
            layout_dropdown.notify["selected"].connect (() => {
                uint sel = layout_dropdown.get_selected ();
                preview.layout = (Gst.LayoutPreset) sel;
                string[] keys = { "desktop-pip", "side-by-side", "screen-only", "camera-only" };
                settings.set_string ("layout-preset", keys[sel < keys.length ? sel : 0]);
            });

            shape_dropdown.notify["selected"].connect (() => {
                uint sel = shape_dropdown.get_selected ();
                preview.shape = shape_from_index (sel);
                string[] keys = { "circle", "square", "rectangle" };
                settings.set_string ("webcam-shape", keys[sel < keys.length ? sel : 0]);
            });

            size_scale.value_changed.connect (() => {
                if (guard_scale) return;
                double val = size_scale.get_value ();
                preview.overlay_scale = val;
                preview.queue_draw ();
                settings.set_double ("webcam-scale", val);
            });

            preview.overlay_changed.connect (() => {
                settings.set_double ("webcam-pos-x", preview.overlay_x);
                settings.set_double ("webcam-pos-y", preview.overlay_y);
                settings.set_double ("webcam-scale", preview.overlay_scale);
                guard_scale = true;
                size_scale.set_value (preview.overlay_scale);
                guard_scale = false;
            });

            countdown_dropdown.notify["selected"].connect (() => {
                if (guard_cd_dd) return;
                uint sel = countdown_dropdown.get_selected ();
                switch (sel) {
                case 0:
                    settings.set_uint ("countdown-seconds", 3);
                    toast_overlay.add_toast (new Adw.Toast ("Countdown: 3 seconds"));
                    break;
                case 1:
                    settings.set_uint ("countdown-seconds", 5);
                    toast_overlay.add_toast (new Adw.Toast ("Countdown: 5 seconds"));
                    break;
                case 2:
                    settings.set_uint ("countdown-seconds", 10);
                    toast_overlay.add_toast (new Adw.Toast ("Countdown: 10 seconds"));
                    break;
                case 3:
                    settings.set_uint ("countdown-seconds", 0);
                    toast_overlay.add_toast (new Adw.Toast ("Countdown disabled"));
                    break;
                default:
                    open_custom_countdown_dialog ();
                    break;
                }
            });

            cam_toggle.toggled.connect (() => {
                if (guard_cam_toggle) return;
                if (cam_toggle.get_active ()) {
                    start_camera_previews ();
                    if (webcam_feeds.length > 0) {
                        toast_overlay.add_toast (new Adw.Toast ("Webcam preview enabled (%u)".printf (webcam_feeds.length)));
                    } else {
                        guard_cam_toggle = true;
                        cam_toggle.set_active (false);
                        guard_cam_toggle = false;
                        toast_overlay.add_toast (new Adw.Toast ("No webcam device available"));
                    }
                } else {
                    stop_camera_previews ();
                    toast_overlay.add_toast (new Adw.Toast ("Webcam preview disabled"));
                }
            });

            mic_toggle.toggled.connect (() => {
                bool on = mic_toggle.get_active ();
                settings.set_boolean ("mic-enabled", on);
                toast_overlay.add_toast (new Adw.Toast (on ? "Microphone enabled" : "Microphone muted"));
            });

            btn_preview_screen.clicked.connect (() => {
                select_screen_source.begin ();
            });

            btn_record.clicked.connect (() => {
                on_record_clicked ();
            });

            btn_pause.toggled.connect (() => {
                on_pause_toggled ();
            });

            btn_stop.clicked.connect (() => {
                stop_recording_action ();
            });
        }

        private void open_custom_countdown_dialog () {
            var dialog = new Adw.PreferencesDialog ();
            dialog.set_title ("Custom Countdown");
            var page = new Adw.PreferencesPage ();
            var grp = new Adw.PreferencesGroup ();
            grp.set_title ("Custom Countdown Delay");
            grp.set_description ("Set duration up to 5 minutes (300 seconds)");
            var spin = new Adw.SpinRow.with_range (1.0, 300.0, 1.0);
            spin.set_title ("Duration (seconds)");
            spin.set_value ((double) settings.get_uint ("custom-countdown-seconds").clamp (1, 300));
            grp.add (spin);
            page.add (grp);
            dialog.add (page);
            dialog.closed.connect (() => {
                uint val = (uint) spin.get_value ();
                settings.set_uint ("custom-countdown-seconds", val);
                settings.set_uint ("countdown-seconds", val);
                toast_overlay.add_toast (new Adw.Toast ("Countdown set to %u seconds".printf (val)));
            });
            dialog.present (this);
        }

        private GLib.GenericArray<Gst.DeviceInfo> resolve_selected_devices (string key,
                                                                            GLib.GenericArray<Gst.DeviceInfo> available,
                                                                            bool pick_default_for_mic) {
            var result = new GLib.GenericArray<Gst.DeviceInfo> ();
            string[] saved = settings.get_strv (key);

            if (saved.length == 0) {
                if (available.length > 0) {
                    if (pick_default_for_mic) {
                        for (uint i = 0; i < available.length; i++) {
                            if (available.get (i).is_default) {
                                result.add (available.get (i).copy ());
                                return result;
                            }
                        }
                    }
                    result.add (available.get (0).copy ());
                }
                return result;
            }

            for (uint i = 0; i < available.length; i++) {
                unowned var dev = available.get (i);
                foreach (unowned string id in saved) {
                    if (dev.id == id) {
                        result.add (dev.copy ());
                        break;
                    }
                }
            }
            return result;
        }

        private void start_camera_previews () {
            stop_camera_previews ();
            var selected = resolve_selected_devices ("enabled-cameras", device_manager.get_cameras (), false);
            preview.set_webcam_count (selected.length);

            for (uint i = 0; i < selected.length; i++) {
                uint cam_idx = i;
                unowned var dev = selected.get (i);
                try {
                    var feed = Gst.PreviewFeed.new_camera (dev);
                    if (feed != null) {
                        feed.frame_ready.connect ((tex) => {
                            preview.set_webcam_texture (cam_idx, tex);
                        });
                        webcam_feeds.add (feed);
                    }
                } catch (GLib.Error e) {
                    warning ("Webcam preview error (%s): %s", dev.id, e.message);
                }
            }

            if (webcam_feeds.length == 0) {
                guard_cam_toggle = true;
                cam_toggle.set_active (false);
                guard_cam_toggle = false;
            }
        }

        private void stop_camera_previews () {
            for (uint i = 0; i < webcam_feeds.length; i++) {
                webcam_feeds.get (i).stop ();
            }
            webcam_feeds.remove_range (0, webcam_feeds.length);
            preview.clear_webcam_textures ();
        }

        private void stop_screen_preview () {
            if (screen_preview_feed != null) {
                screen_preview_feed.stop ();
                screen_preview_feed = null;
            }
        }

        private async void select_screen_source () {
            uint32 source_type = (source_dropdown.get_selected () == 0) ? 1 : 2;
            status_label.set_text ("Selecting desktop or window for live preview...");
            try {
                var stream = yield PortalClient.start_screencast (source_type);
                stop_screen_preview ();
                var feed = Gst.PreviewFeed.new_screen (stream.fd, stream.node_id);
                if (feed != null) {
                    feed.frame_ready.connect ((tex) => {
                        preview.set_screen_texture (tex);
                    });
                    screen_preview_feed = feed;
                }
                active_stream = stream;
                status_label.set_text ("Live desktop preview active (%dx%d)".printf (stream.width, stream.height));
                toast_overlay.add_toast (new Adw.Toast ("Live desktop preview active"));
            } catch (GLib.Error e) {
                status_label.set_text ("Screen selection cancelled");
            }
        }

        public void start_recording_action () {
            if (is_recording || is_counting_down)
                return;
            on_record_clicked ();
        }

        public void toggle_pause_action () {
            if (!is_recording)
                return;
            btn_pause.set_active (!btn_pause.get_active ());
        }

        public void cycle_webcam_shape_action () {
            uint next = (shape_dropdown.get_selected () + 1) % 3;
            shape_dropdown.set_selected (next);
        }

        public string get_control_state () {
            if (!is_recording) return "idle";
            return is_paused ? "paused" : "recording";
        }

        private void reset_record_button_ui () {
            rec_icon.set_from_icon_name ("media-record-symbolic");
            rec_label.set_text ("Record");
            btn_record.remove_css_class ("camera-cancel-btn");
            btn_record.add_css_class ("camera-record-btn");
        }

        private void cancel_countdown () {
            if (countdown_source_id != 0) {
                GLib.Source.remove (countdown_source_id);
                countdown_source_id = 0;
            }
            is_counting_down = false;
            preview.set_countdown_value (-1);
            reset_record_button_ui ();
            osd_text.set_text ("STBY  00:00:00");
            osd_dot.set_css_classes ({ "camera-osd-dot", "standby" });
        }

        private void on_record_clicked () {
            if (is_counting_down) {
                cancel_countdown ();
                toast_overlay.add_toast (new Adw.Toast ("Countdown cancelled"));
                return;
            }

            uint cd = settings.get_uint ("countdown-seconds");
            if (cd == 0) {
                launch_recording.begin ();
                return;
            }

            is_counting_down = true;
            uint remaining = cd;
            preview.set_countdown_value ((int) remaining);
            rec_icon.set_from_icon_name ("window-close-symbolic");
            rec_label.set_text ("Cancel (%us)".printf (remaining));
            btn_record.remove_css_class ("camera-record-btn");
            btn_record.add_css_class ("camera-cancel-btn");
            osd_text.set_text ("COUNTDOWN  %02u:%02u".printf (remaining / 60, remaining % 60));
            osd_dot.set_css_classes ({ "camera-osd-dot", "paused" });

            countdown_source_id = GLib.Timeout.add_seconds (1, () => {
                if (remaining > 1) {
                    remaining--;
                    preview.set_countdown_value ((int) remaining);
                    rec_label.set_text ("Cancel (%us)".printf (remaining));
                    osd_text.set_text ("COUNTDOWN  %02u:%02u".printf (remaining / 60, remaining % 60));
                    return GLib.Source.CONTINUE;
                }
                countdown_source_id = 0;
                is_counting_down = false;
                preview.set_countdown_value (-1);
                reset_record_button_ui ();
                osd_dot.set_css_classes ({ "camera-osd-dot", "recording" });
                launch_recording.begin ();
                return GLib.Source.REMOVE;
            });
        }

        private async void launch_recording () {
            var layout = (Gst.LayoutPreset) layout_dropdown.get_selected ();
            var shape = shape_from_index (shape_dropdown.get_selected ());

            Gst.VideoCodec codec = Gst.VideoCodec.H264;
            Gst.ContainerFormat container = Gst.ContainerFormat.MP4;
            if (selected_codec_idx >= 0 && (uint) selected_codec_idx < supported_codecs.length) {
                unowned var c = supported_codecs.get ((uint) selected_codec_idx);
                codec = c.codec;
                container = c.container;
            }

            string? videos_dir = GLib.Environment.get_user_special_dir (GLib.UserDirectory.VIDEOS);
            if (videos_dir == null) videos_dir = "/tmp";
            var now = new GLib.DateTime.now_local ();
            string ts = now.format ("%Y-%m-%d_%H-%M-%S") ?? "output";
            string ext = Gst.get_extension_for_container (container);
            string out_path = GLib.Path.build_filename (videos_dir, "RecGo-%s.%s".printf (ts, ext));

            if (layout != Gst.LayoutPreset.CAMERA_ONLY && active_stream == null) {
                status_label.set_text ("Requesting screen access...");
                uint32 source_type = (source_dropdown.get_selected () == 0) ? 1 : 2;
                try {
                    active_stream = yield PortalClient.start_screencast (source_type);
                } catch (GLib.Error e) {
                    status_label.set_text ("Screen selection cancelled or failed");
                    toast_overlay.add_toast (new Adw.Toast ("ScreenCast portal: %s".printf (e.message)));
                    return;
                }
            }

            // Synchronously release preview pipelines before opening devices for recording (prevents EBUSY)
            stop_screen_preview ();
            if (layout != Gst.LayoutPreset.SCREEN_ONLY) {
                stop_camera_previews ();
            }

            var cameras = (layout == Gst.LayoutPreset.SCREEN_ONLY)
                ? new GLib.GenericArray<Gst.DeviceInfo> ()
                : resolve_selected_devices ("enabled-cameras", device_manager.get_cameras (), false);

            var mics = resolve_selected_devices ("enabled-microphones", device_manager.get_microphones (), true);

            Gst.RecordConfig cfg = {};
            cfg.screen_fd = (active_stream != null) ? active_stream.fd : -1;
            cfg.screen_node_id = (active_stream != null) ? active_stream.node_id : 0;
            cfg.screen_width = (active_stream != null) ? (uint) active_stream.width : 1920;
            cfg.screen_height = (active_stream != null) ? (uint) active_stream.height : 1080;
            cfg.cameras = cameras;
            cfg.microphones = mics;
            cfg.mic_enabled = mic_toggle.get_active ();
            cfg.layout = layout;
            cfg.shape = shape;
            cfg.overlay_x = preview.overlay_x;
            cfg.overlay_y = preview.overlay_y;
            cfg.overlay_scale = preview.overlay_scale;
            cfg.codec = codec;
            cfg.container = container;
            cfg.prefer_gpu_compositing = settings.get_boolean ("gpu-compositing");
            cfg.output_path = out_path;

            try {
                recorder = new Gst.Recorder (cfg);
                var prev_feed = recorder.get_preview_feed ();
                if (prev_feed != null) {
                    prev_feed.frame_ready.connect ((tex) => {
                        preview.set_screen_texture (tex);
                    });
                }
                recorder.stopped.connect (() => {
                    on_recorder_stopped ();
                });
                recorder.error.connect ((msg) => {
                    toast_overlay.add_toast (new Adw.Toast ("Recorder error: %s".printf (msg)));
                });

                recorder.start ();

                is_recording = true;
                is_paused = false;
                elapsed_seconds = 0;
                osd_text.set_text ("REC  00:00:00");
                osd_dot.set_css_classes ({ "camera-osd-dot", "recording" });
                preview.set_recording_state (true, false);
                btn_preview_screen.set_sensitive (false);
                btn_record.set_sensitive (false);
                btn_pause.set_sensitive (true);
                btn_stop.set_sensitive (true);
                string enc_desc = recorder.get_encoder_description ();
                status_label.set_text ("Recording [%s] -> %s".printf (enc_desc, out_path));
                toast_overlay.add_toast (new Adw.Toast ("Recording with %s".printf (enc_desc)));
                control_state_changed ();
            } catch (GLib.Error e) {
                status_label.set_text ("Pipeline error: %s".printf (e.message));
                toast_overlay.add_toast (new Adw.Toast ("GStreamer error: %s".printf (e.message)));
                recorder = null;
                if (cam_toggle.get_active ()) {
                    start_camera_previews ();
                }
            }
        }

        private void on_pause_toggled () {
            if (!is_recording || recorder == null)
                return;
            bool paused = btn_pause.get_active ();
            is_paused = paused;
            preview.set_recording_state (true, paused);
            try {
                if (paused) recorder.pause ();
                else recorder.resume ();
            } catch (GLib.Error e) {
                warning ("Pause/Resume failed: %s", e.message);
            }
            status_label.set_text (paused ? "Recording Paused" : "Recording Resumed");
            toast_overlay.add_toast (new Adw.Toast (paused ? "Recording paused" : "Recording resumed"));
            control_state_changed ();
        }

        public void stop_recording_action () {
            if (is_counting_down) {
                cancel_countdown ();
                toast_overlay.add_toast (new Adw.Toast ("Countdown cancelled"));
                return;
            }
            if (!is_recording || recorder == null)
                return;

            status_label.set_text ("Finalizing recording file...");
            btn_stop.set_sensitive (false);
            btn_pause.set_sensitive (false);
            recorder.stop_async ();
        }

        private void on_recorder_stopped () {
            recorder = null;
            is_recording = false;
            is_paused = false;
            elapsed_seconds = 0;
            osd_text.set_text ("STBY  00:00:00");
            osd_dot.set_css_classes ({ "camera-osd-dot", "standby" });
            preview.set_recording_state (false, false);
            btn_pause.set_active (false);
            btn_record.set_sensitive (true);
            btn_preview_screen.set_sensitive (true);

            if (cam_toggle.get_active ()) {
                start_camera_previews ();
            }
            if (active_stream != null) {
                try {
                    var feed = Gst.PreviewFeed.new_screen (active_stream.fd, active_stream.node_id);
                    if (feed != null) {
                        feed.frame_ready.connect ((tex) => {
                            preview.set_screen_texture (tex);
                        });
                        screen_preview_feed = feed;
                    }
                } catch (GLib.Error e) {
                    warning ("Failed to restore screen preview: %s", e.message);
                }
            }

            status_label.set_text ("Recording saved to Videos folder");
            toast_overlay.add_toast (new Adw.Toast ("Video successfully saved to Videos folder"));
            control_state_changed ();
        }

        private void start_osd_timer () {
            osd_timer_id = GLib.Timeout.add_seconds (1, () => {
                if (is_recording) {
                    if (!is_paused) {
                        elapsed_seconds++;
                        uint hrs = (uint) (elapsed_seconds / 3600);
                        uint mins = (uint) ((elapsed_seconds % 3600) / 60);
                        uint secs = (uint) (elapsed_seconds % 60);
                        osd_text.set_text ("REC  %02u:%02u:%02u".printf (hrs, mins, secs));
                        osd_dot.set_css_classes ({ "camera-osd-dot", "recording" });
                    } else {
                        uint hrs = (uint) (elapsed_seconds / 3600);
                        uint mins = (uint) ((elapsed_seconds % 3600) / 60);
                        uint secs = (uint) (elapsed_seconds % 60);
                        osd_text.set_text ("PAUSE  %02u:%02u:%02u".printf (hrs, mins, secs));
                        osd_dot.set_css_classes ({ "camera-osd-dot", "paused" });
                    }
                    control_state_changed ();
                }
                return GLib.Source.CONTINUE;
            });
        }

        private void cleanup_on_close () {
            if (osd_timer_id != 0) {
                GLib.Source.remove (osd_timer_id);
                osd_timer_id = 0;
            }
            if (countdown_source_id != 0) {
                GLib.Source.remove (countdown_source_id);
                countdown_source_id = 0;
            }
            stop_camera_previews ();
            stop_screen_preview ();
            if (recorder != null) {
                recorder.stop_async ();
                recorder = null;
            }
        }

        private void maybe_run_screenshot_mode () {
            string? shot_dir = GLib.Environment.get_variable ("RECGO_SCREENSHOT_DIR");
            if (shot_dir == null || shot_dir == "")
                return;

            GLib.DirUtils.create_with_parents (shot_dir, 0755);
            GLib.Timeout.add (800, () => {
                save_widget_png (this, GLib.Path.build_filename (shot_dir, "01-studio-main.png"));
                preview.set_countdown_value (3);
                GLib.Timeout.add (250, () => {
                    save_widget_png (this, GLib.Path.build_filename (shot_dir, "02-studio-countdown.png"));
                    preview.set_countdown_value (-1);
                    activate_action ("win.preferences", null);
                    GLib.Timeout.add (650, () => {
                        save_widget_png (this, GLib.Path.build_filename (shot_dir, "03-preferences-recording.png"));
                        close ();
                        return GLib.Source.REMOVE;
                    });
                    return GLib.Source.REMOVE;
                });
                return GLib.Source.REMOVE;
            });
        }

        private static void save_widget_png (Gtk.Widget widget, string path) {
            var paintable = new Gtk.WidgetPaintable (widget);
            float w = float.max (1.0f, (float) widget.get_width ());
            float h = float.max (1.0f, (float) widget.get_height ());
            var snapshot = new Gtk.Snapshot ();
            paintable.snapshot (snapshot, (double) w, (double) h);
            var node = snapshot.to_node ();
            var native = widget.get_native ();
            if (node != null && native != null) {
                var renderer = native.get_renderer ();
                if (renderer != null) {
                    var bounds = Graphene.Rect ().init (0, 0, w, h);
                    var tex = renderer.render_texture (node, bounds);
                    tex.save_to_png (path);
                }
            }
        }
    }
}
