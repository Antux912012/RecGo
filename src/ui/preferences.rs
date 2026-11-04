// Preferences dialog for RecGo showing codec selection, GPU, Vulkan, OpenGL, and VA-API encoder status.

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
    pub fn new<F>(current_codec_idx: usize, on_codec_changed: F) -> Self
    where
        F: Fn(usize) + 'static,
    {
        let dialog: Self = glib::Object::builder().build();
        dialog.set_title("Preferences & Diagnostics");

        // Page 1: Recording & Codecs
        let rec_page = adw::PreferencesPage::new();
        rec_page.set_title("Recording");
        rec_page.set_icon_name(Some("media-record-symbolic"));

        let enc_choice_group = adw::PreferencesGroup::new();
        enc_choice_group.set_title("Video Encoder &amp; Container Format");
        enc_choice_group.set_description(Some(
            "Select the video format and hardware-accelerated encoder used for recordings.",
        ));

        let supported = get_supported_codecs();
        let titles: Vec<&str> = supported.iter().map(|c| c.display_name.as_str()).collect();
        let string_list = gtk::StringList::new(&titles);

        let combo_row = adw::ComboRow::new();
        combo_row.set_title("Active Video Encoder");
        combo_row.set_subtitle("Hardware GPU encoders (VA-API / Vulkan) minimize CPU overhead");
        combo_row.set_model(Some(&string_list));
        if (current_codec_idx as u32) < (supported.len() as u32) {
            combo_row.set_selected(current_codec_idx as u32);
        }

        let cb = std::rc::Rc::new(on_codec_changed);
        combo_row.connect_selected_notify(move |row| {
            cb(row.selected() as usize);
        });
        enc_choice_group.add(&combo_row);
        rec_page.add(&enc_choice_group);

        // Hardware Acceleration Overview in Recording Page
        let (_mode, accel_summary) = detect_gpu_acceleration();
        let gpu_group = adw::PreferencesGroup::new();
        gpu_group.set_title("Hardware Acceleration Status");
        gpu_group.set_description(Some(&format!("Active Pipelines: {}", accel_summary)));

        let backends = [
            (
                "VA-API Hardware Video Engine (Intel / AMD)",
                has_element("vapostproc") || has_element("vah264enc"),
                "vah264enc / vah265enc / vavp8enc / vapostproc",
            ),
            (
                "Vulkan Compute &amp; Video Acceleration",
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
            let status_badge =
                gtk::Label::new(Some(if active { "ACTIVE (GPU)" } else { "UNAVAILABLE" }));
            status_badge.add_css_class(if active { "success" } else { "dim-label" });
            row.add_suffix(&status_badge);
            gpu_group.add(&row);
        }
        rec_page.add(&gpu_group);
        dialog.add(&rec_page);

        // Page 2: Detected Encoders (Diagnostics)
        let diag_page = adw::PreferencesPage::new();
        diag_page.set_title("Encoders");
        diag_page.set_icon_name(Some("video-display-symbolic"));

        let enc_group = adw::PreferencesGroup::new();
        enc_group.set_title("Available GStreamer Elements");
        enc_group.set_description(Some(
            "Hardware encoders run directly on your GPU with near-zero CPU load.",
        ));

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
        diag_page.add(&enc_group);
        dialog.add(&diag_page);

        dialog
    }
}
