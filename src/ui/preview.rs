// Real-time interactive video and layout preview with live desktop and resizable webcam textures.

use gtk::prelude::*;
use gtk::subclass::prelude::*;
use gtk::{gdk, glib, graphene, gsk};
use std::cell::{Cell, RefCell};

use crate::pipeline::compositor::{LayoutPreset, WebcamShape};

mod imp {
    use super::*;

    pub struct RecGoPreview {
        pub overlay_x: Cell<f64>,
        pub overlay_y: Cell<f64>,
        pub overlay_scale: Cell<f64>,
        pub shape: Cell<WebcamShape>,
        pub layout: Cell<LayoutPreset>,
        pub is_recording: Cell<bool>,
        pub is_paused: Cell<bool>,
        pub drag_start_x: Cell<f64>,
        pub drag_start_y: Cell<f64>,
        pub drag_start_scale: Cell<f64>,
        pub is_resizing: Cell<bool>,
        pub screen_texture: RefCell<Option<gdk::MemoryTexture>>,
        pub webcam_texture: RefCell<Option<gdk::MemoryTexture>>,
        pub last_cam_bounds: Cell<(f32, f32, f32, f32)>,
    }

    impl Default for RecGoPreview {
        fn default() -> Self {
            Self {
                overlay_x: Cell::new(0.76),
                overlay_y: Cell::new(0.68),
                overlay_scale: Cell::new(0.24),
                shape: Cell::new(WebcamShape::Circle),
                layout: Cell::new(LayoutPreset::DesktopPiP),
                is_recording: Cell::new(false),
                is_paused: Cell::new(false),
                drag_start_x: Cell::new(0.76),
                drag_start_y: Cell::new(0.68),
                drag_start_scale: Cell::new(0.24),
                is_resizing: Cell::new(false),
                screen_texture: RefCell::new(None),
                webcam_texture: RefCell::new(None),
                last_cam_bounds: Cell::new((0.0, 0.0, 160.0, 160.0)),
            }
        }
    }

    #[glib::object_subclass]
    impl ObjectSubclass for RecGoPreview {
        const NAME: &'static str = "RecGoPreview";
        type Type = super::RecGoPreview;
        type ParentType = gtk::Widget;
    }

    impl ObjectImpl for RecGoPreview {
        fn constructed(&self) {
            self.parent_constructed();
            let obj = self.obj();
            obj.set_hexpand(true);
            obj.set_vexpand(true);
            obj.set_overflow(gtk::Overflow::Hidden);
        }
    }

    impl WidgetImpl for RecGoPreview {
        fn snapshot(&self, snapshot: &gtk::Snapshot) {
            let widget = self.obj();
            let width = widget.width() as f32;
            let height = widget.height() as f32;

            if width <= 0.0 || height <= 0.0 {
                return;
            }

            let canvas_rect = graphene::Rect::new(0.0, 0.0, width, height);
            let bg_color = gdk::RGBA::new(0.09, 0.10, 0.13, 1.0);
            snapshot.append_color(&bg_color, &canvas_rect);

            let pad = 20.0_f32;
            let stage_w = (width - pad * 2.0).max(100.0);
            let stage_h = (height - pad * 2.0).max(80.0);
            let layout = self.layout.get();

            match layout {
                LayoutPreset::DesktopPiP | LayoutPreset::ScreenOnly => {
                    let screen_rect = graphene::Rect::new(pad, pad, stage_w, stage_h);
                    let screen_rounded = gsk::RoundedRect::from_rect(screen_rect, 12.0);
                    snapshot.push_rounded_clip(&screen_rounded);

                    if let Some(ref tex) = *self.screen_texture.borrow() {
                        snapshot.append_texture(tex, &screen_rect);
                    } else {
                        snapshot.append_color(&gdk::RGBA::new(0.14, 0.18, 0.25, 1.0), &screen_rect);
                        let top_bar = graphene::Rect::new(pad, pad, stage_w, 26.0);
                        snapshot.append_color(&gdk::RGBA::new(0.07, 0.08, 0.10, 0.9), &top_bar);
                    }
                    snapshot.pop();

                    let border_color = if self.is_recording.get() {
                        if self.is_paused.get() {
                            gdk::RGBA::new(0.96, 0.82, 0.18, 0.95)
                        } else {
                            gdk::RGBA::new(0.88, 0.11, 0.14, 0.95)
                        }
                    } else {
                        gdk::RGBA::new(0.35, 0.42, 0.55, 0.65)
                    };
                    snapshot.append_border(&screen_rounded, &[2.5; 4], &[border_color; 4]);

                    if layout == LayoutPreset::DesktopPiP {
                        self.draw_webcam_enclosure(snapshot, pad, pad, stage_w, stage_h);
                    }
                }
                LayoutPreset::SideBySide => {
                    let half_w = (stage_w - 12.0) / 2.0;
                    let left_rect = graphene::Rect::new(pad, pad, half_w, stage_h);
                    let right_rect = graphene::Rect::new(pad + half_w + 12.0, pad, half_w, stage_h);

                    let left_rounded = gsk::RoundedRect::from_rect(left_rect, 12.0);
                    snapshot.push_rounded_clip(&left_rounded);
                    if let Some(ref tex) = *self.screen_texture.borrow() {
                        snapshot.append_texture(tex, &left_rect);
                    } else {
                        snapshot.append_color(&gdk::RGBA::new(0.14, 0.18, 0.25, 1.0), &left_rect);
                    }
                    snapshot.pop();
                    snapshot.append_border(
                        &left_rounded,
                        &[2.0; 4],
                        &[gdk::RGBA::new(0.35, 0.42, 0.55, 0.6); 4],
                    );

                    let right_rounded = gsk::RoundedRect::from_rect(right_rect, 12.0);
                    snapshot.push_rounded_clip(&right_rounded);
                    if let Some(ref cam_tex) = *self.webcam_texture.borrow() {
                        snapshot.append_texture(cam_tex, &right_rect);
                    } else {
                        snapshot.append_color(&gdk::RGBA::new(0.18, 0.32, 0.36, 1.0), &right_rect);
                    }
                    snapshot.pop();
                    snapshot.append_border(
                        &right_rounded,
                        &[2.0; 4],
                        &[gdk::RGBA::new(0.28, 0.72, 0.65, 0.8); 4],
                    );
                }
                LayoutPreset::CameraOnly => {
                    self.draw_webcam_enclosure(snapshot, pad, pad, stage_w, stage_h);
                }
            }
        }
    }

    impl RecGoPreview {
        fn draw_webcam_enclosure(
            &self,
            snapshot: &gtk::Snapshot,
            stage_x: f32,
            stage_y: f32,
            stage_w: f32,
            stage_h: f32,
        ) {
            let scale = self.overlay_scale.get().clamp(0.10, 0.65) as f32;
            let shape = self.shape.get();

            let (cam_w, cam_h) = match shape {
                WebcamShape::Circle | WebcamShape::Square => {
                    let side = (stage_w * scale).clamp(80.0, stage_h * 0.85);
                    (side, side)
                }
                WebcamShape::Rectangle => {
                    let w = (stage_w * scale * 1.3).clamp(110.0, stage_w * 0.85);
                    let h = w * 9.0 / 16.0;
                    (w, h)
                }
            };

            let max_x = (stage_w - cam_w).max(0.0);
            let max_y = (stage_h - cam_h).max(0.0);
            let x = stage_x + (self.overlay_x.get() as f32 * max_x).clamp(0.0, max_x);
            let y = stage_y + (self.overlay_y.get() as f32 * max_y).clamp(0.0, max_y);

            self.last_cam_bounds.set((x, y, cam_w, cam_h));

            let cam_rect = graphene::Rect::new(x, y, cam_w, cam_h);
            let radius = match shape {
                WebcamShape::Circle => cam_w / 2.0,
                WebcamShape::Square => 22.0,
                WebcamShape::Rectangle => 14.0,
            };

            let rounded = gsk::RoundedRect::from_rect(cam_rect, radius);

            // Outer drop shadow so webcam pops out in the recording
            snapshot.append_outset_shadow(
                &rounded,
                &gdk::RGBA::new(0.0, 0.0, 0.0, 0.72),
                0.0,
                8.0,
                3.0,
                20.0,
            );

            snapshot.push_rounded_clip(&rounded);

            if let Some(ref cam_tex) = *self.webcam_texture.borrow() {
                let tex_w = cam_tex.width() as f32;
                let tex_h = cam_tex.height() as f32;
                let tex_aspect = (tex_w / tex_h.max(1.0)).max(0.5);
                let box_aspect = cam_w / cam_h.max(1.0);

                let draw_rect = if tex_aspect > box_aspect {
                    let draw_w = cam_h * tex_aspect;
                    let offset_x = (draw_w - cam_w) / 2.0;
                    graphene::Rect::new(x - offset_x, y, draw_w, cam_h)
                } else {
                    let draw_h = cam_w / tex_aspect;
                    let offset_y = (draw_h - cam_h) / 2.0;
                    graphene::Rect::new(x, y - offset_y, cam_w, draw_h)
                };
                snapshot.append_texture(cam_tex, &draw_rect);
            } else {
                snapshot.append_color(&gdk::RGBA::new(0.18, 0.34, 0.40, 1.0), &cam_rect);
            }

            snapshot.pop();

            // Crisp border highlight
            snapshot.append_border(
                &rounded,
                &[2.5; 4],
                &[gdk::RGBA::new(1.0, 1.0, 1.0, 0.90); 4],
            );

            // Draw interactive resize grip handle in the bottom-right corner of the webcam enclosure
            let handle_size = 16.0_f32;
            let (hx, hy) = match shape {
                WebcamShape::Circle => {
                    // Place handle on the 45-degree bottom-right rim of the circle
                    let r = cam_w / 2.0;
                    let cx = x + r;
                    let cy = y + r;
                    let offset = r * 0.7071;
                    (cx + offset - handle_size / 2.0, cy + offset - handle_size / 2.0)
                }
                _ => (x + cam_w - handle_size * 0.75, y + cam_h - handle_size * 0.75),
            };
            let handle_rect = graphene::Rect::new(hx, hy, handle_size, handle_size);
            let handle_rounded = gsk::RoundedRect::from_rect(handle_rect, handle_size / 2.0);
            snapshot.push_rounded_clip(&handle_rounded);
            snapshot.append_color(&gdk::RGBA::new(0.21, 0.52, 0.89, 1.0), &handle_rect);
            snapshot.pop();
            snapshot.append_border(
                &handle_rounded,
                &[2.0; 4],
                &[gdk::RGBA::new(1.0, 1.0, 1.0, 0.95); 4],
            );
        }
    }
}

glib::wrapper! {
    pub struct RecGoPreview(ObjectSubclass<imp::RecGoPreview>)
        @extends gtk::Widget;
}

impl RecGoPreview {
    pub fn new() -> Self {
        let preview: Self = glib::Object::builder().build();

        // Drag controller: either moves the webcam enclosure or resizes it if grabbed near the bottom-right handle
        let drag = gtk::GestureDrag::new();
        let preview_begin = preview.clone();
        drag.connect_drag_begin(move |_, start_x, start_y| {
            let imp = preview_begin.imp();
            imp.drag_start_x.set(imp.overlay_x.get());
            imp.drag_start_y.set(imp.overlay_y.get());
            imp.drag_start_scale.set(imp.overlay_scale.get());

            let (cx, cy, cw, ch) = imp.last_cam_bounds.get();
            let sx = start_x as f32;
            let sy = start_y as f32;

            // Check if user grabbed near the bottom-right resize handle (within 32px radius)
            let handle_x = cx + cw;
            let handle_y = cy + ch;
            let dist_sq = (sx - handle_x).powi(2) + (sy - handle_y).powi(2);
            imp.is_resizing.set(dist_sq <= 36.0 * 36.0);
        });

        let preview_update = preview.clone();
        drag.connect_drag_update(move |_, offset_x, offset_y| {
            let width = (preview_update.width() as f64 - 40.0).max(100.0);
            let height = (preview_update.height() as f64 - 40.0).max(100.0);
            let imp = preview_update.imp();

            if imp.is_resizing.get() {
                let delta = (offset_x + offset_y) / (width * 1.5);
                let new_scale = (imp.drag_start_scale.get() + delta).clamp(0.10, 0.65);
                imp.overlay_scale.set(new_scale);
            } else {
                let new_x = (imp.drag_start_x.get() + offset_x / width).clamp(0.0, 1.0);
                let new_y = (imp.drag_start_y.get() + offset_y / height).clamp(0.0, 1.0);
                imp.overlay_x.set(new_x);
                imp.overlay_y.set(new_y);
            }
            preview_update.queue_draw();
        });
        preview.add_controller(drag);

        // Mouse wheel scroll controller: scroll up/down to resize the webcam enclosure smoothly
        let scroll = gtk::EventControllerScroll::new(gtk::EventControllerScrollFlags::VERTICAL);
        let preview_scroll = preview.clone();
        scroll.connect_scroll(move |_, _dx, dy| {
            let imp = preview_scroll.imp();
            let current = imp.overlay_scale.get();
            let next = (current - dy * 0.02).clamp(0.10, 0.65);
            imp.overlay_scale.set(next);
            preview_scroll.queue_draw();
            glib::Propagation::Stop
        });
        preview.add_controller(scroll);

        preview
    }

    pub fn set_overlay_scale(&self, scale: f64) {
        self.imp().overlay_scale.set(scale.clamp(0.10, 0.65));
        self.queue_draw();
    }

    pub fn overlay_scale(&self) -> f64 {
        self.imp().overlay_scale.get()
    }

    pub fn set_webcam_texture(&self, tex: Option<gdk::MemoryTexture>) {
        self.imp().webcam_texture.replace(tex);
        self.queue_draw();
    }

    pub fn set_screen_texture(&self, tex: Option<gdk::MemoryTexture>) {
        self.imp().screen_texture.replace(tex);
        self.queue_draw();
    }

    pub fn set_shape(&self, shape: WebcamShape) {
        self.imp().shape.set(shape);
        self.queue_draw();
    }

    pub fn shape(&self) -> WebcamShape {
        self.imp().shape.get()
    }

    pub fn set_layout(&self, layout: LayoutPreset) {
        self.imp().layout.set(layout);
        self.queue_draw();
    }

    pub fn layout(&self) -> LayoutPreset {
        self.imp().layout.get()
    }

    pub fn set_recording_state(&self, recording: bool, paused: bool) {
        self.imp().is_recording.set(recording);
        self.imp().is_paused.set(paused);
        self.queue_draw();
    }
}
