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
    // Ignore SIGPIPE so broken IPC sockets (PipeWire / GStreamer) never force-quit the application
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    }

    gst::init().expect("Failed to initialize GStreamer");

    let app = RecGoApplication::new();
    app.run()
}
