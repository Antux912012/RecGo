// Screen, monitor, and window capture implementation with XDG Desktop Portal.

use ashpd::desktop::screencast::{CursorMode, Screencast, SourceType};
use ashpd::WindowIdentifier;
use std::os::fd::{AsRawFd, OwnedFd, RawFd};
use std::sync::OnceLock;

static PORTAL_RUNTIME: OnceLock<tokio::runtime::Runtime> = OnceLock::new();

pub fn portal_runtime() -> &'static tokio::runtime::Runtime {
    PORTAL_RUNTIME.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("Failed to initialize background Tokio runtime")
    })
}

#[derive(Debug)]
pub struct CaptureStream {
    pub fd: OwnedFd,
    pub node_id: u32,
    pub width: i32,
    pub height: i32,
    _close_tx: Option<tokio::sync::oneshot::Sender<()>>,
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
    source_type: SourceType,
    cursor_mode: CursorMode,
) -> Result<CaptureStream, ashpd::Error> {
    let (ready_tx, ready_rx) = tokio::sync::oneshot::channel();
    let (close_tx, close_rx) = tokio::sync::oneshot::channel();

    portal_runtime().spawn(async move {
        let proxy = match Screencast::new().await {
            Ok(p) => p,
            Err(e) => {
                let _ = ready_tx.send(Err(e));
                return;
            }
        };

        let session = match proxy.create_session().await {
            Ok(s) => s,
            Err(e) => {
                let _ = ready_tx.send(Err(e));
                return;
            }
        };

        let default_id = WindowIdentifier::default();

        if let Err(e) = proxy
            .select_sources(
                &session,
                cursor_mode,
                source_type.into(),
                true,
                None,
                ashpd::desktop::PersistMode::ExplicitlyRevoked,
            )
            .await
        {
            let _ = ready_tx.send(Err(e));
            return;
        }

        let start_res = match proxy.start(&session, &default_id).await {
            Ok(res) => res,
            Err(e) => {
                let _ = ready_tx.send(Err(e));
                return;
            }
        };

        let response = match start_res.response() {
            Ok(r) => r,
            Err(e) => {
                let _ = ready_tx.send(Err(e.into()));
                return;
            }
        };

        let streams = response.streams();
        let first_stream = match streams.first() {
            Some(s) => s,
            None => {
                let _ = ready_tx.send(Err(ashpd::Error::Response(
                    ashpd::desktop::ResponseError::Cancelled,
                )));
                return;
            }
        };

        let node_id = first_stream.pipe_wire_node_id();
        let (width, height) = match first_stream.size() {
            Some((w, h)) => (w, h),
            None => (1920, 1080),
        };

        let fd = match proxy.open_pipe_wire_remote(&session).await {
            Ok(fd) => fd,
            Err(e) => {
                let _ = ready_tx.send(Err(e));
                return;
            }
        };

        // Stream parameters ready; send to caller
        let _ = ready_tx.send(Ok((fd, node_id, width, height)));

        // Keep proxy and session alive until CaptureStream is dropped
        let _ = close_rx.await;
        let _ = session.close().await;
    });

    let (fd, node_id, width, height) = ready_rx
        .await
        .map_err(|_| ashpd::Error::Response(ashpd::desktop::ResponseError::Cancelled))??;

    Ok(CaptureStream {
        fd,
        node_id,
        width,
        height,
        _close_tx: Some(close_tx),
    })
}
