namespace RecGo {

    public class TimelineWidget : Gtk.Widget {
        public int64 duration_ms { get; private set; default = 0; }
        public int64 position_ms { get; private set; default = 0; }
        public int64 in_point_ms { get; private set; default = 0; }
        public int64 out_point_ms { get; private set; default = 0; }

        public signal void seek_requested (int64 pos_ms);
        public signal void range_changed (int64 in_ms, int64 out_ms);

        private GLib.GenericArray<int64?> split_points = new GLib.GenericArray<int64?> ();
        private enum DragTarget { NONE, PLAYHEAD, IN_HANDLE, OUT_HANDLE }
        private DragTarget drag_target = DragTarget.NONE;

        private const float RULER_H = 24.0f;
        private const float PAD_X = 14.0f;

        construct {
            hexpand = true;
            vexpand = false;
            set_size_request (-1, 132);
            overflow = Gtk.Overflow.HIDDEN;

            var click = new Gtk.GestureClick ();
            click.pressed.connect ((n_press, x, y) => {
                if (duration_ms <= 0)
                    return;
                int64 ms = x_to_ms ((float) x);
                position_ms = ms;
                seek_requested (ms);
                queue_draw ();
            });
            add_controller (click);

            var drag = new Gtk.GestureDrag ();
            drag.drag_begin.connect ((start_x, start_y) => {
                if (duration_ms <= 0) {
                    drag_target = DragTarget.NONE;
                    return;
                }
                float in_x = ms_to_x (in_point_ms);
                float out_x = ms_to_x (out_point_ms);
                if (Math.fabsf ((float) start_x - in_x) <= 12.0f && start_y >= RULER_H) {
                    drag_target = DragTarget.IN_HANDLE;
                } else if (Math.fabsf ((float) start_x - out_x) <= 12.0f && start_y >= RULER_H) {
                    drag_target = DragTarget.OUT_HANDLE;
                } else {
                    drag_target = DragTarget.PLAYHEAD;
                    int64 ms = x_to_ms ((float) start_x);
                    position_ms = ms;
                    seek_requested (ms);
                    queue_draw ();
                }
            });

            drag.drag_update.connect ((offset_x, offset_y) => {
                if (duration_ms <= 0 || drag_target == DragTarget.NONE)
                    return;
                double sx = 0, sy = 0;
                drag.get_start_point (out sx, out sy);
                int64 ms = x_to_ms ((float) (sx + offset_x));

                if (drag_target == DragTarget.IN_HANDLE) {
                    in_point_ms = int64.min (ms, int64.max (0, out_point_ms - 500));
                    range_changed (in_point_ms, out_point_ms);
                } else if (drag_target == DragTarget.OUT_HANDLE) {
                    out_point_ms = int64.max (ms, int64.min (duration_ms, in_point_ms + 500));
                    range_changed (in_point_ms, out_point_ms);
                } else {
                    position_ms = ms;
                    seek_requested (ms);
                }
                queue_draw ();
            });

            drag.drag_end.connect (() => {
                drag_target = DragTarget.NONE;
            });
            add_controller (drag);
        }

        public void load_duration_ms (int64 dur_ms) {
            duration_ms = int64.max (0, dur_ms);
            in_point_ms = 0;
            out_point_ms = duration_ms;
            position_ms = 0;
            split_points.remove_range (0, split_points.length);
            queue_draw ();
        }

        public void update_position_ms (int64 pos_ms) {
            if (duration_ms <= 0) {
                position_ms = 0;
            } else {
                position_ms = pos_ms.clamp (0, duration_ms);
            }
            queue_draw ();
        }

        public void set_in_point_at_playhead () {
            if (duration_ms <= 0) return;
            in_point_ms = int64.min (position_ms, int64.max (0, out_point_ms - 500));
            range_changed (in_point_ms, out_point_ms);
            queue_draw ();
        }

        public void set_out_point_at_playhead () {
            if (duration_ms <= 0) return;
            out_point_ms = int64.max (position_ms, int64.min (duration_ms, in_point_ms + 500));
            range_changed (in_point_ms, out_point_ms);
            queue_draw ();
        }

        public void add_split_at_playhead () {
            if (duration_ms <= 0) return;
            if (position_ms > in_point_ms + 100 && position_ms < out_point_ms - 100) {
                split_points.add (position_ms);
                queue_draw ();
            }
        }

        public void reset_trim () {
            in_point_ms = 0;
            out_point_ms = duration_ms;
            split_points.remove_range (0, split_points.length);
            range_changed (in_point_ms, out_point_ms);
            queue_draw ();
        }

        private float track_width () {
            return float.max (40.0f, (float) get_width () - PAD_X * 2.0f);
        }

        private float ms_to_x (int64 ms) {
            if (duration_ms <= 0) return PAD_X;
            double frac = ((double) ms / (double) duration_ms).clamp (0.0, 1.0);
            return PAD_X + (float) frac * track_width ();
        }

        private int64 x_to_ms (float x) {
            if (duration_ms <= 0) return 0;
            double frac = ((double) (x - PAD_X) / (double) track_width ()).clamp (0.0, 1.0);
            return (int64) Math.llround (frac * (double) duration_ms);
        }

        private static Gdk.RGBA rgba (float r, float g, float b, float a) {
            return { r, g, b, a };
        }

        public override void snapshot (Gtk.Snapshot snapshot) {
            float w = (float) get_width ();
            float h = (float) get_height ();
            if (w <= 0 || h <= 0) return;

            var bg = Graphene.Rect ().init (0, 0, w, h);
            snapshot.append_color (rgba (0.10f, 0.11f, 0.14f, 1.0f), bg);

            var ruler_rect = Graphene.Rect ().init (0, 0, w, RULER_H);
            snapshot.append_color (rgba (0.07f, 0.08f, 0.10f, 1.0f), ruler_rect);

            float tw = track_width ();
            float v1_y = RULER_H + 8.0f;
            float track_h = 42.0f;
            float a1_y = v1_y + track_h + 6.0f;

            // Track backgrounds
            snapshot.append_color (rgba (0.14f, 0.16f, 0.20f, 1.0f), Graphene.Rect ().init (PAD_X, v1_y, tw, track_h));
            snapshot.append_color (rgba (0.14f, 0.16f, 0.20f, 1.0f), Graphene.Rect ().init (PAD_X, a1_y, tw, track_h));

            // Ruler ticks
            int ticks = 8;
            var pango_ctx = get_pango_context ();
            var font = new Pango.FontDescription ();
            font.set_family ("monospace");
            font.set_size (8 * Pango.SCALE);

            for (int i = 0; i <= ticks; i++) {
                float tx = PAD_X + tw * ((float) i / (float) ticks);
                snapshot.append_color (rgba (0.45f, 0.50f, 0.60f, 0.65f), Graphene.Rect ().init (tx, RULER_H - 6.0f, 1.0f, 6.0f));
                if (duration_ms > 0 && i < ticks) {
                    int64 t_ms = (int64) ((double) duration_ms * ((double) i / (double) ticks));
                    int secs = (int) (t_ms / 1000);
                    var lbl = new Pango.Layout (pango_ctx);
                    lbl.set_font_description (font);
                    lbl.set_text ("%02d:%02d".printf (secs / 60, secs % 60), -1);
                    snapshot.save ();
                    snapshot.translate (Graphene.Point ().init (tx + 3.0f, 4.0f));
                    snapshot.append_layout (lbl, rgba (0.68f, 0.72f, 0.80f, 0.85f));
                    snapshot.restore ();
                }
            }

            if (duration_ms > 0) {
                float in_x = ms_to_x (in_point_ms);
                float out_x = ms_to_x (out_point_ms);
                float seg_w = float.max (4.0f, out_x - in_x);

                // V1 clip block
                var v1_rect = Graphene.Rect ().init (in_x, v1_y + 2.0f, seg_w, track_h - 4.0f);
                var v1_round = Gsk.RoundedRect ().init_from_rect (v1_rect, 6.0f);
                snapshot.push_rounded_clip (v1_round);
                snapshot.append_color (rgba (0.20f, 0.44f, 0.76f, 0.90f), v1_rect);
                snapshot.pop ();
                var v1_bc = rgba (0.45f, 0.68f, 0.95f, 0.95f);
                snapshot.append_border (v1_round, { 1.5f, 1.5f, 1.5f, 1.5f }, { v1_bc, v1_bc, v1_bc, v1_bc });

                // A1 clip block
                var a1_rect = Graphene.Rect ().init (in_x, a1_y + 2.0f, seg_w, track_h - 4.0f);
                var a1_round = Gsk.RoundedRect ().init_from_rect (a1_rect, 6.0f);
                snapshot.push_rounded_clip (a1_round);
                snapshot.append_color (rgba (0.16f, 0.56f, 0.45f, 0.90f), a1_rect);
                snapshot.pop ();
                var a1_bc = rgba (0.36f, 0.82f, 0.68f, 0.95f);
                snapshot.append_border (a1_round, { 1.5f, 1.5f, 1.5f, 1.5f }, { a1_bc, a1_bc, a1_bc, a1_bc });

                // Track labels
                draw_track_label (snapshot, pango_ctx, font, "V1", in_x + 8.0f, v1_y + 13.0f);
                draw_track_label (snapshot, pango_ctx, font, "A1", in_x + 8.0f, a1_y + 13.0f);

                // Split markers
                for (uint i = 0; i < split_points.length; i++) {
                    int64 sp = split_points.get (i);
                    if (sp > in_point_ms && sp < out_point_ms) {
                        float sx = ms_to_x (sp);
                        snapshot.append_color (rgba (0.98f, 0.84f, 0.25f, 0.95f),
                                               Graphene.Rect ().init (sx - 1.0f, v1_y, 2.0f, (a1_y + track_h) - v1_y));
                    }
                }

                // In / Out trim handles
                snapshot.append_color (rgba (0.96f, 0.98f, 1.0f, 0.95f),
                                       Graphene.Rect ().init (in_x - 2.0f, v1_y, 4.0f, (a1_y + track_h) - v1_y));
                snapshot.append_color (rgba (0.96f, 0.98f, 1.0f, 0.95f),
                                       Graphene.Rect ().init (out_x - 2.0f, v1_y, 4.0f, (a1_y + track_h) - v1_y));

                // Playhead
                float px = ms_to_x (position_ms);
                snapshot.append_color (rgba (0.93f, 0.20f, 0.23f, 1.0f),
                                       Graphene.Rect ().init (px - 1.0f, 4.0f, 2.0f, h - 4.0f));
                var head_rect = Graphene.Rect ().init (px - 6.0f, 2.0f, 12.0f, 12.0f);
                var head_round = Gsk.RoundedRect ().init_from_rect (head_rect, 3.0f);
                snapshot.push_rounded_clip (head_round);
                snapshot.append_color (rgba (0.93f, 0.20f, 0.23f, 1.0f), head_rect);
                snapshot.pop ();
            }
        }

        private static void draw_track_label (Gtk.Snapshot snapshot,
                                              Pango.Context ctx,
                                              Pango.FontDescription font,
                                              string text,
                                              float x,
                                              float y) {
            var lbl = new Pango.Layout (ctx);
            lbl.set_font_description (font);
            lbl.set_text (text, -1);
            snapshot.save ();
            snapshot.translate (Graphene.Point ().init (x, y));
            snapshot.append_layout (lbl, rgba (1.0f, 1.0f, 1.0f, 0.92f));
            snapshot.restore ();
        }
    }
}
