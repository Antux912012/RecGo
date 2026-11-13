// Hardware-accelerated (VA-API Intel/AMD, Vulkan Video, NVENC) and software video codec detection.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VideoCodec {
    Vp8,
    Vp9,
    H264,
    H265,
    Av1,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ContainerFormat {
    WebM,
    Mp4,
    Mkv,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuAccelerationMode {
    Vaapi,
    Vulkan,
    OpenGl,
    Software,
}

#[derive(Debug, Clone)]
pub struct CodecInfo {
    pub codec: VideoCodec,
    pub container: ContainerFormat,
    pub display_name: String,
    pub element_name: String,
    pub is_hardware_accelerated: bool,
    pub backend_label: &'static str,
}

pub fn has_element(name: &str) -> bool {
    gst::ElementFactory::find(name).is_some()
}

pub fn detect_gpu_acceleration() -> (GpuAccelerationMode, String) {
    let has_vulkan = has_element("vulkanupload") && has_element("vulkancolorconvert");
    let has_opengl = has_element("glupload") && has_element("glcolorconvert");
    let has_vaapi = has_element("vapostproc")
        || has_element("vah264enc")
        || has_element("vah265enc")
        || has_element("vavp8enc")
        || has_element("vaav1enc");

    let mut badges = Vec::new();
    if has_vaapi {
        badges.push("VA-API (Intel/AMD)");
    }
    if has_vulkan {
        badges.push("Vulkan");
    }
    if has_opengl {
        badges.push("OpenGL");
    }

    let summary = if badges.is_empty() {
        "Software CPU".to_string()
    } else {
        badges.join(" + ")
    };

    let mode = if has_vaapi {
        GpuAccelerationMode::Vaapi
    } else if has_vulkan {
        GpuAccelerationMode::Vulkan
    } else if has_opengl {
        GpuAccelerationMode::OpenGl
    } else {
        GpuAccelerationMode::Software
    };

    (mode, summary)
}

pub fn detect_encoder(codec: &VideoCodec) -> (String, bool, &'static str) {
    match codec {
        VideoCodec::H264 => {
            if has_element("vah264enc") {
                ("vah264enc".to_string(), true, "VA-API GPU (Intel/AMD)")
            } else if has_element("vah264lpenc") {
                ("vah264lpenc".to_string(), true, "VA-API Low-Power GPU (Intel/AMD)")
            } else if has_element("vulkanh264enc") {
                ("vulkanh264enc".to_string(), true, "Vulkan Video GPU")
            } else if has_element("vaapih264enc") {
                ("vaapih264enc".to_string(), true, "VA-API Legacy GPU")
            } else if has_element("nvh264enc") {
                ("nvh264enc".to_string(), true, "NVIDIA NVENC GPU")
            } else if has_element("x264enc") {
                ("x264enc".to_string(), false, "Software CPU (x264)")
            } else {
                ("openh264enc".to_string(), false, "Software CPU (OpenH264)")
            }
        }
        VideoCodec::H265 => {
            if has_element("vah265enc") {
                ("vah265enc".to_string(), true, "VA-API GPU (Intel/AMD)")
            } else if has_element("vah265lpenc") {
                ("vah265lpenc".to_string(), true, "VA-API Low-Power GPU (Intel/AMD)")
            } else if has_element("vulkanh265enc") {
                ("vulkanh265enc".to_string(), true, "Vulkan Video GPU")
            } else if has_element("vaapih265enc") {
                ("vaapih265enc".to_string(), true, "VA-API Legacy GPU")
            } else if has_element("nvh265enc") {
                ("nvh265enc".to_string(), true, "NVIDIA NVENC GPU")
            } else {
                ("x265enc".to_string(), false, "Software CPU (x265)")
            }
        }
        VideoCodec::Vp8 => {
            if has_element("vavp8enc") {
                ("vavp8enc".to_string(), true, "VA-API GPU (Intel/AMD)")
            } else if has_element("vaapivp8enc") {
                ("vaapivp8enc".to_string(), true, "VA-API Legacy GPU")
            } else {
                ("vp8enc".to_string(), false, "Software CPU (libvpx)")
            }
        }
        VideoCodec::Vp9 => {
            if has_element("vavp9enc") {
                ("vavp9enc".to_string(), true, "VA-API GPU (Intel/AMD)")
            } else if has_element("vaapivp9enc") {
                ("vaapivp9enc".to_string(), true, "VA-API Legacy GPU")
            } else {
                ("vp9enc".to_string(), false, "Software CPU (libvpx)")
            }
        }
        VideoCodec::Av1 => {
            if has_element("vaav1enc") {
                ("vaav1enc".to_string(), true, "VA-API GPU (Intel/AMD)")
            } else if has_element("vulkanav1enc") {
                ("vulkanav1enc".to_string(), true, "Vulkan Video GPU")
            } else if has_element("nvav1enc") {
                ("nvav1enc".to_string(), true, "NVIDIA NVENC GPU")
            } else {
                ("svtav1enc".to_string(), false, "Software CPU (SVT-AV1)")
            }
        }
    }
}

pub fn get_supported_codecs() -> Vec<CodecInfo> {
    let mut list = Vec::new();

    let candidates = [
        (VideoCodec::H264, ContainerFormat::Mp4, "H.264 / MP4"),
        (VideoCodec::H265, ContainerFormat::Mp4, "H.265 (HEVC) / MP4"),
        (VideoCodec::Vp8, ContainerFormat::WebM, "VP8 / WebM"),
        (VideoCodec::Vp9, ContainerFormat::WebM, "VP9 / WebM (GNOME)"),
        (VideoCodec::Av1, ContainerFormat::Mkv, "AV1 / MKV"),
    ];

    for (codec, container, base_name) in candidates {
        let (element_name, is_hw, backend_label) = detect_encoder(&codec);
        if has_element(&element_name) {
            let accel_tag = if is_hw { "GPU HW" } else { "CPU" };
            let display_name = format!(
                "{} [{} - {}]",
                base_name, accel_tag, element_name
            );
            list.push(CodecInfo {
                codec,
                container,
                display_name,
                element_name,
                is_hardware_accelerated: is_hw,
                backend_label,
            });
        }
    }

    list
}

pub fn get_muxer_for_container(container: &ContainerFormat) -> &'static str {
    match container {
        ContainerFormat::WebM => "webmmux",
        ContainerFormat::Mp4 => "mp4mux",
        ContainerFormat::Mkv => "matroskamux",
    }
}

pub fn get_extension_for_container(container: &ContainerFormat) -> &'static str {
    match container {
        ContainerFormat::WebM => "webm",
        ContainerFormat::Mp4 => "mp4",
        ContainerFormat::Mkv => "mkv",
    }
}
