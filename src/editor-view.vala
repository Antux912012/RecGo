namespace RecGo {

    public class EditorView : Gtk.Box {
        public signal void toast_requested (string message);

        private SafeSettings settings;
        private Gtk.ListBox clip_list;
        private Gtk.Picture video_picture;
        private Gtk.MediaFile? media_file = null;
        private TimelineWidget timeline;

        private Gtk.Button btn_play_pause;
        private Gtk.Image play_icon;
        private Gtk.Label timecode_label;
        private Gtk.Label trim_label;
        private Gtk.Button btn_export;
        private Gtk.ProgressBar export_progress;

        private GLib.GenericArray<string> clip_paths = new GLib.GenericArray<string> ();
        private string? active_clip_path = null;
        private Gst.Editor editor;
        private bool is_exporting = false;

        public EditorView (SafeSettings settings) {
            Object (orientation: Gtk.Orientation.VERTICAL, spacing: 0);
            this.settings = settings;
            this.editor = new Gst.Editor ();

            editor.progress.connect ((frac) => {
                export_progress.set_fraction (frac);
            });
            editor.finished.connect ((success, err_msg) => {
                is_exporting = false;
                btn_export.set_sensitive (active_clip_path != null);
                export_progress.set_visible (false);
                if (success) {
                    toast_requested ("Exported edited video to Videos folder");
                } else {
                    toast_requested ("Export failed: %s".printf (err_msg ?? "unknown error"));
                }
            });

            build_ui ();
        }

        private void build_ui () {
            var top_split = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 0);
            top_split.set_vexpand (true);

            // Left: Project Bin (Kdenlive-inspired)
            var bin_box = new Gtk.Box (Gtk.Orientation.VERTICAL, 8);
            bin_box.set_size_request (240, -1);
            bin_box.set_margin_top (10);
            bin_box.set_margin_bottom (10);
            bin_box.set_margin_start (10);
            bin_box.set_margin_end (10);

            var bin_header = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8);
            var bin_title = new Gtk.Label ("Project Bin");
            bin_title.add_css_class ("heading");
            bin_title.set_xalign (0.0f);
            bin_title.set_hexpand (true);
            var add_btn = new Gtk.Button.from_icon_name ("list-add-symbolic");
            add_btn.set_tooltip_text ("Add Video Clip");
            add_btn.clicked.connect (() => open_file_picker ());
            bin_header.append (bin_title);
            bin_header.append (add_btn);
            bin_box.append (bin_header);

            clip_list = new Gtk.ListBox ();
            clip_list.add_css_class ("boxed-list");
            clip_list.set_selection_mode (Gtk.SelectionMode.SINGLE);
            clip_list.row_selected.connect ((row) => {
                if (row != null) {
                    int idx = row.get_index ();
                    if (idx >= 0 && (uint) idx < clip_paths.length) {
                        load_clip (clip_paths.get ((uint) idx));
                    }
                }
            });

            var bin_scroll = new Gtk.ScrolledWindow ();
            bin_scroll.set_vexpand (true);
            bin_scroll.set_child (clip_list);
            bin_box.append (bin_scroll);

            top_split.append (bin_box);
            top_split.append (new Gtk.Separator (Gtk.Orientation.VERTICAL));

            // Right: Monitor + Transport
            var monitor_box = new Gtk.Box (Gtk.Orientation.VERTICAL, 8);
            monitor_box.set_hexpand (true);
            monitor_box.set_vexpand (true);
            monitor_box.set_margin_top (10);
            monitor_box.set_margin_bottom (8);
            monitor_box.set_margin_start (10);
            monitor_box.set_margin_end (10);

            video_picture = new Gtk.Picture ();
            video_picture.set_hexpand (true);
            video_picture.set_vexpand (true);
            video_picture.set_content_fit (Gtk.ContentFit.CONTAIN);
            monitor_box.append (video_picture);

            var transport = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8);
            transport.set_halign (Gtk.Align.FILL);

            btn_play_pause = new Gtk.Button ();
            play_icon = new Gtk.Image.from_icon_name ("media-playback-start-symbolic");
            btn_play_pause.set_child (play_icon);
            btn_play_pause.set_tooltip_text ("Play / Pause");
            btn_play_pause.set_sensitive (false);
            btn_play_pause.clicked.connect (() => toggle_playback ());
            transport.append (btn_play_pause);

            var back_btn = new Gtk.Button.with_label ("-1s");
            back_btn.set_tooltip_text ("Step back 1 second");
            back_btn.clicked.connect (() => step_seconds (-1));
            transport.append (back_btn);

            var fwd_btn = new Gtk.Button.with_label ("+1s");
            fwd_btn.set_tooltip_text ("Step forward 1 second");
            fwd_btn.clicked.connect (() => step_seconds (1));
            transport.append (fwd_btn);

            transport.append (new Gtk.Separator (Gtk.Orientation.VERTICAL));

            var in_btn = new Gtk.Button.with_label ("Set In");
            in_btn.set_tooltip_text ("Trim Start to Playhead");
            in_btn.clicked.connect (() => timeline.set_in_point_at_playhead ());
            transport.append (in_btn);

            var out_btn = new Gtk.Button.with_label ("Set Out");
            out_btn.set_tooltip_text ("Trim End to Playhead");
            out_btn.clicked.connect (() => timeline.set_out_point_at_playhead ());
            transport.append (out_btn);

            var split_btn = new Gtk.Button.with_label ("Split");
            split_btn.set_tooltip_text ("Mark Razor Split at Playhead");
            split_btn.clicked.connect (() => timeline.add_split_at_playhead ());
            transport.append (split_btn);

            var reset_btn = new Gtk.Button.with_label ("Reset");
            reset_btn.set_tooltip_text ("Reset Trim");
            reset_btn.clicked.connect (() => timeline.reset_trim ());
            transport.append (reset_btn);

            timecode_label = new Gtk.Label ("00:00 / 00:00");
            timecode_label.add_css_class ("numeric");
            timecode_label.add_css_class ("dim-label");
            timecode_label.set_hexpand (true);
            timecode_label.set_xalign (1.0f);
            transport.append (timecode_label);

            trim_label = new Gtk.Label ("");
            trim_label.add_css_class ("numeric");
            trim_label.add_css_class ("dim-label");
            transport.append (trim_label);

            btn_export = new Gtk.Button.with_label ("Export");
            btn_export.add_css_class ("suggested-action");
            btn_export.add_css_class ("pill");
            btn_export.set_sensitive (false);
            btn_export.clicked.connect (() => start_export ());
            transport.append (btn_export);

            monitor_box.append (transport);

            export_progress = new Gtk.ProgressBar ();
            export_progress.set_visible (false);
            export_progress.set_show_text (true);
            monitor_box.append (export_progress);

            top_split.append (monitor_box);
            append (top_split);
            append (new Gtk.Separator (Gtk.Orientation.HORIZONTAL));

            // Bottom: Simplified V1/A1 Timeline
            timeline = new TimelineWidget ();
            timeline.seek_requested.connect ((pos_ms) => {
                if (media_file != null) {
                    media_file.seek (pos_ms * 1000);
                }
                update_timecode_labels ();
            });
            timeline.range_changed.connect ((in_ms, out_ms) => {
                update_timecode_labels ();
            });
            append (timeline);
        }

        public void add_and_select_clip (string path) {
            if (path == "") return;
            int found = -1;
            for (uint i = 0; i < clip_paths.length; i++) {
                if (clip_paths.get (i) == path) {
                    found = (int) i;
                    break;
                }
            }
            if (found < 0) {
                clip_paths.add (path);
                var row = new Adw.ActionRow ();
                row.set_title (GLib.Path.get_basename (path));
                row.set_subtitle (GLib.Path.get_dirname (path));
                clip_list.append (row);
                found = (int) clip_paths.length - 1;
            }
            var row = clip_list.get_row_at_index (found);
            if (row != null) {
                clip_list.select_row (row);
            }
            load_clip (path);
        }

        private void load_clip (string path) {
            active_clip_path = path;
            if (media_file != null) {
                media_file.pause ();
            }
            media_file = Gtk.MediaFile.for_filename (path);
            video_picture.set_paintable (media_file);
            btn_play_pause.set_sensitive (true);
            btn_export.set_sensitive (!is_exporting);

            media_file.notify["duration"].connect (() => {
                int64 dur_us = media_file.get_duration ();
                if (dur_us > 0) {
                    timeline.load_duration_ms (dur_us / 1000);
                    update_timecode_labels ();
                }
            });
            media_file.notify["timestamp"].connect (() => {
                int64 pos_us = media_file.get_timestamp ();
                timeline.update_position_ms (pos_us / 1000);
                update_timecode_labels ();
            });
            media_file.notify["playing"].connect (() => {
                play_icon.set_from_icon_name (
                    media_file.get_playing () ? "media-playback-pause-symbolic" : "media-playback-start-symbolic"
                );
            });
        }

        private void toggle_playback () {
            if (media_file == null) return;
            if (media_file.get_playing ()) {
                media_file.pause ();
            } else {
                media_file.play ();
            }
        }

        private void step_seconds (int delta_s) {
            if (media_file == null || timeline.duration_ms <= 0) return;
            int64 target = (timeline.position_ms + (int64) delta_s * 1000).clamp (0, timeline.duration_ms);
            media_file.seek (target * 1000);
            timeline.update_position_ms (target);
            update_timecode_labels ();
        }

        private static string fmt_ms (int64 ms) {
            int total_s = (int) (int64.max (0, ms) / 1000);
            return "%02d:%02d".printf (total_s / 60, total_s % 60);
        }

        private void update_timecode_labels () {
            timecode_label.set_text ("%s / %s".printf (fmt_ms (timeline.position_ms), fmt_ms (timeline.duration_ms)));
            if (timeline.duration_ms > 0) {
                trim_label.set_text ("[%s – %s]".printf (fmt_ms (timeline.in_point_ms), fmt_ms (timeline.out_point_ms)));
            } else {
                trim_label.set_text ("");
            }
        }

        private void open_file_picker () {
            var dialog = new Gtk.FileDialog ();
            dialog.set_title ("Open Video Clip");
            var filter = new Gtk.FileFilter ();
            filter.name = "Video files";
            filter.add_mime_type ("video/*");
            var filters = new GLib.ListStore (typeof (Gtk.FileFilter));
            filters.append (filter);
            dialog.set_filters (filters);

            var win = get_root () as Gtk.Window;
            dialog.open.begin (win, null, (obj, res) => {
                try {
                    var file = dialog.open.end (res);
                    if (file != null && file.get_path () != null) {
                        add_and_select_clip (file.get_path ());
                    }
                } catch (GLib.Error e) {
                    // Cancelled
                }
            });
        }

        private void start_export () {
            if (active_clip_path == null || is_exporting)
                return;

            var codecs = Gst.get_supported_codecs ();
            Gst.VideoCodec codec = Gst.VideoCodec.H264;
            Gst.ContainerFormat container = Gst.ContainerFormat.MP4;
            string saved_id = settings.get_string ("video-codec", "h264");
            for (uint i = 0; i < codecs.length; i++) {
                if (codecs.get (i).id == saved_id) {
                    codec = codecs.get (i).codec;
                    container = codecs.get (i).container;
                    break;
                }
            }

            string? videos_dir = GLib.Environment.get_user_special_dir (GLib.UserDirectory.VIDEOS);
            if (videos_dir == null) videos_dir = "/tmp";
            var now = new GLib.DateTime.now_local ();
            string ts = now.format ("%Y-%m-%d_%H-%M-%S") ?? "edited";
            string ext = Gst.get_extension_for_container (container);
            string out_path = GLib.Path.build_filename (videos_dir, "RecGo-Edit-%s.%s".printf (ts, ext));

            Gst.EditSegment seg = { (uint64) timeline.in_point_ms, (uint64) timeline.out_point_ms };
            Gst.EditSegment[] segs = { seg };

            try {
                is_exporting = true;
                btn_export.set_sensitive (false);
                export_progress.set_fraction (0.0);
                export_progress.set_visible (true);
                editor.export_async (active_clip_path, out_path, segs, codec, container);
            } catch (GLib.Error e) {
                is_exporting = false;
                btn_export.set_sensitive (true);
                export_progress.set_visible (false);
                toast_requested ("Export error: %s".printf (e.message));
            }
        }
    }
}
