// Main entry point for RecGo application.

mod application;
mod config;
mod pipeline;
mod portal;
mod tray;
mod ui;
mod window;

use adw::prelude::*;
use application::RecGoApplication;

fn main() -> glib::ExitCode {
    gst::init().expect("Failed to initialize GStreamer");

    let app = RecGoApplication::new();
    app.run()
}
