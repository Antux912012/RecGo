// Application object for RecGo.

use adw::subclass::prelude::*;
use gtk::prelude::*;
use gtk::{gio, glib};

use crate::config::APP_ID;
use crate::window::RecGoWindow;

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct RecGoApplication;

    #[glib::object_subclass]
    impl ObjectSubclass for RecGoApplication {
        const NAME: &'static str = "RecGoApplication";
        type Type = super::RecGoApplication;
        type ParentType = adw::Application;
    }

    impl ObjectImpl for RecGoApplication {}

    impl ApplicationImpl for RecGoApplication {
        fn activate(&self) {
            let app = self.obj();
            let window = app.active_window().unwrap_or_else(|| {
                let window = RecGoWindow::new(&*app);
                window.upcast()
            });
            window.present();
        }

        fn startup(&self) {
            self.parent_startup();
            register_bundled_icons();
        }
    }

    impl GtkApplicationImpl for RecGoApplication {}
    impl AdwApplicationImpl for RecGoApplication {}
}

/// Registers the embedded Papirus symbolic icon GResource as a fallback icon path.
///
/// Runs on the GTK main thread during `ApplicationImpl::startup`.
fn register_bundled_icons() {
    const GRESOURCE_BYTES: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/recgo.gresource"));
    let bytes = glib::Bytes::from_static(GRESOURCE_BYTES);
    if let Ok(resource) = gio::Resource::from_data(&bytes) {
        gio::resources_register(&resource);
    }

    if let Some(display) = gtk::gdk::Display::default() {
        let icon_theme = gtk::IconTheme::for_display(&display);
        icon_theme.add_resource_path("/io/github/Antux912012/RecGo/icons/papirus");
    }
}

glib::wrapper! {
    pub struct RecGoApplication(ObjectSubclass<imp::RecGoApplication>)
        @extends gio::Application, gtk::Application, adw::Application,
        @implements gio::ActionGroup, gio::ActionMap;
}

impl RecGoApplication {
    pub fn new() -> Self {
        glib::Object::builder()
            .property("application-id", APP_ID)
            .property("flags", gio::ApplicationFlags::FLAGS_NONE)
            .build()
    }
}
