// D-Bus service and system tray support for RecGo.

use zbus::interface;

pub struct RecGoControlService {
    recording: bool,
    paused: bool,
    elapsed_seconds: u64,
}

impl Default for RecGoControlService {
    fn default() -> Self {
        Self {
            recording: false,
            paused: false,
            elapsed_seconds: 0,
        }
    }
}

#[interface(name = "io.github.antonio.RecGo.Control")]
impl RecGoControlService {
    async fn start(&mut self) -> zbus::fdo::Result<bool> {
        self.recording = true;
        self.paused = false;
        Ok(true)
    }

    async fn pause(&mut self) -> zbus::fdo::Result<bool> {
        if self.recording {
            self.paused = !self.paused;
        }
        Ok(self.paused)
    }

    async fn stop(&mut self) -> zbus::fdo::Result<bool> {
        self.recording = false;
        self.paused = false;
        self.elapsed_seconds = 0;
        Ok(true)
    }

    #[zbus(property)]
    fn state(&self) -> String {
        if !self.recording {
            "idle".to_string()
        } else if self.paused {
            "paused".to_string()
        } else {
            "recording".to_string()
        }
    }

    #[zbus(property)]
    fn elapsed_seconds(&self) -> u64 {
        self.elapsed_seconds
    }
}

pub async fn start_dbus_service() -> Result<zbus::Connection, zbus::Error> {
    let service = RecGoControlService::default();
    let connection = zbus::connection::Builder::session()?
        .name("io.github.antonio.RecGo")?
        .serve_at("/io/github/antonio/RecGo", service)?
        .build()
        .await?;
    Ok(connection)
}
