// Screen, monitor, and window capture implementation with XDG Desktop Portal.

use ashpd::desktop::screencast::{CursorMode, Screencast, SourceType};
use ashpd::WindowIdentifier;
use std::os::fd::{AsRawFd, OwnedFd, RawFd};

#[derive(Debug)]
pub struct CaptureStream {
    pub fd: OwnedFd,
    pub node_id: u32,
    pub width: i32,
    pub height: i32,
}

impl CaptureStream {
    pub fn try_clone_fd(&self) -> std::io::Result<OwnedFd> {
        self.fd.try_clone()
    }

    pub fn raw_fd(&self) -> RawFd {
        self.fd.as_raw_fd()
    }
}

pub async fn start_screencast_session(
    window_identifier: Option<&WindowIdentifier>,
    source_type: SourceType,
    cursor_mode: CursorMode,
) -> Result<CaptureStream, ashpd::Error> {
    let proxy = Screencast::new().await?;
    let session = proxy.create_session().await?;

    let default_id = WindowIdentifier::default();
    let root_identifier = window_identifier.unwrap_or(&default_id);

    proxy
        .select_sources(
            &session,
            cursor_mode,
            source_type.into(),
            true, // Multiple sources allowed
            None,
            ashpd::desktop::PersistMode::ExplicitlyRevoked,
        )
        .await?;

    let response = proxy.start(&session, root_identifier).await?.response()?;
    let streams = response.streams();

    let first_stream = streams.first().ok_or_else(|| {
        ashpd::Error::Response(ashpd::desktop::ResponseError::Cancelled)
    })?;

    let node_id = first_stream.pipe_wire_node_id();
    let (width, height) = match first_stream.size() {
        Some((w, h)) => (w, h),
        None => (1920, 1080),
    };

    let fd = proxy.open_pipe_wire_remote(&session).await?;

    Ok(CaptureStream {
        fd,
        node_id,
        width,
        height,
    })
}
