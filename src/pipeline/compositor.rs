// GStreamer video composition, OpenGL/Vulkan/VA-API GPU acceleration, live preview, and recording pipeline.

use gtk::gdk;
use gst::prelude::*;
use std::os::fd::RawFd;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use crate::pipeline::codecs::{
    detect_encoder, get_muxer_for_container, has_element, ContainerFormat, VideoCodec,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LayoutPreset {
    DesktopPiP,
    SideBySide,
    ScreenOnly,
    CameraOnly,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WebcamShape {
    Rectangle = 0,
    Square = 1,
    Circle = 2,
}

pub struct LiveFeedCapture {
    pipeline: gst::Pipeline,
    latest_frame: Arc<Mutex<Option<(Vec<u8>, i32, i32, i32)>>>,
}

impl LiveFeedCapture {
    pub fn new_webcam(device: &str) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        let desc = format!(
            "v4l2src device={} ! videoconvert ! video/x-raw,format=RGBA ! appsink name=sink max-buffers=1 drop=true sync=false",
            device
        );
        Self::from_pipeline_description(&desc)
    }

    pub fn new_webcam_auto() -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        // Prioritize /dev/video0 for standard capture cameras
        for dev in &["/dev/video0", "/dev/video2", "/dev/video1"] {
            if std::path::Path::new(dev).exists() {
                if let Ok(capture) = Self::new_webcam(dev) {
                    eprintln!("RecGo: Started live webcam capture on {}", dev);
                    return Ok(capture);
                }
            }
        }

        // PipeWire camera fallback for sandboxed Flatpak camera portal
        let pipewire_desc = "pipewiresrc ! videoconvert ! video/x-raw,format=RGBA ! appsink name=sink max-buffers=1 drop=true sync=false";
        if let Ok(capture) = Self::from_pipeline_description(pipewire_desc) {
            eprintln!("RecGo: Started live webcam capture via PipeWire");
            return Ok(capture);
        }

        Err("No functional webcam device or PipeWire camera stream found".into())
    }

    pub fn new_screen(fd: RawFd, node_id: u32) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        let desc = format!(
            "pipewiresrc fd={} path={} do-timestamp=true ! videoconvert ! videoscale ! video/x-raw,format=RGBA,width=960,height=540 ! appsink name=sink max-buffers=1 drop=true sync=false",
            fd, node_id
        );
        Self::from_pipeline_description(&desc)
    }

    fn from_pipeline_description(desc: &str) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        let element = gst::parse::launch(desc)?;
        let pipeline = element.dynamic_cast::<gst::Pipeline>().map_err(|_| "Not a pipeline")?;
        let sink = pipeline
            .by_name("sink")
            .ok_or("Missing appsink")?
            .dynamic_cast::<gst_app::AppSink>()
            .map_err(|_| "Not an AppSink")?;

        let latest_frame: Arc<Mutex<Option<(Vec<u8>, i32, i32, i32)>>> = Arc::new(Mutex::new(None));
        let frame_store = latest_frame.clone();

        sink.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |appsink| {
                    let sample = appsink.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                    let buffer = sample.buffer().ok_or(gst::FlowError::Error)?;
                    let caps = sample.caps().ok_or(gst::FlowError::Error)?;
                    let video_info = gst_video::VideoInfo::from_caps(caps)
                        .map_err(|_| gst::FlowError::Error)?;
                    let map = buffer.map_readable().map_err(|_| gst::FlowError::Error)?;

                    let width = video_info.width() as i32;
                    let height = video_info.height() as i32;
                    let stride = video_info.stride()[0];
                    if let Ok(mut guard) = frame_store.lock() {
                        *guard = Some((map.as_slice().to_vec(), width, height, stride));
                    }
                    Ok(gst::FlowSuccess::Ok)
                })
                .build(),
        );

        let _ = pipeline.set_state(gst::State::Playing);
        Ok(Self {
            pipeline,
            latest_frame,
        })
    }

    pub fn latest_texture(&self) -> Option<gdk::MemoryTexture> {
        let mut guard = self.latest_frame.lock().ok()?;
        let (bytes, width, height, stride) = guard.take()?;
        if width <= 0 || height <= 0 || bytes.is_empty() {
            return None;
        }
        let gbytes = glib::Bytes::from_owned(bytes);
        Some(gdk::MemoryTexture::new(
            width,
            height,
            gdk::MemoryFormat::R8g8b8a8,
            &gbytes,
            stride as usize,
        ))
    }

    pub fn stop_async(&self) {
        let pipe = self.pipeline.clone();
        thread::spawn(move || {
            let _ = pipe.set_state(gst::State::Null);
        });
    }
}

pub struct CompositorPipeline {
    pipeline: gst::Pipeline,
    latest_screen_frame: Arc<Mutex<Option<(Vec<u8>, i32, i32, i32)>>>,
    pub encoder_description: String,
}

impl CompositorPipeline {
    pub fn new(
        screen_stream: Option<(RawFd, u32)>,
        webcam_device: Option<String>,
        enable_microphone: bool,
        layout: LayoutPreset,
        _shape: WebcamShape,
        codec: VideoCodec,
        container: ContainerFormat,
        output_path: PathBuf,
    ) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        let (encoder_name, is_hw, backend_label) = detect_encoder(&codec);
        let muxer_spec = match container {
            ContainerFormat::Mp4 => "mp4mux faststart=true fragment-duration=1000",
            ContainerFormat::WebM => "webmmux",
            ContainerFormat::Mkv => "matroskamux",
        };

        // Configure pre-encoder GPU/CPU pipeline stages and parser elements per codec
        let (pre_encode, encoder_opts, post_encode) = match encoder_name.as_str() {
            "vah264enc" | "vah264lpenc" | "vaapih264enc" => {
                let pre = if has_element("vapostproc") {
                    "videoconvert ! vapostproc ! video/x-raw,format=NV12"
                } else {
                    "videoconvert ! video/x-raw,format=NV12"
                };
                (pre, "", "! h264parse")
            }
            "vah265enc" | "vah265lpenc" | "vaapih265enc" => {
                let pre = if has_element("vapostproc") {
                    "videoconvert ! vapostproc ! video/x-raw,format=NV12"
                } else {
                    "videoconvert ! video/x-raw,format=NV12"
                };
                (pre, "", "! h265parse")
            }
            "vavp8enc" | "vaapivp8enc" | "vavp9enc" | "vaapivp9enc" => {
                let pre = if has_element("vapostproc") {
                    "videoconvert ! vapostproc ! video/x-raw,format=NV12"
                } else {
                    "videoconvert ! video/x-raw,format=NV12"
                };
                (pre, "", "")
            }
            "vaav1enc" => {
                let pre = if has_element("vapostproc") {
                    "videoconvert ! vapostproc ! video/x-raw,format=NV12"
                } else {
                    "videoconvert ! video/x-raw,format=NV12"
                };
                (pre, "", "! av1parse")
            }
            "vulkanh264enc" => (
                "videoconvert ! vulkanupload ! vulkancolorconvert",
                "",
                "! h264parse",
            ),
            "vulkanh265enc" => (
                "videoconvert ! vulkanupload ! vulkancolorconvert",
                "",
                "! h265parse",
            ),
            "nvh264enc" => ("videoconvert ! video/x-raw,format=NV12", "", "! h264parse"),
            "nvh265enc" => ("videoconvert ! video/x-raw,format=NV12", "", "! h265parse"),
            "vp8enc" | "vp9enc" => (
                "videoconvert ! video/x-raw,format=I420",
                "deadline=1 cpu-used=4",
                "",
            ),
            "x264enc" => (
                "videoconvert ! video/x-raw,format=I420",
                "speed-preset=ultrafast tune=zerolatency",
                "! h264parse",
            ),
            "x265enc" => (
                "videoconvert ! video/x-raw,format=I420",
                "speed-preset=ultrafast tune=zerolatency",
                "! h265parse",
            ),
            _ => ("videoconvert", "", ""),
        };

        let out_loc = output_path.to_string_lossy().replace('\"', "\\\"");

        // Select matching audio encoder for container (Opus for WebM/MKV, AAC or Opus for MP4)
        let audio_branch = if enable_microphone {
            let audio_enc = if container == ContainerFormat::Mp4 && has_element("avenc_aac") {
                "avenc_aac"
            } else {
                "opusenc"
            };
            format!(
                " pulsesrc do-timestamp=true ! audioconvert ! audioresample ! queue ! {} ! queue ! mux.",
                audio_enc
            )
        } else {
            String::new()
        };

        let video_branch = match layout {
            LayoutPreset::CameraOnly => {
                let dev = webcam_device.unwrap_or_else(|| "/dev/video0".to_string());
                format!(
                    "v4l2src device={dev} ! videoconvert ! tee name=t \
                     t. ! queue ! {pre} ! {enc} {enc_opts} {post} ! queue ! {mux} name=mux ! filesink location=\"{loc}\" \
                     t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! appsink name=previewsink max-buffers=1 drop=true sync=false",
                    dev = dev,
                    pre = pre_encode,
                    enc = encoder_name,
                    enc_opts = encoder_opts,
                    post = post_encode,
                    mux = muxer_spec,
                    loc = out_loc,
                )
            }
            LayoutPreset::ScreenOnly => {
                let (fd, node) = screen_stream
                    .ok_or("Screen stream required for Screen Only recording")?;
                format!(
                    "pipewiresrc fd={fd} path={node} do-timestamp=true ! videoconvert ! tee name=t \
                     t. ! queue ! {pre} ! {enc} {enc_opts} {post} ! queue ! {mux} name=mux ! filesink location=\"{loc}\" \
                     t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! appsink name=previewsink max-buffers=1 drop=true sync=false",
                    fd = fd,
                    node = node,
                    pre = pre_encode,
                    enc = encoder_name,
                    enc_opts = encoder_opts,
                    post = post_encode,
                    mux = muxer_spec,
                    loc = out_loc,
                )
            }
            LayoutPreset::DesktopPiP => {
                if let (Some((fd, node)), Some(dev)) = (screen_stream, webcam_device.as_deref()) {
                    format!(
                        "compositor name=comp background=black \
                           sink_0::xpos=0 sink_0::ypos=0 sink_0::width=1920 sink_0::height=1080 \
                           sink_1::xpos=1420 sink_1::ypos=760 sink_1::width=460 sink_1::height=280 ! \
                         videoconvert ! tee name=t \
                         t. ! queue ! {pre} ! {enc} {enc_opts} {post} ! queue ! {mux} name=mux ! filesink location=\"{loc}\" \
                         t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! appsink name=previewsink max-buffers=1 drop=true sync=false \
                         pipewiresrc fd={fd} path={node} do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! videoscale ! comp.sink_0 \
                         v4l2src device={dev} ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! videoscale ! comp.sink_1",
                        fd = fd,
                        node = node,
                        dev = dev,
                        pre = pre_encode,
                        enc = encoder_name,
                        enc_opts = encoder_opts,
                        post = post_encode,
                        mux = muxer_spec,
                        loc = out_loc,
                    )
                } else if let Some((fd, node)) = screen_stream {
                    format!(
                        "pipewiresrc fd={fd} path={node} do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! tee name=t \
                         t. ! queue ! {pre} ! {enc} {enc_opts} {post} ! queue ! {mux} name=mux ! filesink location=\"{loc}\" \
                         t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! appsink name=previewsink max-buffers=1 drop=true sync=false",
                        fd = fd,
                        node = node,
                        pre = pre_encode,
                        enc = encoder_name,
                        enc_opts = encoder_opts,
                        post = post_encode,
                        mux = muxer_spec,
                        loc = out_loc,
                    )
                } else {
                    return Err("Desktop or Camera stream required for recording".into());
                }
            }
            LayoutPreset::SideBySide => {
                if let (Some((fd, node)), Some(dev)) = (screen_stream, webcam_device.as_deref()) {
                    format!(
                        "compositor name=comp background=black \
                           sink_0::xpos=0 sink_0::ypos=0 sink_0::width=960 sink_0::height=1080 \
                           sink_1::xpos=960 sink_1::ypos=0 sink_1::width=960 sink_1::height=1080 ! \
                         videoconvert ! tee name=t \
                         t. ! queue ! {pre} ! {enc} {enc_opts} {post} ! queue ! {mux} name=mux ! filesink location=\"{loc}\" \
                         t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! appsink name=previewsink max-buffers=1 drop=true sync=false \
                         pipewiresrc fd={fd} path={node} do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! videoscale ! comp.sink_0 \
                         v4l2src device={dev} ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! videoscale ! comp.sink_1",
                        fd = fd,
                        node = node,
                        dev = dev,
                        pre = pre_encode,
                        enc = encoder_name,
                        enc_opts = encoder_opts,
                        post = post_encode,
                        mux = muxer_spec,
                        loc = out_loc,
                    )
                } else if let Some((fd, node)) = screen_stream {
                    format!(
                        "pipewiresrc fd={fd} path={node} do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! tee name=t \
                         t. ! queue ! {pre} ! {enc} {enc_opts} {post} ! queue ! {mux} name=mux ! filesink location=\"{loc}\" \
                         t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! appsink name=previewsink max-buffers=1 drop=true sync=false",
                        fd = fd,
                        node = node,
                        pre = pre_encode,
                        enc = encoder_name,
                        enc_opts = encoder_opts,
                        post = post_encode,
                        mux = muxer_spec,
                        loc = out_loc,
                    )
                } else {
                    return Err("Desktop or Camera stream required for recording".into());
                }
            }
        };

        let launch_str = format!("{}{}", video_branch, audio_branch);
        let element = gst::parse::launch(&launch_str)?;
        let pipeline = element
            .dynamic_cast::<gst::Pipeline>()
            .map_err(|_| "Failed to cast to Pipeline")?;

        let latest_screen_frame: Arc<Mutex<Option<(Vec<u8>, i32, i32, i32)>>> =
            Arc::new(Mutex::new(None));

        if let Some(sink_el) = pipeline.by_name("previewsink") {
            if let Ok(appsink) = sink_el.dynamic_cast::<gst_app::AppSink>() {
                let store = latest_screen_frame.clone();
                appsink.set_callbacks(
                    gst_app::AppSinkCallbacks::builder()
                        .new_sample(move |sink| {
                            let sample = sink.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                            let buffer = sample.buffer().ok_or(gst::FlowError::Error)?;
                            let caps = sample.caps().ok_or(gst::FlowError::Error)?;
                            let info = gst_video::VideoInfo::from_caps(caps)
                                .map_err(|_| gst::FlowError::Error)?;
                            let map = buffer.map_readable().map_err(|_| gst::FlowError::Error)?;

                            if let Ok(mut guard) = store.lock() {
                                *guard = Some((
                                    map.as_slice().to_vec(),
                                    info.width() as i32,
                                    info.height() as i32,
                                    info.stride()[0],
                                ));
                            }
                            Ok(gst::FlowSuccess::Ok)
                        })
                        .build(),
                );
            }
        }

        let encoder_description = format!(
            "{} ({})",
            encoder_name,
            if is_hw { backend_label } else { "Software CPU" }
        );

        Ok(Self {
            pipeline,
            latest_screen_frame,
            encoder_description,
        })
    }

    pub fn latest_screen_texture(&self) -> Option<gdk::MemoryTexture> {
        let mut guard = self.latest_screen_frame.lock().ok()?;
        let (bytes, width, height, stride) = guard.take()?;
        if width <= 0 || height <= 0 || bytes.is_empty() {
            return None;
        }
        let gbytes = glib::Bytes::from_owned(bytes);
        Some(gdk::MemoryTexture::new(
            width,
            height,
            gdk::MemoryFormat::R8g8b8a8,
            &gbytes,
            stride as usize,
        ))
    }

    pub fn start(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        self.pipeline.set_state(gst::State::Playing)?;
        Ok(())
    }

    pub fn pause(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        self.pipeline.set_state(gst::State::Paused)?;
        Ok(())
    }

    pub fn resume(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        self.pipeline.set_state(gst::State::Playing)?;
        Ok(())
    }

    pub fn stop_async<F>(&self, on_complete: F)
    where
        F: FnOnce() + Send + 'static,
    {
        let pipe = self.pipeline.clone();
        thread::spawn(move || {
            // Send EOS to every source element so encoders and muxers flush and finalize
            for item in pipe.iterate_sources() {
                if let Ok(src) = item {
                    src.send_event(gst::event::Eos::new());
                }
            }
            pipe.send_event(gst::event::Eos::new());

            if let Some(bus) = pipe.bus() {
                let deadline = std::time::Instant::now() + Duration::from_millis(3000);
                while std::time::Instant::now() < deadline {
                    if let Some(msg) = bus.timed_pop(gst::ClockTime::from_mseconds(100)) {
                        use gst::MessageView;
                        match msg.view() {
                            MessageView::Eos(..) | MessageView::Error(..) => break,
                            _ => {}
                        }
                    }
                }
            }
            let _ = pipe.set_state(gst::State::Null);
            on_complete();
        });
    }
}
