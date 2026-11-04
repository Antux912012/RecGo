// Main application window for RecGo with GPU/VA-API/Vulkan/OpenGL acceleration, live preview, and webcam resizing.

use adw::prelude::*;
use adw::subclass::prelude::*;
use ashpd::desktop::screencast::{CursorMode, SourceType};
use gtk::{gdk, gio, glib};
use std::cell::{Cell, RefCell};
use std::os::fd::IntoRawFd;
use std::path::PathBuf;
use std::rc::Rc;
use std::time::Duration;

const CAMERA_CSS: &str = r#"
.camera-record-btn {
    background: linear-gradient(180deg, #ed333b 0%, #c01c28 100%);
    color: #ffffff;
    font-weight: 700;
    box-shadow: 0 2px 8px rgba(192, 28, 40, 0.45);
    border: 1.5px solid #ff7b84;
    padding: 6px 18px;
    border-radius: 9999px;
    transition: all 200ms ease;
}
.camera-record-btn:hover {
    background: linear-gradient(180deg, #f66151 0%, #d81b2a 100%);
    box-shadow: 0 0 14px rgba(237, 51, 59, 0.7);
}
.camera-record-btn:disabled {
    background: rgba(120, 120, 120, 0.25);
    color: rgba(255, 255, 255, 0.35);
    border-color: rgba(255, 255, 255, 0.1);
    box-shadow: none;
}

.camera-pause-btn {
    font-weight: 600;
    padding: 6px 16px;
    border-radius: 9999px;
    border: 1.5px solid rgba(255, 255, 255, 0.15);
    transition: all 200ms ease;
}
.camera-pause-btn:checked {
    background: linear-gradient(180deg, #f6d32d 0%, #e5a50a 100%);
    color: #1a1a1a;
    font-weight: 700;
    border-color: #f8e45c;
    box-shadow: 0 0 12px rgba(246, 211, 45, 0.65);
}

.camera-stop-btn {
    background: linear-gradient(180deg, #303541 0%, #1e222a 100%);
    color: #ffffff;
    font-weight: 600;
    border: 1.5px solid #4a5263;
    padding: 6px 16px;
    border-radius: 9999px;
    transition: all 200ms ease;
}
.camera-stop-btn:hover:not(:disabled) {
    background: linear-gradient(180deg, #c01c28 0%, #a51d2d 100%);
    border-color: #ed333b;
    box-shadow: 0 0 12px rgba(192, 28, 40, 0.55);
}
.camera-stop-btn:disabled {
    opacity: 0.4;
}

.camera-preview-btn {
    background: linear-gradient(180deg, #303541 0%, #1e222a 100%);
    color: #ffffff;
    font-weight: 600;
    border: 1.5px solid #4a5263;
    padding: 6px 14px;
    border-radius: 9999px;
    transition: all 200ms ease;
}
.camera-preview-btn:hover:not(:disabled) {
    background: linear-gradient(180deg, #3584e4 0%, #1c71d8 100%);
    border-color: #62a0ea;
    box-shadow: 0 0 12px rgba(53, 132, 228, 0.55);
}
.camera-preview-btn:disabled {
    opacity: 0.4;
}

.camera-osd-badge {
    background: rgba(14, 18, 25, 0.88);
    border: 1.5px solid rgba(255, 255, 255, 0.14);
    border-radius: 9999px;
    padding: 4px 12px;
}

.camera-osd-time {
    font-family: monospace;
    font-weight: 700;
    font-size: 13px;
    letter-spacing: 1.2px;
}

.camera-osd-dot.standby {
    color: #77767b;
}
.camera-osd-dot.recording {
    color: #ed333b;
}
.camera-osd-dot.paused {
    color: #f6d32d;
}

.camera-btn-label {
    font-weight: 700;
    font-size: 13px;
}
"#;

use crate::pipeline::codecs::{
    detect_gpu_acceleration, get_extension_for_container, get_supported_codecs, CodecInfo,
    ContainerFormat, VideoCodec,
};
use crate::pipeline::compositor::{CompositorPipeline, LayoutPreset, LiveFeedCapture, WebcamShape};
use crate::portal::screencast::{start_screencast_session, CaptureStream};
use crate::ui::preferences::PreferencesDialog;
use crate::ui::preview::RecGoPreview;

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct RecGoWindow {
        pub preview: std::cell::OnceCell<RecGoPreview>,
        pub pipeline: RefCell<Option<CompositorPipeline>>,
        pub webcam_feed: RefCell<Option<LiveFeedCapture>>,
        pub screen_preview_feed: RefCell<Option<LiveFeedCapture>>,
        pub is_recording: Cell<bool>,
        pub is_paused: Cell<bool>,
        pub mic_enabled: Cell<bool>,
        pub selected_codec_index: Cell<usize>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for RecGoWindow {
        const NAME: &'static str = "RecGoWindow";
        type Type = super::RecGoWindow;
        type ParentType = adw::ApplicationWindow;
    }

    impl ObjectImpl for RecGoWindow {}
    impl WidgetImpl for RecGoWindow {}
    impl WindowImpl for RecGoWindow {}
    impl ApplicationWindowImpl for RecGoWindow {}
    impl AdwApplicationWindowImpl for RecGoWindow {}
}

glib::wrapper! {
    pub struct RecGoWindow(ObjectSubclass<imp::RecGoWindow>)
        @extends gtk::Widget, gtk::Window, gtk::ApplicationWindow,
                 adw::ApplicationWindow,
        @implements gtk::Accessible, gtk::Buildable, gtk::ConstraintTarget,
                    gtk::Native, gtk::Root, gtk::ShortcutManager,
                    gio::ActionGroup, gio::ActionMap;
}

impl RecGoWindow {
    pub fn new(app: &impl IsA<gtk::Application>) -> Self {
        let window: Self = glib::Object::builder()
            .property("application", app)
            .property("title", "RecGo")
            .property("default-width", 1180)
            .property("default-height", 740)
            .build();

        window.imp().mic_enabled.set(true);

        let toast_overlay = adw::ToastOverlay::new();
        let toolbar_view = adw::ToolbarView::new();

        let (_gpu_mode, gpu_summary) = detect_gpu_acceleration();
        let supported_codecs: Rc<Vec<CodecInfo>> = Rc::new(get_supported_codecs());
        let default_codec_desc = supported_codecs
            .first()
            .map(|c| c.display_name.as_str())
            .unwrap_or("H.264");
        let status_label = gtk::Label::new(Some(&format!("Ready ({} | {})", gpu_summary, default_codec_desc)));
        status_label.add_css_class("dim-label");

        // Top HeaderBar
        let header = adw::HeaderBar::new();

        let source_dropdown = gtk::DropDown::from_strings(&[
            "Screen / Monitor",
            "Application Window",
        ]);
        source_dropdown.set_tooltip_text(Some("Select Capture Target"));
        header.pack_start(&source_dropdown);

        let layout_dropdown = gtk::DropDown::from_strings(&[
            "Desktop + Webcam PiP",
            "Side by Side (50/50)",
            "Screen Only",
            "Camera Only",
        ]);
        layout_dropdown.set_tooltip_text(Some("Select Recording Layout"));
        header.pack_start(&layout_dropdown);

        let shape_dropdown = gtk::DropDown::from_strings(&[
            "Circle (Shadow)",
            "Square (Shadow)",
            "Rectangle (Shadow)",
        ]);
        shape_dropdown.set_tooltip_text(Some("Select Webcam Enclosure Shape"));
        header.pack_start(&shape_dropdown);

        // Live Webcam Toggle Button in HeaderBar
        let cam_toggle = gtk::ToggleButton::new();
        cam_toggle.set_icon_name("camera-web-symbolic");
        cam_toggle.set_tooltip_text(Some("Toggle Live Webcam Feed"));
        header.pack_end(&cam_toggle);

        // Microphone Toggle Button in HeaderBar
        let mic_toggle = gtk::ToggleButton::new();
        mic_toggle.set_icon_name("audio-input-microphone-symbolic");
        mic_toggle.set_active(true);
        mic_toggle.set_tooltip_text(Some("Toggle Microphone Audio Recording"));
        header.pack_end(&mic_toggle);

        // Preferences Button in HeaderBar
        let pref_btn = gtk::Button::from_icon_name("emblem-system-symbolic");
        pref_btn.set_tooltip_text(Some("Preferences (Video Codecs & GPU Acceleration)"));
        let win_clone = window.clone();
        let toast_for_pref = toast_overlay.clone();
        let status_for_pref = status_label.clone();
        pref_btn.connect_clicked(move |_| {
            let current_idx = win_clone.imp().selected_codec_index.get();
            let win_for_dialog = win_clone.clone();
            let toast_inner = toast_for_pref.clone();
            let status_inner = status_for_pref.clone();
            let dialog = PreferencesDialog::new(current_idx, move |new_idx| {
                win_for_dialog.imp().selected_codec_index.set(new_idx);
                let codecs = get_supported_codecs();
                if let Some(c) = codecs.get(new_idx) {
                    toast_inner.add_toast(adw::Toast::new(&format!("Active encoder: {}", c.display_name)));
                    status_inner.set_text(&format!("Encoder: {}", c.display_name));
                }
            });
            dialog.present(Some(&win_clone));
        });
        header.pack_end(&pref_btn);

        // About Button in HeaderBar
        let about_btn = gtk::Button::from_icon_name("help-about-symbolic");
        about_btn.set_tooltip_text(Some("About RecGo"));
        let win_for_about = window.clone();
        about_btn.connect_clicked(move |_| {
            let about = adw::AboutDialog::builder()
                .application_name("RecGo")
                .application_icon("io.github.antonio.RecGo")
                .developer_name("Antonio")
                .version("0.1.0")
                .comments("High-performance GPU-accelerated desktop and webcam recorder for GNOME.")
                .website("https://github.com/Antux912012/RecGo")
                .issue_url("https://github.com/Antux912012/RecGo/issues")
                .license_type(gtk::License::Gpl30)
                .copyright("© 2026 Antonio")
                .build();
            about.present(Some(&win_for_about));
        });
        header.pack_end(&about_btn);

        toolbar_view.add_top_bar(&header);

        // Center content: Interactive Live Video & Webcam Enclosure Preview
        let preview = RecGoPreview::new();
        window.imp().preview.set(preview.clone()).ok();

        let cam_guard = std::rc::Rc::new(std::cell::Cell::new(false));

        // Start live webcam preview automatically if camera is available
        match LiveFeedCapture::new_webcam_auto() {
            Ok(cam_feed) => {
                window.imp().webcam_feed.replace(Some(cam_feed));
                cam_guard.set(true);
                cam_toggle.set_active(true);
                cam_guard.set(false);
            }
            Err(e) => {
                eprintln!("RecGo: Webcam auto-start notice: {}", e);
                cam_toggle.set_active(false);
            }
        }

        let banner_text = format!(
            "GPU Acceleration Active: {} | Drag webcam to move; drag blue corner handle, scroll wheel, or use slider to resize.",
            gpu_summary
        );
        let status_banner = adw::Banner::new(&banner_text);
        status_banner.set_revealed(true);

        let content_box = gtk::Box::new(gtk::Orientation::Vertical, 0);
        content_box.append(&status_banner);
        content_box.append(&preview);
        toolbar_view.set_content(Some(&content_box));

        // 30 FPS UI refresh timer pulling frames from live webcam & desktop feeds
        let win_timer = window.clone();
        let preview_timer = preview.clone();
        glib::timeout_add_local(Duration::from_millis(33), move || {
            if let Some(ref cam) = *win_timer.imp().webcam_feed.borrow() {
                if let Some(tex) = cam.latest_texture() {
                    preview_timer.set_webcam_texture(Some(tex));
                }
            }
            if let Some(ref rec_pipe) = *win_timer.imp().pipeline.borrow() {
                if let Some(tex) = rec_pipe.latest_screen_texture() {
                    preview_timer.set_screen_texture(Some(tex));
                }
            } else if let Some(ref scr_feed) = *win_timer.imp().screen_preview_feed.borrow() {
                if let Some(tex) = scr_feed.latest_texture() {
                    preview_timer.set_screen_texture(Some(tex));
                }
            }
            glib::ControlFlow::Continue
        });

        // Connect Layout, Shape, and Mic controls
        let preview_for_layout = preview.clone();
        layout_dropdown.connect_selected_notify(move |dd| {
            let preset = match dd.selected() {
                0 => LayoutPreset::DesktopPiP,
                1 => LayoutPreset::SideBySide,
                2 => LayoutPreset::ScreenOnly,
                _ => LayoutPreset::CameraOnly,
            };
            preview_for_layout.set_layout(preset);
        });

        let preview_for_shape = preview.clone();
        shape_dropdown.connect_selected_notify(move |dd| {
            let shape = match dd.selected() {
                0 => WebcamShape::Circle,
                1 => WebcamShape::Square,
                _ => WebcamShape::Rectangle,
            };
            preview_for_shape.set_shape(shape);
        });

        let win_for_cam = window.clone();
        let preview_for_cam = preview.clone();
        let toast_for_cam = toast_overlay.clone();
        let cam_guard_cb = cam_guard.clone();
        cam_toggle.connect_toggled(move |btn| {
            if cam_guard_cb.get() {
                return;
            }
            if btn.is_active() {
                if win_for_cam.imp().webcam_feed.borrow().is_none() {
                    match LiveFeedCapture::new_webcam_auto() {
                        Ok(cam_feed) => {
                            win_for_cam.imp().webcam_feed.replace(Some(cam_feed));
                            toast_for_cam.add_toast(adw::Toast::new("Webcam preview enabled"));
                        }
                        Err(err) => {
                            cam_guard_cb.set(true);
                            btn.set_active(false);
                            cam_guard_cb.set(false);
                            toast_for_cam.add_toast(adw::Toast::new(&format!("Webcam unavailable: {}", err)));
                        }
                    }
                }
            } else {
                if let Some(cam) = win_for_cam.imp().webcam_feed.borrow_mut().take() {
                    cam.stop_async();
                }
                preview_for_cam.set_webcam_texture(None);
                toast_for_cam.add_toast(adw::Toast::new("Webcam preview disabled"));
            }
        });

        let win_for_mic = window.clone();
        let toast_for_mic = toast_overlay.clone();
        mic_toggle.connect_toggled(move |btn| {
            let enabled = btn.is_active();
            win_for_mic.imp().mic_enabled.set(enabled);
            let msg = if enabled {
                "Microphone enabled"
            } else {
                "Microphone muted"
            };
            toast_for_mic.add_toast(adw::Toast::new(msg));
        });

        // Initialize Camera Styles CSS Provider
        let css_provider = gtk::CssProvider::new();
        css_provider.load_from_data(CAMERA_CSS);
        if let Some(display) = gdk::Display::default() {
            gtk::style_context_add_provider_for_display(
                &display,
                &css_provider,
                gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
            );
        }

        // Bottom Action Bar: Camera Viewfinder OSD, Record/Pause/Stop triggers, and Webcam Size Slider
        let action_bar = gtk::ActionBar::new();

        // Camera Shutter Record Button with camera record symbol
        let btn_record = gtk::Button::new();
        let rec_box = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        let rec_icon = gtk::Image::from_icon_name("media-record-symbolic");
        rec_icon.set_pixel_size(18);
        let rec_label = gtk::Label::new(Some("Record"));
        rec_label.set_css_classes(&["camera-btn-label"]);
        rec_box.append(&rec_icon);
        rec_box.append(&rec_label);
        btn_record.set_child(Some(&rec_box));
        btn_record.set_css_classes(&["pill", "camera-record-btn"]);
        btn_record.set_tooltip_text(Some("Start Recording (Camera Shutter)"));

        // Camera Pause Button with ❚❚ symbol
        let btn_pause = gtk::ToggleButton::new();
        let pause_box = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        let pause_icon = gtk::Image::from_icon_name("media-playback-pause-symbolic");
        pause_icon.set_pixel_size(18);
        let pause_label = gtk::Label::new(Some("Pause"));
        pause_label.set_css_classes(&["camera-btn-label"]);
        pause_box.append(&pause_icon);
        pause_box.append(&pause_label);
        btn_pause.set_child(Some(&pause_box));
        btn_pause.set_css_classes(&["pill", "camera-pause-btn"]);
        btn_pause.set_tooltip_text(Some("Pause / Resume (❚❚)"));
        btn_pause.set_sensitive(false);

        // Camera Stop Button with ◼ symbol
        let btn_stop = gtk::Button::new();
        let stop_box = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        let stop_icon = gtk::Image::from_icon_name("media-playback-stop-symbolic");
        stop_icon.set_pixel_size(18);
        let stop_label = gtk::Label::new(Some("Stop"));
        stop_label.set_css_classes(&["camera-btn-label"]);
        stop_box.append(&stop_icon);
        stop_box.append(&stop_label);
        btn_stop.set_child(Some(&stop_box));
        btn_stop.set_css_classes(&["pill", "camera-stop-btn"]);
        btn_stop.set_tooltip_text(Some("Stop & Save Recording (◼)"));
        btn_stop.set_sensitive(false);

        // Camera Viewfinder OSD Timecode Display (STBY / REC / PAUSE)
        let osd_box = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        osd_box.set_css_classes(&["camera-osd-badge"]);
        let osd_dot = gtk::Label::new(Some("●"));
        osd_dot.set_css_classes(&["camera-osd-dot", "standby"]);
        let osd_text = gtk::Label::new(Some("STBY  00:00:00"));
        osd_text.set_css_classes(&["camera-osd-time"]);
        osd_box.append(&osd_dot);
        osd_box.append(&osd_text);

        // Desktop ScreenCast Preview Button with screen symbol
        let btn_preview_screen = gtk::Button::from_icon_name("video-display-symbolic");
        btn_preview_screen.set_css_classes(&["pill", "camera-preview-btn"]);
        btn_preview_screen.set_tooltip_text(Some("Select Desktop or Window to Preview (ScreenCast)"));

        let left_status_box = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        left_status_box.append(&osd_box);
        left_status_box.append(&status_label);
        action_bar.pack_start(&left_status_box);

        let box_controls = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        box_controls.set_halign(gtk::Align::Center);
        box_controls.append(&btn_preview_screen);
        box_controls.append(&btn_record);
        box_controls.append(&btn_pause);
        box_controls.append(&btn_stop);
        action_bar.set_center_widget(Some(&box_controls));

        // Webcam Size Slider on the right side of the bottom bar
        let size_box = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        let size_label = gtk::Label::new(Some("Webcam Size"));
        size_label.add_css_class("dim-label");
        let size_scale = gtk::Scale::with_range(gtk::Orientation::Horizontal, 0.10, 0.65, 0.01);
        size_scale.set_value(0.24);
        size_scale.set_width_request(150);
        size_scale.set_tooltip_text(Some("Resize Webcam Overlay"));

        let preview_for_scale = preview.clone();
        size_scale.connect_value_changed(move |scale| {
            preview_for_scale.set_overlay_scale(scale.value());
        });

        size_box.append(&size_label);
        size_box.append(&size_scale);
        action_bar.pack_end(&size_box);

        toolbar_view.add_bottom_bar(&action_bar);

        // Elapsed recording timer state & OSD ticker
        let elapsed_seconds = Rc::new(Cell::new(0u64));
        let win_osd = window.clone();
        let elapsed_osd = elapsed_seconds.clone();
        let osd_text_osd = osd_text.clone();
        let osd_dot_osd = osd_dot.clone();
        glib::timeout_add_local(Duration::from_secs(1), move || {
            if win_osd.imp().is_recording.get() {
                if !win_osd.imp().is_paused.get() {
                    let s = elapsed_osd.get() + 1;
                    elapsed_osd.set(s);
                    let hrs = s / 3600;
                    let mins = (s % 3600) / 60;
                    let secs = s % 60;
                    osd_text_osd.set_text(&format!("REC  {:02}:{:02}:{:02}", hrs, mins, secs));
                    osd_dot_osd.set_css_classes(&["camera-osd-dot", "recording"]);
                } else {
                    let s = elapsed_osd.get();
                    let hrs = s / 3600;
                    let mins = (s % 3600) / 60;
                    let secs = s % 60;
                    osd_text_osd.set_text(&format!("PAUSE  {:02}:{:02}:{:02}", hrs, mins, secs));
                    osd_dot_osd.set_css_classes(&["camera-osd-dot", "paused"]);
                }
            } else {
                osd_text_osd.set_text("STBY  00:00:00");
                osd_dot_osd.set_css_classes(&["camera-osd-dot", "standby"]);
            }
            glib::ControlFlow::Continue
        });

        // Shared portal stream holder
        let active_stream_holder: Rc<RefCell<Option<CaptureStream>>> = Rc::new(RefCell::new(None));

        // Wire "Preview Desktop" Button
        let stream_holder_prev = active_stream_holder.clone();
        let win_for_prev = window.clone();
        let source_dd_prev = source_dropdown.clone();
        let status_for_prev = status_label.clone();
        let toast_for_prev = toast_overlay.clone();

        btn_preview_screen.connect_clicked(move |_| {
            let source_type = match source_dd_prev.selected() {
                0 => SourceType::Monitor,
                _ => SourceType::Window,
            };
            let stream_store = stream_holder_prev.clone();
            let win_inner = win_for_prev.clone();
            let status_l = status_for_prev.clone();
            let toast_inner = toast_for_prev.clone();

            glib::spawn_future_local(async move {
                status_l.set_text("Selecting desktop or window for live preview...");
                let (tx, rx) = tokio::sync::oneshot::channel();
                std::thread::spawn(move || {
                    let rt = tokio::runtime::Runtime::new().expect("Failed to create Tokio runtime");
                    let res = rt.block_on(start_screencast_session(
                        None,
                        source_type,
                        CursorMode::Embedded,
                    ));
                    let _ = tx.send(res);
                });

                if let Ok(Ok(stream)) = rx.await {
                    if let Some(old_preview) = win_inner.imp().screen_preview_feed.borrow_mut().take() {
                        old_preview.stop_async();
                    }
                    if let Ok(cloned_fd) = stream.try_clone_fd() {
                        match LiveFeedCapture::new_screen(cloned_fd.into_raw_fd(), stream.node_id) {
                            Ok(feed) => {
                                win_inner.imp().screen_preview_feed.replace(Some(feed));
                                status_l.set_text(&format!(
                                    "Live desktop preview active ({}x{})",
                                    stream.width, stream.height
                                ));
                                toast_inner.add_toast(adw::Toast::new("Live desktop preview active"));
                            }
                            Err(e) => {
                                status_l.set_text(&format!("Preview error: {}", e));
                            }
                        }
                    }
                    stream_store.replace(Some(stream));
                }
            });
        });

        // Wire Record Button
        let toast_for_rec = toast_overlay.clone();
        let preview_for_rec = preview.clone();
        let btn_rec_clone = btn_record.clone();
        let btn_pause_clone = btn_pause.clone();
        let btn_stop_clone = btn_stop.clone();
        let btn_prev_rec_clone = btn_preview_screen.clone();
        let status_for_rec = status_label.clone();
        let source_dd_clone = source_dropdown.clone();
        let codecs_list_rec = supported_codecs.clone();
        let win_for_rec = window.clone();
        let stream_holder_rec = active_stream_holder.clone();
        let elapsed_rec = elapsed_seconds.clone();
        let osd_text_rec = osd_text.clone();
        let osd_dot_rec = osd_dot.clone();

        btn_record.connect_clicked(move |_| {
            let source_type = match source_dd_clone.selected() {
                0 => SourceType::Monitor,
                _ => SourceType::Window,
            };

            let selected_idx = win_for_rec.imp().selected_codec_index.get();
            let (chosen_codec, chosen_container) = codecs_list_rec
                .get(selected_idx)
                .map(|c| (c.codec, c.container))
                .unwrap_or((VideoCodec::H264, ContainerFormat::Mp4));

            let layout = preview_for_rec.layout();
            let shape = preview_for_rec.shape();
            let toast_inner = toast_for_rec.clone();
            let preview_inner = preview_for_rec.clone();
            let btn_r = btn_rec_clone.clone();
            let btn_p = btn_pause_clone.clone();
            let btn_s = btn_stop_clone.clone();
            let btn_prev_inner = btn_prev_rec_clone.clone();
            let status_l = status_for_rec.clone();
            let stream_store = stream_holder_rec.clone();
            let win_inner = win_for_rec.clone();
            let elapsed_inner = elapsed_rec.clone();
            let osd_text_inner = osd_text_rec.clone();
            let osd_dot_inner = osd_dot_rec.clone();

            glib::spawn_future_local(async move {
                let out_dir = glib::user_special_dir(glib::UserDirectory::Videos)
                    .unwrap_or_else(|| PathBuf::from("/tmp"));
                let timestamp = glib::DateTime::now_local()
                    .and_then(|dt| dt.format("%Y-%m-%d_%H-%M-%S"))
                    .unwrap_or_else(|_| "output".into());
                let ext = get_extension_for_container(&chosen_container);
                let out_file = out_dir.join(format!("RecGo-{}.{}", timestamp, ext));
                let mic_on = win_inner.imp().mic_enabled.get();

                // If CameraOnly layout, record camera directly without screen portal prompt
                if layout == LayoutPreset::CameraOnly {
                    status_l.set_text("Starting camera recording...");
                    match CompositorPipeline::new(
                        None,
                        Some("/dev/video0".to_string()),
                        mic_on,
                        layout,
                        shape,
                        chosen_codec,
                        chosen_container,
                        out_file.clone(),
                    ) {
                        Ok(pipeline) => {
                            if let Err(err) = pipeline.start() {
                                status_l.set_text(&format!("Pipeline start error: {}", err));
                                toast_inner.add_toast(adw::Toast::new(&format!("Pipeline error: {}", err)));
                                return;
                            }
                            let enc_desc = pipeline.encoder_description.clone();
                            win_inner.imp().pipeline.replace(Some(pipeline));
                            win_inner.imp().is_recording.set(true);
                            win_inner.imp().is_paused.set(false);
                            elapsed_inner.set(0);
                            osd_text_inner.set_text("REC  00:00:00");
                            osd_dot_inner.set_css_classes(&["camera-osd-dot", "recording"]);
                            preview_inner.set_recording_state(true, false);
                            btn_prev_inner.set_sensitive(false);
                            btn_r.set_sensitive(false);
                            btn_p.set_sensitive(true);
                            btn_s.set_sensitive(true);
                            status_l.set_text(&format!("Recording Camera [{}] -> {}", enc_desc, out_file.display()));
                            toast_inner.add_toast(adw::Toast::new(&format!("Recording camera with {}", enc_desc)));
                        }
                        Err(err) => {
                            status_l.set_text(&format!("Pipeline error: {}", err));
                            toast_inner.add_toast(adw::Toast::new(&format!("GStreamer error: {}", err)));
                        }
                    }
                    return;
                }

                // Check if active screen stream is already available or request via desktop portal
                let existing_stream = if let Some(ref s) = *stream_store.borrow() {
                    s.try_clone_fd().ok().map(|fd| (fd.into_raw_fd(), s.node_id))
                } else {
                    None
                };

                let (screen_fd, screen_node) = match existing_stream {
                    Some(info) => info,
                    None => {
                        status_l.set_text("Requesting screen access...");
                        let (tx, rx) = tokio::sync::oneshot::channel();
                        std::thread::spawn(move || {
                            let rt = tokio::runtime::Runtime::new().expect("Failed to create Tokio runtime");
                            let res = rt.block_on(start_screencast_session(
                                None,
                                source_type,
                                CursorMode::Embedded,
                            ));
                            let _ = tx.send(res);
                        });

                        match rx.await {
                            Ok(Ok(stream)) => {
                                let cloned = stream.try_clone_fd().ok().map(|fd| fd.into_raw_fd());
                                let node = stream.node_id;
                                stream_store.replace(Some(stream));
                                if let Some(cfd) = cloned {
                                    (cfd, node)
                                } else {
                                    status_l.set_text("Failed to duplicate screen stream handle");
                                    return;
                                }
                            }
                            Ok(Err(err)) => {
                                status_l.set_text("Screen selection cancelled or failed");
                                toast_inner.add_toast(adw::Toast::new(&format!("ScreenCast portal: {}", err)));
                                return;
                            }
                            Err(_) => {
                                status_l.set_text("Portal request failed");
                                return;
                            }
                        }
                    }
                };

                let webcam_dev = if win_inner.imp().webcam_feed.borrow().is_some()
                    || layout == LayoutPreset::DesktopPiP
                    || layout == LayoutPreset::SideBySide
                {
                    Some("/dev/video0".to_string())
                } else {
                    None
                };

                match CompositorPipeline::new(
                    Some((screen_fd, screen_node)),
                    webcam_dev,
                    mic_on,
                    layout,
                    shape,
                    chosen_codec,
                    chosen_container,
                    out_file.clone(),
                ) {
                    Ok(pipeline) => {
                        if let Err(err) = pipeline.start() {
                            status_l.set_text(&format!("Pipeline start error: {}", err));
                            toast_inner.add_toast(adw::Toast::new(&format!("Pipeline error: {}", err)));
                            return;
                        }
                        let enc_desc = pipeline.encoder_description.clone();
                        win_inner.imp().pipeline.replace(Some(pipeline));
                        win_inner.imp().is_recording.set(true);
                        win_inner.imp().is_paused.set(false);
                        elapsed_inner.set(0);
                        osd_text_inner.set_text("REC  00:00:00");
                        osd_dot_inner.set_css_classes(&["camera-osd-dot", "recording"]);
                        preview_inner.set_recording_state(true, false);
                        btn_prev_inner.set_sensitive(false);
                        btn_r.set_sensitive(false);
                        btn_p.set_sensitive(true);
                        btn_s.set_sensitive(true);
                        status_l.set_text(&format!("Recording [{}] -> {}", enc_desc, out_file.display()));
                        toast_inner.add_toast(adw::Toast::new(&format!("Recording with {}", enc_desc)));
                    }
                    Err(err) => {
                        status_l.set_text(&format!("Pipeline error: {}", err));
                        toast_inner.add_toast(adw::Toast::new(&format!("GStreamer error: {}", err)));
                    }
                }
            });
        });

        // Wire Pause Button
        let win_for_pause = window.clone();
        let preview_for_pause = preview.clone();
        let status_for_pause = status_label.clone();
        let toast_for_pause = toast_overlay.clone();
        btn_pause.connect_toggled(move |btn| {
            let paused = btn.is_active();
            win_for_pause.imp().is_paused.set(paused);
            preview_for_pause.set_recording_state(true, paused);
            if let Some(ref pipe) = *win_for_pause.imp().pipeline.borrow() {
                if paused {
                    let _ = pipe.pause();
                } else {
                    let _ = pipe.resume();
                }
            }
            if paused {
                status_for_pause.set_text("Recording Paused");
                toast_for_pause.add_toast(adw::Toast::new("Recording paused"));
            } else {
                status_for_pause.set_text("Recording Resumed");
                toast_for_pause.add_toast(adw::Toast::new("Recording resumed"));
            }
        });

        // Wire Stop Button
        let win_for_stop = window.clone();
        let preview_for_stop = preview.clone();
        let btn_rec_stop = btn_record.clone();
        let btn_pause_stop = btn_pause.clone();
        let btn_stop_self = btn_stop.clone();
        let btn_prev_stop = btn_preview_screen.clone();
        let status_for_stop = status_label.clone();
        let toast_for_stop = toast_overlay.clone();
        let elapsed_stop = elapsed_seconds.clone();
        let osd_text_stop = osd_text.clone();
        let osd_dot_stop = osd_dot.clone();

        btn_stop.connect_clicked(move |_| {
            status_for_stop.set_text("Finalizing recording file...");
            btn_stop_self.set_sensitive(false);
            btn_pause_stop.set_sensitive(false);

            let (tx, rx) = tokio::sync::oneshot::channel();
            if let Some(pipe) = win_for_stop.imp().pipeline.borrow_mut().take() {
                pipe.stop_async(move || {
                    let _ = tx.send(());
                });
            } else {
                let _ = tx.send(());
            }

            let win_done = win_for_stop.clone();
            let preview_done = preview_for_stop.clone();
            let btn_r_done = btn_rec_stop.clone();
            let btn_p_done = btn_pause_stop.clone();
            let btn_prev_done = btn_prev_stop.clone();
            let status_done = status_for_stop.clone();
            let toast_done = toast_for_stop.clone();
            let elapsed_done = elapsed_stop.clone();
            let osd_text_done = osd_text_stop.clone();
            let osd_dot_done = osd_dot_stop.clone();

            glib::spawn_future_local(async move {
                let _ = rx.await;
                win_done.imp().is_recording.set(false);
                win_done.imp().is_paused.set(false);
                elapsed_done.set(0);
                osd_text_done.set_text("STBY  00:00:00");
                osd_dot_done.set_css_classes(&["camera-osd-dot", "standby"]);
                preview_done.set_recording_state(false, false);
                btn_p_done.set_active(false);
                btn_r_done.set_sensitive(true);
                btn_prev_done.set_sensitive(true);
                status_done.set_text("Recording saved to Videos folder");
                toast_done.add_toast(adw::Toast::new("Video successfully saved to Videos folder"));
            });
        });

        toast_overlay.set_child(Some(&toolbar_view));
        window.set_content(Some(&toast_overlay));
        window
    }
}
