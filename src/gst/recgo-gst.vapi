[CCode (cprefix = "RecGo", lower_case_cprefix = "recgo_")]
namespace RecGo.Gst {

    [CCode (cname = "RecGoVideoCodec", cprefix = "RECGO_CODEC_", has_type_id = false, cheader_filename = "recgo-codecs.h")]
    public enum VideoCodec {
        H264,
        H265,
        VP8,
        VP9,
        AV1
    }

    [CCode (cname = "RecGoContainerFormat", cprefix = "RECGO_CONTAINER_", has_type_id = false, cheader_filename = "recgo-codecs.h")]
    public enum ContainerFormat {
        MP4,
        WEBM,
        MKV
    }

    [CCode (cname = "RecGoGpuMode", cprefix = "RECGO_GPU_", has_type_id = false, cheader_filename = "recgo-codecs.h")]
    public enum GpuMode {
        VAAPI,
        VULKAN,
        OPENGL,
        SOFTWARE
    }

    [Compact]
    [CCode (cname = "RecGoCodecInfo", free_function = "recgo_codec_info_free", cheader_filename = "recgo-codecs.h")]
    public class CodecInfo {
        public VideoCodec codec;
        public ContainerFormat container;
        public string id;
        public string display_name;
        public string element_name;
        public bool is_hardware;
        public string backend_label;
    }

    [CCode (cname = "recgo_has_gst_element", cheader_filename = "recgo-codecs.h")]
    public bool has_gst_element (string name);

    [CCode (cname = "recgo_detect_gpu_acceleration", cheader_filename = "recgo-codecs.h")]
    public GpuMode detect_gpu_acceleration (out string summary);

    [CCode (cname = "recgo_get_supported_codecs", cheader_filename = "recgo-codecs.h")]
    public GLib.GenericArray<CodecInfo> get_supported_codecs ();

    [CCode (cname = "recgo_get_muxer_for_container", cheader_filename = "recgo-codecs.h")]
    public unowned string get_muxer_for_container (ContainerFormat container);

    [CCode (cname = "recgo_get_extension_for_container", cheader_filename = "recgo-codecs.h")]
    public unowned string get_extension_for_container (ContainerFormat container);

    [CCode (cname = "RecGoDeviceKind", cprefix = "RECGO_DEVICE_", has_type_id = false, cheader_filename = "recgo-devices.h")]
    public enum DeviceKind {
        CAMERA,
        MICROPHONE
    }

    [Compact]
    [CCode (cname = "RecGoDeviceInfo", free_function = "recgo_device_info_free", copy_function = "recgo_device_info_copy", cheader_filename = "recgo-devices.h")]
    public class DeviceInfo {
        public string id;
        public string display_name;
        public string gst_element;
        public string gst_prop;
        public string gst_value;
        public DeviceKind kind;
        public bool is_default;
        public DeviceInfo copy ();
    }

    [CCode (cname = "RecGoDeviceManager", cheader_filename = "recgo-devices.h")]
    public class DeviceManager : GLib.Object {
        [CCode (cname = "recgo_device_manager_new")]
        public DeviceManager ();
        public GLib.GenericArray<DeviceInfo> get_cameras ();
        public GLib.GenericArray<DeviceInfo> get_microphones ();
        public signal void devices_changed ();
    }

    [CCode (cname = "RecGoWebcamShape", cprefix = "RECGO_SHAPE_", has_type_id = false, cheader_filename = "recgo-video-fx.h")]
    public enum WebcamShape {
        RECTANGLE,
        SQUARE,
        CIRCLE
    }

    [CCode (cname = "RecGoWebcamImageConfig", has_type_id = false, cheader_filename = "recgo-video-fx.h")]
    public struct WebcamImageConfig {
        public bool mirror;
        public bool auto_enhance;
        public double brightness;
        public double contrast;
        public double saturation;
        public double warmth;
        public double gamma;
        public double sharpness;
        public double denoise;
        public double zoom;
        public bool border_enabled;
        public bool shadow_enabled;
    }

    [CCode (cname = "recgo_webcam_image_config_init_defaults", cheader_filename = "recgo-video-fx.h")]
    public void webcam_image_config_init_defaults (out WebcamImageConfig cfg);

    [CCode (cname = "RecGoPreviewFeed", cheader_filename = "recgo-preview.h")]
    public class PreviewFeed : GLib.Object {
        [CCode (cname = "recgo_preview_feed_new_camera")]
        public static PreviewFeed? new_camera (DeviceInfo device, WebcamImageConfig? image_cfg = null) throws GLib.Error;

        [CCode (cname = "recgo_preview_feed_new_screen")]
        public static PreviewFeed? new_screen (int pw_fd, uint32 node_id) throws GLib.Error;

        public void update_image_config (WebcamImageConfig image_cfg);
        public void stop ();
        public unowned string? get_device_id ();
        public signal void frame_ready (Gdk.Texture texture);
    }

    [CCode (cname = "RecGoLayoutPreset", cprefix = "RECGO_LAYOUT_", has_type_id = false, cheader_filename = "recgo-recorder.h")]
    public enum LayoutPreset {
        DESKTOP_PIP,
        SIDE_BY_SIDE,
        SCREEN_ONLY,
        CAMERA_ONLY
    }

    [CCode (cname = "RecGoPerformanceMode", cprefix = "RECGO_PERF_", has_type_id = false, cheader_filename = "recgo-recorder.h")]
    public enum PerformanceMode {
        LOW_POWER,
        BALANCED,
        QUALITY
    }

    [CCode (cname = "RecGoRecordConfig", has_type_id = false, cheader_filename = "recgo-recorder.h")]
    public struct RecordConfig {
        public int screen_fd;
        public uint32 screen_node_id;
        public uint screen_width;
        public uint screen_height;
        public unowned GLib.GenericArray<DeviceInfo>? cameras;
        public unowned GLib.GenericArray<DeviceInfo>? microphones;
        public bool mic_enabled;
        public LayoutPreset layout;
        public WebcamShape shape;
        public double overlay_x;
        public double overlay_y;
        public double overlay_scale;
        public WebcamImageConfig image_config;
        public VideoCodec codec;
        public ContainerFormat container;
        public bool prefer_gpu_compositing;
        public uint target_fps;
        public PerformanceMode performance_mode;
        public unowned string output_path;
    }

    [CCode (cname = "RecGoRecorder", cheader_filename = "recgo-recorder.h")]
    public class Recorder : GLib.Object {
        [CCode (cname = "recgo_recorder_new")]
        public Recorder (RecordConfig config) throws GLib.Error;

        public bool start () throws GLib.Error;
        public bool pause () throws GLib.Error;
        public bool resume () throws GLib.Error;
        public void stop_async ();
        public void update_image_config (WebcamImageConfig image_cfg);

        public unowned string get_encoder_description ();
        public unowned PreviewFeed? get_preview_feed ();

        public signal void stopped ();
        public signal void error (string message);
    }
}
