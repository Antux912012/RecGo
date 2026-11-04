// Preferences dialog for RecGo showing real-time GPU, Vulkan, OpenGL, and VA-API encoder status.

use adw::prelude::*;
use adw::subclass::prelude::*;
use gtk::glib;

use crate::pipeline::codecs::{detect_gpu_acceleration, get_supported_codecs, has_element};

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct PreferencesDialog;

    #[glib::object_subclass]
    impl ObjectSubclass for PreferencesDialog {
        const NAME: &'static str = "RecGoPreferencesDialog";
        type Type = super::PreferencesDialog;
        type ParentType = adw::PreferencesDialog;
    }

    impl ObjectImpl for PreferencesDialog {}
    impl WidgetImpl for PreferencesDialog {}
    impl AdwDialogImpl for PreferencesDialog {}
    impl PreferencesDialogImpl for PreferencesDialog {}
}

glib::wrapper! {
    pub struct PreferencesDialog(ObjectSubclass<imp::PreferencesDialog>)
        @extends gtk::Widget, adw::Dialog, adw::PreferencesDialog;
}

impl PreferencesDialog {
    pub fn new() -> Self {
        let dialog: Self = glib::Object::builder().build();
        dialog.set_title("GPU & Encoding Diagnostics");

        let page = adw::PreferencesPage::new();
        page.set_title("GPU & Encoders");
        page.set_icon_name(Some("video-display-symbolic"));

        let (_mode, accel_summary) = detect_gpu_acceleration();

        let gpu_group = adw::PreferencesGroup::new();
        gpu_group.set_title("Hardware Acceleration Backends (Intel / AMD / Vulkan / OpenGL)");
        gpu_group.set_description(Some(&format!("Active Pipelines: {}", accel_summary)));

        let backends = [
            (
                "VA-API Hardware Video Engine (Intel / AMD)",
                has_element("vapostproc") || has_element("vah264enc"),
                "vah264enc / vah265enc / vavp8enc / vapostproc",
            ),
            (
                "Vulkan Compute & Video Acceleration",
                has_element("vulkanupload") && has_element("vulkancolorconvert"),
                "vulkanupload / vulkancolorconvert / vulkanshaderspv",
            ),
            (
                "OpenGL / GLSL Shader Compositor",
                has_element("glupload") && has_element("glshader"),
                "glupload / glcolorconvert / glvideomixer / glshader",
            ),
        ];

        for (title, active, subtitle) in backends {
            let row = adw::ActionRow::new();
            row.set_title(title);
            row.set_subtitle(subtitle);
            let status_badge = gtk::Label::new(Some(if active { "ACTIVE (GPU)" } else { "UNAVAILABLE" }));
            status_badge.add_css_class(if active { "success" } else { "dim-label" });
            row.add_suffix(&status_badge);
            gpu_group.add(&row);
        }

        page.add(&gpu_group);

        let enc_group = adw::PreferencesGroup::new();
        enc_group.set_title("Detected Video Encoders");
        enc_group.set_description(Some("Hardware encoders run directly on your GPU with near-zero CPU load"));

        for info in get_supported_codecs() {
            let row = adw::ActionRow::new();
            row.set_title(&info.display_name);
            row.set_subtitle(&format!(
                "GStreamer element: {} — {}",
                info.element_name, info.backend_label
            ));
            let badge = gtk::Label::new(Some(if info.is_hardware_accelerated {
                "GPU Hardware"
            } else {
                "CPU Software"
            }));
            badge.add_css_class(if info.is_hardware_accelerated {
                "accent"
            } else {
                "dim-label"
            });
            row.add_suffix(&badge);
            enc_group.add(&row);
        }

        page.add(&enc_group);
        dialog.add(&page);

        dialog
    }
}
