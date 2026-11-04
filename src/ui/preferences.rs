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
    pub fn new<F, G>(
        current_codec_idx: usize,
        countdown_secs: u32,
        custom_countdown_secs: u32,
        on_codec_changed: F,
        on_countdown_changed: G,
    ) -> Self
    where
        F: Fn(usize) + 'static,
        G: Fn(u32, u32) + 'static,
    {
        let dialog: Self = glib::Object::builder().build();
        dialog.set_title("Preferences & Diagnostics");

        // Page 1: Recording & Codecs
        let rec_page = adw::PreferencesPage::new();
        rec_page.set_title("Recording");
        rec_page.set_icon_name(Some("media-record-symbolic"));

        // Countdown settings group
        let cd_group = adw::PreferencesGroup::new();
        cd_group.set_title("Countdown Before Recording");
        cd_group.set_description(Some(
            "Prepare before capturing starts with an automatic countdown timer.",
        ));

        let cd_options = [
            "No Delay (0s)",
            "3 seconds",
            "5 seconds",
            "10 seconds",
            "Custom Duration (up to 5 min)",
        ];
        let cd_string_list = gtk::StringList::new(&cd_options);
        let cd_combo_row = adw::ComboRow::new();
        cd_combo_row.set_title("Countdown Preset");
        cd_combo_row.set_subtitle("Time delay after pressing Record");
        cd_combo_row.set_model(Some(&cd_string_list));

        let initial_cd_idx = match countdown_secs {
            0 => 0,
            3 => 1,
            5 => 2,
            10 => 3,
            _ => 4,
        };
        cd_combo_row.set_selected(initial_cd_idx);

        let spin_row = adw::SpinRow::with_range(1.0, 300.0, 1.0);
        spin_row.set_title("Custom Duration (seconds)");
        spin_row.set_subtitle("Configurable from 1 second up to 300 seconds (5 minutes)");
        let initial_spin_val = if countdown_secs != 0 && countdown_secs != 3 && countdown_secs != 5 && countdown_secs != 10 {
            countdown_secs as f64
        } else {
            custom_countdown_secs as f64
        };
        spin_row.set_value(initial_spin_val.clamp(1.0, 300.0));
        spin_row.set_visible(initial_cd_idx == 4);

        let on_cd_cb = std::rc::Rc::new(on_countdown_changed);
        let spin_row_for_combo = spin_row.clone();
        let cb_for_combo = on_cd_cb.clone();
        cd_combo_row.connect_selected_notify(move |row| {
            let selected = row.selected();
            spin_row_for_combo.set_visible(selected == 4);
            let secs = match selected {
                0 => 0,
                1 => 3,
                2 => 5,
                3 => 10,
                _ => spin_row_for_combo.value() as u32,
            };
            cb_for_combo(secs, spin_row_for_combo.value() as u32);
        });

        let cb_for_spin = on_cd_cb.clone();
        let combo_for_spin = cd_combo_row.clone();
        spin_row.adjustment().connect_value_changed(move |adj| {
            if combo_for_spin.selected() == 4 {
                let val = adj.value() as u32;
                cb_for_spin(val, val);
            }
        });

        cd_group.add(&cd_combo_row);
        cd_group.add(&spin_row);
        rec_page.add(&cd_group);

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
