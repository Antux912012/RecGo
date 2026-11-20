namespace RecGo {

    public class PreviewWidget : Gtk.Widget {
        public double overlay_x { get; set; default = 0.76; }
        public double overlay_y { get; set; default = 0.68; }
        public double overlay_scale { get; set; default = 0.24; }
        public Gst.WebcamShape shape { get; set; default = Gst.WebcamShape.CIRCLE; }
        public Gst.LayoutPreset layout { get; set; default = Gst.LayoutPreset.DESKTOP_PIP; }
        public bool border_enabled { get; set; default = true; }
        public bool shadow_enabled { get; set; default = true; }
        public bool is_recording { get; private set; default = false; }
        public bool is_paused { get; private set; default = false; }
        public int countdown { get; private set; default = -1; }

        public signal void overlay_changed ();

        private Gdk.Texture? screen_texture = null;
        private GLib.GenericArray<Gdk.Texture?> webcam_textures = new GLib.GenericArray<Gdk.Texture?> ();

        private double drag_start_x = 0.76;
        private double drag_start_y = 0.68;
        private double drag_start_scale = 0.24;
        private bool is_resizing = false;
        private float last_handle_x = 0;
        private float last_handle_y = 0;

        construct {
            hexpand = true;
            vexpand = true;
            overflow = Gtk.Overflow.HIDDEN;

            notify["shape"].connect (() => queue_draw ());
            notify["layout"].connect (() => queue_draw ());
            notify["border-enabled"].connect (() => queue_draw ());
            notify["shadow-enabled"].connect (() => queue_draw ());

            var drag = new Gtk.GestureDrag ();
            drag.drag_begin.connect ((start_x, start_y) => {
                if (is_recording || layout != Gst.LayoutPreset.DESKTOP_PIP)
                    return;

                drag_start_x = overlay_x;
                drag_start_y = overlay_y;
                drag_start_scale = overlay_scale;

                float dx = (float) start_x - last_handle_x;
                float dy = (float) start_y - last_handle_y;
                is_resizing = (dx * dx + dy * dy) <= (28.0f * 28.0f);
            });

            drag.drag_update.connect ((offset_x, offset_y) => {
                if (is_recording || layout != Gst.LayoutPreset.DESKTOP_PIP)
                    return;

                double w = double.max (100.0, (double) get_width () - 40.0);
                double h = double.max (100.0, (double) get_height () - 40.0);

                if (is_resizing) {
                    double delta = (offset_x + offset_y) / (w * 1.5);
                    overlay_scale = (drag_start_scale + delta).clamp (0.10, 0.65);
                } else {
                    overlay_x = (drag_start_x + offset_x / w).clamp (0.0, 1.0);
                    overlay_y = (drag_start_y + offset_y / h).clamp (0.0, 1.0);
                }
                queue_draw ();
                overlay_changed ();
            });
            add_controller (drag);

            var scroll = new Gtk.EventControllerScroll (Gtk.EventControllerScrollFlags.VERTICAL);
            scroll.scroll.connect ((dx, dy) => {
                if (is_recording || layout != Gst.LayoutPreset.DESKTOP_PIP)
                    return false;

                overlay_scale = (overlay_scale - dy * 0.02).clamp (0.10, 0.65);
                queue_draw ();
                overlay_changed ();
                return true;
            });
            add_controller (scroll);
        }

        public void set_screen_texture (Gdk.Texture? tex) {
            screen_texture = tex;
            queue_draw ();
        }

        public void set_webcam_count (uint count) {
            while (webcam_textures.length < count) {
                webcam_textures.add (null);
            }
            if (webcam_textures.length > count) {
                webcam_textures.remove_range (count, webcam_textures.length - count);
            }
            queue_draw ();
        }

        public void set_webcam_texture (uint index, Gdk.Texture? tex) {
            while (webcam_textures.length <= index) {
                webcam_textures.add (null);
            }
            webcam_textures.set (index, tex);
            queue_draw ();
        }

        public void clear_webcam_textures () {
            webcam_textures.remove_range (0, webcam_textures.length);
            queue_draw ();
        }

        public void set_recording_state (bool recording, bool paused) {
            is_recording = recording;
            is_paused = paused;
            queue_draw ();
        }

        public void set_countdown_value (int sec) {
            countdown = sec;
            queue_draw ();
        }

        private static Gdk.RGBA rgba (float r, float g, float b, float a) {
            return { r, g, b, a };
        }

        public override void snapshot (Gtk.Snapshot snapshot) {
            float width = (float) get_width ();
            float height = (float) get_height ();
            if (width <= 0.0f || height <= 0.0f)
                return;

            var canvas_rect = Graphene.Rect ().init (0, 0, width, height);
            snapshot.append_color (rgba (0.09f, 0.10f, 0.13f, 1.0f), canvas_rect);

            float pad = 20.0f;
            float stage_w = float.max (100.0f, width - pad * 2.0f);
            float stage_h = float.max (80.0f, height - pad * 2.0f);

            if (is_recording) {
                // During recording, screen_texture carries the live composited recording output
                // (with the baked webcam mask, border, shadow, and image FX).
                draw_screen_stage (snapshot, pad, pad, stage_w, stage_h);
            } else {
                switch (layout) {
                case Gst.LayoutPreset.DESKTOP_PIP:
                case Gst.LayoutPreset.SCREEN_ONLY:
                    draw_screen_stage (snapshot, pad, pad, stage_w, stage_h);
                    if (layout == Gst.LayoutPreset.DESKTOP_PIP && webcam_textures.length > 0) {
                        draw_primary_webcam (snapshot, pad, pad, stage_w, stage_h);
                        draw_secondary_webcams (snapshot, pad, pad, stage_w, stage_h);
                    }
                    break;

                case Gst.LayoutPreset.SIDE_BY_SIDE:
                    draw_side_by_side (snapshot, pad, pad, stage_w, stage_h);
                    break;

                case Gst.LayoutPreset.CAMERA_ONLY:
                    if (webcam_textures.length <= 1) {
                        draw_camera_full (snapshot, pad, pad, stage_w, stage_h);
                    } else {
                        draw_camera_grid (snapshot, pad, pad, stage_w, stage_h);
                    }
                    break;
                }
            }

            if (countdown > 0) {
                draw_countdown_overlay (snapshot, width, height);
            }
        }

        private void draw_screen_stage (Gtk.Snapshot snapshot, float x, float y, float w, float h) {
            var rect = Graphene.Rect ().init (x, y, w, h);
            var rounded = Gsk.RoundedRect ().init_from_rect (rect, 12.0f);

            snapshot.push_rounded_clip (rounded);
            if (screen_texture != null) {
                snapshot.append_texture (screen_texture, rect);
            } else {
                snapshot.append_color (rgba (0.11f, 0.14f, 0.19f, 1.0f), rect);
                var top_bar = Graphene.Rect ().init (x, y, w, 26.0f);
                snapshot.append_color (rgba (0.07f, 0.08f, 0.11f, 0.92f), top_bar);

                if (countdown <= 0 && !is_recording) {
                    draw_empty_state_text (snapshot, x, y, w, h);
                }
            }
            snapshot.pop ();

            Gdk.RGBA border_color;
            if (is_recording) {
                border_color = is_paused
                    ? rgba (0.96f, 0.82f, 0.18f, 0.95f)
                    : rgba (0.88f, 0.11f, 0.14f, 0.95f);
            } else {
                border_color = rgba (0.35f, 0.42f, 0.55f, 0.65f);
            }
            snapshot.append_border (rounded, { 2.5f, 2.5f, 2.5f, 2.5f },
                                    { border_color, border_color, border_color, border_color });
        }

        private void draw_camera_full (Gtk.Snapshot snapshot, float x, float y, float w, float h) {
            var rect = Graphene.Rect ().init (x, y, w, h);
            var rounded = Gsk.RoundedRect ().init_from_rect (rect, 12.0f);

            snapshot.push_rounded_clip (rounded);
            Gdk.Texture? cam_tex = (webcam_textures.length > 0) ? webcam_textures.get (0) : null;
            if (cam_tex != null) {
                draw_texture_cover (snapshot, cam_tex, rect);
            } else {
                snapshot.append_color (rgba (0.14f, 0.22f, 0.28f, 1.0f), rect);
            }
            snapshot.pop ();

            var border_c = rgba (0.35f, 0.42f, 0.55f, 0.65f);
            snapshot.append_border (rounded, { 2.5f, 2.5f, 2.5f, 2.5f },
                                    { border_c, border_c, border_c, border_c });
        }

        private void draw_empty_state_text (Gtk.Snapshot snapshot, float x, float y, float w, float h) {
            var pango_ctx = get_pango_context ();
            var title_layout = new Pango.Layout (pango_ctx);
            title_layout.set_text ("Ready to Capture", -1);
            var title_font = new Pango.FontDescription ();
            title_font.set_family ("sans-serif");
            title_font.set_weight (Pango.Weight.BOLD);
            title_font.set_size (16 * Pango.SCALE);
            title_layout.set_font_description (title_font);

            var sub_layout = new Pango.Layout (pango_ctx);
            sub_layout.set_text (
                "Click Select Source to preview a monitor or window,\n" +
                "or press Record to begin capturing.\n" +
                "Drag webcam to move · Drag handle or scroll to resize", -1);
            sub_layout.set_alignment (Pango.Alignment.CENTER);
            var sub_font = new Pango.FontDescription ();
            sub_font.set_family ("sans-serif");
            sub_font.set_size (10 * Pango.SCALE);
            sub_layout.set_font_description (sub_font);

            int tw, th, sw, sh;
            title_layout.get_pixel_size (out tw, out th);
            sub_layout.get_pixel_size (out sw, out sh);

            float cx = (layout == Gst.LayoutPreset.DESKTOP_PIP) ? (x + w * 0.35f) : (x + w * 0.5f);
            float cy = y + h * 0.46f;

            snapshot.save ();
            snapshot.translate (Graphene.Point ().init (cx - (float) tw / 2.0f, cy - (float) th - 4.0f));
            snapshot.append_layout (title_layout, rgba (0.92f, 0.94f, 0.97f, 0.88f));
            snapshot.restore ();

            snapshot.save ();
            snapshot.translate (Graphene.Point ().init (cx - (float) sw / 2.0f, cy + 6.0f));
            snapshot.append_layout (sub_layout, rgba (0.70f, 0.75f, 0.84f, 0.72f));
            snapshot.restore ();
        }

        private void draw_texture_cover (Gtk.Snapshot snapshot, Gdk.Texture tex, Graphene.Rect bounds) {
            float tex_w = (float) tex.get_width ();
            float tex_h = (float) tex.get_height ();
            float tex_aspect = float.max (0.5f, tex_w / float.max (1.0f, tex_h));
            float box_aspect = bounds.size.width / float.max (1.0f, bounds.size.height);

            Graphene.Rect draw_rect;
            if (tex_aspect > box_aspect) {
                float draw_w = bounds.size.height * tex_aspect;
                float offset_x = (draw_w - bounds.size.width) / 2.0f;
                draw_rect = Graphene.Rect ().init (bounds.origin.x - offset_x, bounds.origin.y, draw_w, bounds.size.height);
            } else {
                float draw_h = bounds.size.width / tex_aspect;
                float offset_y = (draw_h - bounds.size.height) / 2.0f;
                draw_rect = Graphene.Rect ().init (bounds.origin.x, bounds.origin.y - offset_y, bounds.size.width, draw_h);
            }
            snapshot.append_texture (tex, draw_rect);
        }

        private void draw_primary_webcam (Gtk.Snapshot snapshot, float stage_x, float stage_y, float stage_w, float stage_h) {
            float scale = (float) overlay_scale.clamp (0.10, 0.65);
            float cam_w, cam_h;

            if (shape == Gst.WebcamShape.CIRCLE || shape == Gst.WebcamShape.SQUARE) {
                float side = (stage_w * scale).clamp (80.0f, stage_h * 0.85f);
                cam_w = side;
                cam_h = side;
            } else {
                cam_w = (stage_w * scale * 1.3f).clamp (110.0f, stage_w * 0.85f);
                cam_h = cam_w * 9.0f / 16.0f;
            }

            float max_x = float.max (0.0f, stage_w - cam_w);
            float max_y = float.max (0.0f, stage_h - cam_h);
            float x = stage_x + ((float) overlay_x * max_x).clamp (0.0f, max_x);
            float y = stage_y + ((float) overlay_y * max_y).clamp (0.0f, max_y);

            var cam_rect = Graphene.Rect ().init (x, y, cam_w, cam_h);
            float radius = (shape == Gst.WebcamShape.CIRCLE) ? (cam_w / 2.0f)
                         : (shape == Gst.WebcamShape.SQUARE ? (cam_w * 0.09f).clamp (8.0f, 26.0f)
                                                            : (cam_h * 0.07f).clamp (6.0f, 18.0f));
            var rounded = Gsk.RoundedRect ().init_from_rect (cam_rect, radius);

            if (shadow_enabled) {
                snapshot.append_outset_shadow (rounded, rgba (0.0f, 0.0f, 0.0f, 0.68f), 0.0f, 6.0f, 2.0f, 16.0f);
            }

            snapshot.push_rounded_clip (rounded);
            Gdk.Texture? cam_tex = (webcam_textures.length > 0) ? webcam_textures.get (0) : null;
            if (cam_tex != null) {
                draw_texture_cover (snapshot, cam_tex, cam_rect);
            } else {
                snapshot.append_color (rgba (0.18f, 0.34f, 0.40f, 1.0f), cam_rect);
            }
            snapshot.pop ();

            if (border_enabled) {
                var border_c = rgba (1.0f, 1.0f, 1.0f, 0.92f);
                snapshot.append_border (rounded, { 2.5f, 2.5f, 2.5f, 2.5f },
                                        { border_c, border_c, border_c, border_c });
            }

            float handle_size = 16.0f;
            float hx, hy;
            if (shape == Gst.WebcamShape.CIRCLE) {
                float r = cam_w / 2.0f;
                float offset = r * 0.7071f;
                hx = x + r + offset - handle_size / 2.0f;
                hy = y + r + offset - handle_size / 2.0f;
            } else {
                hx = x + cam_w - handle_size * 0.75f;
                hy = y + cam_h - handle_size * 0.75f;
            }
            last_handle_x = hx + handle_size / 2.0f;
            last_handle_y = hy + handle_size / 2.0f;

            var handle_rect = Graphene.Rect ().init (hx, hy, handle_size, handle_size);
            var handle_rounded = Gsk.RoundedRect ().init_from_rect (handle_rect, handle_size / 2.0f);
            snapshot.push_rounded_clip (handle_rounded);
            snapshot.append_color (rgba (0.21f, 0.52f, 0.89f, 1.0f), handle_rect);
            snapshot.pop ();
            var hb = rgba (1.0f, 1.0f, 1.0f, 0.95f);
            snapshot.append_border (handle_rounded, { 2.0f, 2.0f, 2.0f, 2.0f }, { hb, hb, hb, hb });
        }

        private void draw_secondary_webcams (Gtk.Snapshot snapshot, float stage_x, float stage_y, float stage_w, float stage_h) {
            if (webcam_textures.length <= 1)
                return;

            float tile_w = float.max (100.0f, stage_w * 0.18f);
            float tile_h = tile_w * 9.0f / 16.0f;
            float margin = 14.0f;

            for (uint i = 1; i < webcam_textures.length; i++) {
                float tx = stage_x + margin + (float) (i - 1) * (tile_w + 10.0f);
                float ty = stage_y + stage_h - tile_h - margin;
                var rect = Graphene.Rect ().init (tx, ty, tile_w, tile_h);
                var rounded = Gsk.RoundedRect ().init_from_rect (rect, 10.0f);

                snapshot.push_rounded_clip (rounded);
                Gdk.Texture? tex = webcam_textures.get (i);
                if (tex != null) {
                    draw_texture_cover (snapshot, tex, rect);
                } else {
                    snapshot.append_color (rgba (0.16f, 0.25f, 0.30f, 1.0f), rect);
                }
                snapshot.pop ();

                var bc = rgba (0.28f, 0.72f, 0.65f, 0.85f);
                snapshot.append_border (rounded, { 2.0f, 2.0f, 2.0f, 2.0f }, { bc, bc, bc, bc });
            }
        }

        private void draw_side_by_side (Gtk.Snapshot snapshot, float x, float y, float w, float h) {
            float half_w = (w - 12.0f) / 2.0f;
            var left_rect = Graphene.Rect ().init (x, y, half_w, h);
            var left_rounded = Gsk.RoundedRect ().init_from_rect (left_rect, 12.0f);

            snapshot.push_rounded_clip (left_rounded);
            if (screen_texture != null) {
                snapshot.append_texture (screen_texture, left_rect);
            } else {
                snapshot.append_color (rgba (0.14f, 0.18f, 0.25f, 1.0f), left_rect);
            }
            snapshot.pop ();
            var lbc = rgba (0.35f, 0.42f, 0.55f, 0.6f);
            snapshot.append_border (left_rounded, { 2.0f, 2.0f, 2.0f, 2.0f }, { lbc, lbc, lbc, lbc });

            uint n_cams = uint.max (1, webcam_textures.length);
            float per_h = (h - (float) (n_cams - 1) * 8.0f) / (float) n_cams;
            for (uint i = 0; i < n_cams; i++) {
                float ry = y + (float) i * (per_h + 8.0f);
                var right_rect = Graphene.Rect ().init (x + half_w + 12.0f, ry, half_w, per_h);
                var right_rounded = Gsk.RoundedRect ().init_from_rect (right_rect, 12.0f);

                snapshot.push_rounded_clip (right_rounded);
                Gdk.Texture? cam_tex = (i < webcam_textures.length) ? webcam_textures.get (i) : null;
                if (cam_tex != null) {
                    draw_texture_cover (snapshot, cam_tex, right_rect);
                } else {
                    snapshot.append_color (rgba (0.18f, 0.32f, 0.36f, 1.0f), right_rect);
                }
                snapshot.pop ();

                var rbc = rgba (0.28f, 0.72f, 0.65f, 0.8f);
                snapshot.append_border (right_rounded, { 2.0f, 2.0f, 2.0f, 2.0f }, { rbc, rbc, rbc, rbc });
            }
        }

        private void draw_camera_grid (Gtk.Snapshot snapshot, float x, float y, float w, float h) {
            uint n = webcam_textures.length;
            uint cols = (n <= 2) ? n : 2;
            uint rows = (n + cols - 1) / cols;
            float gap = 10.0f;
            float cell_w = (w - gap * (float) (cols - 1)) / (float) cols;
            float cell_h = (h - gap * (float) (rows - 1)) / (float) rows;

            for (uint i = 0; i < n; i++) {
                uint c = i % cols;
                uint r = i / cols;
                var rect = Graphene.Rect ().init (x + (float) c * (cell_w + gap),
                                                  y + (float) r * (cell_h + gap),
                                                  cell_w, cell_h);
                var rounded = Gsk.RoundedRect ().init_from_rect (rect, 12.0f);
                snapshot.push_rounded_clip (rounded);
                Gdk.Texture? tex = webcam_textures.get (i);
                if (tex != null) {
                    draw_texture_cover (snapshot, tex, rect);
                } else {
                    snapshot.append_color (rgba (0.18f, 0.32f, 0.36f, 1.0f), rect);
                }
                snapshot.pop ();
                var bc = rgba (1.0f, 1.0f, 1.0f, 0.7f);
                snapshot.append_border (rounded, { 2.0f, 2.0f, 2.0f, 2.0f }, { bc, bc, bc, bc });
            }
        }

        private void draw_countdown_overlay (Gtk.Snapshot snapshot, float width, float height) {
            float badge_radius = 70.0f;
            float cx = width / 2.0f;
            float cy = height / 2.0f;
            var badge_rect = Graphene.Rect ().init (cx - badge_radius, cy - badge_radius,
                                                    badge_radius * 2.0f, badge_radius * 2.0f);
            var badge_rounded = Gsk.RoundedRect ().init_from_rect (badge_rect, badge_radius);

            snapshot.append_outset_shadow (badge_rounded, rgba (0.0f, 0.0f, 0.0f, 0.85f), 0.0f, 10.0f, 4.0f, 24.0f);
            snapshot.push_rounded_clip (badge_rounded);
            snapshot.append_color (rgba (0.08f, 0.10f, 0.14f, 0.92f), badge_rect);
            snapshot.pop ();

            var bc = rgba (0.93f, 0.20f, 0.23f, 0.95f);
            snapshot.append_border (badge_rounded, { 4.0f, 4.0f, 4.0f, 4.0f }, { bc, bc, bc, bc });

            var text_layout = new Pango.Layout (get_pango_context ());
            string text = (countdown > 60)
                ? "%d:%02d".printf (countdown / 60, countdown % 60)
                : "%d".printf (countdown);
            text_layout.set_text (text, -1);

            var font_desc = new Pango.FontDescription ();
            font_desc.set_family ("sans-serif");
            font_desc.set_weight (Pango.Weight.HEAVY);
            font_desc.set_size ((countdown >= 100 ? 30 : 46) * Pango.SCALE);
            text_layout.set_font_description (font_desc);

            int tw, th;
            text_layout.get_pixel_size (out tw, out th);
            snapshot.save ();
            snapshot.translate (Graphene.Point ().init (cx - (float) tw / 2.0f, cy - (float) th / 2.0f));
            snapshot.append_layout (text_layout, rgba (1.0f, 1.0f, 1.0f, 1.0f));
            snapshot.restore ();
        }
    }
}
