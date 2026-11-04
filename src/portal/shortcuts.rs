// Global keyboard shortcut integration with ashpd.

use ashpd::desktop::global_shortcuts::{GlobalShortcuts, NewShortcut};
use ashpd::WindowIdentifier;
use std::collections::HashMap;

pub const ACTION_START_STOP: &str = "start-stop-recording";
pub const ACTION_PAUSE_RESUME: &str = "pause-resume-recording";
pub const ACTION_CYCLE_SHAPE: &str = "cycle-webcam-shape";

pub struct ShortcutService {
    _proxy: GlobalShortcuts<'static>,
}

impl ShortcutService {
    pub async fn new() -> Result<Self, ashpd::Error> {
        let proxy = GlobalShortcuts::new().await?;
        let session = proxy.create_session().await?;

        let mut shortcuts = HashMap::new();
        shortcuts.insert(
            ACTION_START_STOP,
            NewShortcut::new(ACTION_START_STOP, "Start or Stop Recording")
                .preferred_trigger("<Control><Alt><Shift>R"),
        );
        shortcuts.insert(
            ACTION_PAUSE_RESUME,
            NewShortcut::new(ACTION_PAUSE_RESUME, "Pause or Resume Recording")
                .preferred_trigger("<Control><Alt><Shift>P"),
        );
        shortcuts.insert(
            ACTION_CYCLE_SHAPE,
            NewShortcut::new(ACTION_CYCLE_SHAPE, "Change Webcam Shape")
                .preferred_trigger("<Control><Alt><Shift>C"),
        );

        let shortcuts_vec: Vec<NewShortcut> = shortcuts.into_values().collect();
        let default_id = WindowIdentifier::default();
        let _ = proxy.bind_shortcuts(&session, &shortcuts_vec, &default_id).await;

        Ok(Self { _proxy: proxy })
    }
}
