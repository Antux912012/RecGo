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
        }
    }

    impl GtkApplicationImpl for RecGoApplication {}
    impl AdwApplicationImpl for RecGoApplication {}
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
