#include "recgo-recorder.h"
#include <gst/app/gstappsink.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

typedef struct {
    int screen_fd;
    guint32 screen_node_id;
    guint screen_width;
    guint screen_height;
    GPtrArray *cameras;
    GPtrArray *microphones;
    gboolean mic_enabled;
    RecGoLayoutPreset layout;
    RecGoWebcamShape shape;
    gdouble overlay_x;
    gdouble overlay_y;
    gdouble overlay_scale;
    RecGoWebcamImageConfig image_config;
    RecGoVideoCodec codec;
    RecGoContainerFormat container;
    gboolean prefer_gpu_compositing;
    guint target_fps;
    RecGoPerformanceMode performance_mode;
    gchar *output_path;
} SavedRecordConfig;

struct _RecGoRecorder {
    GObject parent_instance;

    SavedRecordConfig cfg;
    gboolean using_gl;
    gboolean using_hw_enc;

    GstElement *pipeline;
    RecGoPreviewFeed *preview_feed;
    GPtrArray *cam_fx_list; /* element type: RecGoVideoFx* */
    gchar *encoder_description;

    guint bus_watch_id;
    guint stop_timeout_id;
    gboolean stopping;
};

G_DEFINE_FINAL_TYPE (RecGoRecorder, recgo_recorder, G_TYPE_OBJECT)

enum {
    SIGNAL_STOPPED,
    SIGNAL_ERROR,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static inline guint
even_u (guint v)
{
    if (v < 2)
        return 2;
    return v & ~1U;
}

static inline gint
even_i (gint v)
{
    if (v < 2)
        return 2;
    return v & ~1;
}

static GPtrArray *
clone_device_array (GPtrArray *src)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func ((GDestroyNotify) recgo_device_info_free);
    if (!src)
        return arr;
    for (guint i = 0; i < src->len; i++) {
        const RecGoDeviceInfo *d = g_ptr_array_index (src, i);
        if (d)
            g_ptr_array_add (arr, recgo_device_info_copy (d));
    }
    return arr;
}

static void
saved_config_clear (SavedRecordConfig *sc)
{
    if (sc->screen_fd >= 0) {
        close (sc->screen_fd);
        sc->screen_fd = -1;
    }
    g_clear_pointer (&sc->cameras, g_ptr_array_unref);
    g_clear_pointer (&sc->microphones, g_ptr_array_unref);
    g_clear_pointer (&sc->output_path, g_free);
}

static gboolean
validate_output_path (const gchar *path)
{
    if (!path || !*path)
        return FALSE;
    for (const gchar *p = path; *p; p++) {
        if (g_ascii_iscntrl (*p) || *p == '"')
            return FALSE;
    }
    return TRUE;
}

static guint
effective_fps (const SavedRecordConfig *cfg)
{
    guint fps = cfg->target_fps ? CLAMP (cfg->target_fps, 15U, 60U) : 30U;
    if (cfg->performance_mode == RECGO_PERF_LOW_POWER)
        fps = MIN (fps, 24U);
    return fps;
}

static guint
effective_threads (const SavedRecordConfig *cfg)
{
    guint cpus = g_get_num_processors ();
    if (cfg->performance_mode == RECGO_PERF_LOW_POWER)
        return (cpus >= 2) ? 2 : 1;
    return CLAMP (cpus, 1U, 4U);
}

static void
compute_output_dimensions (const SavedRecordConfig *cfg, guint *out_w, guint *out_h)
{
    guint w = MAX (640U, cfg->screen_width ? cfg->screen_width : 1920U);
    guint h = MAX (360U, cfg->screen_height ? cfg->screen_height : 1080U);

    guint max_w = 0;
    if (cfg->performance_mode == RECGO_PERF_LOW_POWER)
        max_w = 1280U;
    else if (cfg->performance_mode == RECGO_PERF_BALANCED)
        max_w = 1920U;

    if (max_w > 0 && w > max_w) {
        gdouble scale = (gdouble) max_w / (gdouble) w;
        w = max_w;
        h = (guint) lround ((gdouble) h * scale);
    }

    *out_w = even_u (MAX (640U, w));
    *out_h = even_u (MAX (360U, h));
}

static void
append_pre_encode (GString *s,
                   const gchar *enc_name,
                   const SavedRecordConfig *cfg,
                   GString *opts_out,
                   const gchar **post_out)
{
    guint fps = effective_fps (cfg);
    guint gop = fps * 2;
    guint threads = effective_threads (cfg);

    guint bitrate_kbps = 5000;
    if (cfg->performance_mode == RECGO_PERF_LOW_POWER)
        bitrate_kbps = 3000;
    else if (cfg->performance_mode == RECGO_PERF_QUALITY)
        bitrate_kbps = 8000;

    guint va_usage = (cfg->performance_mode == RECGO_PERF_LOW_POWER) ? 7 : 6;

    if (g_strcmp0 (enc_name, "vah264enc") == 0 ||
        g_strcmp0 (enc_name, "vah264lpenc") == 0) {
        g_string_append_printf (s, recgo_has_gst_element ("vapostproc")
            ? "videoconvert n-threads=%u ! vapostproc ! video/x-raw,format=NV12"
            : "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_printf (opts_out, "bitrate=%u key-int-max=%u target-usage=%u",
                         bitrate_kbps, gop, va_usage);
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "vaapih264enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_printf (opts_out, "bitrate=%u keyframe-period=%u", bitrate_kbps, gop);
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "vah265enc") == 0 ||
               g_strcmp0 (enc_name, "vah265lpenc") == 0) {
        g_string_append_printf (s, recgo_has_gst_element ("vapostproc")
            ? "videoconvert n-threads=%u ! vapostproc ! video/x-raw,format=NV12"
            : "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_printf (opts_out, "bitrate=%u key-int-max=%u target-usage=%u",
                         bitrate_kbps, gop, va_usage);
        *post_out = "! h265parse";
    } else if (g_strcmp0 (enc_name, "vaapih265enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_printf (opts_out, "bitrate=%u keyframe-period=%u", bitrate_kbps, gop);
        *post_out = "! h265parse";
    } else if (g_strcmp0 (enc_name, "vavp8enc") == 0 ||
               g_strcmp0 (enc_name, "vavp9enc") == 0) {
        g_string_append_printf (s, recgo_has_gst_element ("vapostproc")
            ? "videoconvert n-threads=%u ! vapostproc ! video/x-raw,format=NV12"
            : "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_printf (opts_out, "bitrate=%u key-int-max=%u", bitrate_kbps, gop);
        *post_out = "";
    } else if (g_strcmp0 (enc_name, "vaapivp8enc") == 0 ||
               g_strcmp0 (enc_name, "vaapivp9enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_assign (opts_out, "");
        *post_out = "";
    } else if (g_strcmp0 (enc_name, "vaav1enc") == 0) {
        g_string_append_printf (s, recgo_has_gst_element ("vapostproc")
            ? "videoconvert n-threads=%u ! vapostproc ! video/x-raw,format=NV12"
            : "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_printf (opts_out, "bitrate=%u key-int-max=%u", bitrate_kbps, gop);
        *post_out = "! av1parse";
    } else if (g_strcmp0 (enc_name, "vulkanh264enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! vulkanupload ! vulkancolorconvert", threads);
        g_string_assign (opts_out, "");
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "vulkanh265enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! vulkanupload ! vulkancolorconvert", threads);
        g_string_assign (opts_out, "");
        *post_out = "! h265parse";
    } else if (g_strcmp0 (enc_name, "nvh264enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_printf (opts_out, "bitrate=%u gop-size=%u", bitrate_kbps, gop);
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "nvh265enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=NV12", threads);
        g_string_printf (opts_out, "bitrate=%u gop-size=%u", bitrate_kbps, gop);
        *post_out = "! h265parse";
    } else if (g_strcmp0 (enc_name, "vp8enc") == 0) {
        guint cpu_used = (cfg->performance_mode == RECGO_PERF_QUALITY) ? 4 : 8;
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=I420", threads);
        g_string_printf (opts_out, "deadline=1 cpu-used=%u target-bitrate=%u keyframe-max-dist=%u threads=%u",
                         cpu_used, bitrate_kbps * 1000U, gop, threads);
        *post_out = "";
    } else if (g_strcmp0 (enc_name, "vp9enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=I420", threads);
        g_string_printf (opts_out, "deadline=1 cpu-used=8 target-bitrate=%u keyframe-max-dist=%u threads=%u",
                         bitrate_kbps * 1000U, gop, threads);
        *post_out = "";
    } else if (g_strcmp0 (enc_name, "x264enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=I420", threads);
        g_string_printf (opts_out, "speed-preset=ultrafast tune=zerolatency bitrate=%u key-int-max=%u threads=%u",
                         bitrate_kbps, gop, threads);
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "openh264enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=I420", threads);
        g_string_printf (opts_out, "bitrate=%u complexity=low", bitrate_kbps * 1000U);
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "x265enc") == 0) {
        g_string_append_printf (s, "videoconvert n-threads=%u ! video/x-raw,format=I420", threads);
        g_string_printf (opts_out, "speed-preset=ultrafast tune=zerolatency bitrate=%u key-int-max=%u",
                         bitrate_kbps, gop);
        *post_out = "! h265parse";
    } else {
        g_string_append_printf (s, "videoconvert n-threads=%u", threads);
        g_string_assign (opts_out, "");
        *post_out = "";
    }
}

static void
append_preview_branch (GString *s, const SavedRecordConfig *cfg)
{
    guint pw = 640, ph = 360, pfps = 20;
    if (cfg->performance_mode == RECGO_PERF_LOW_POWER) {
        pw = 480; ph = 270; pfps = 15;
    } else if (cfg->performance_mode == RECGO_PERF_QUALITY) {
        pw = 960; ph = 540; pfps = 24;
    }

    const gchar *rate_part = recgo_has_gst_element ("videorate")
        ? "videorate drop-only=true max-rate=%u ! "
        : "";

    g_string_append (s, " t. ! queue max-size-buffers=1 leaky=downstream ! ");
    if (*rate_part)
        g_string_append_printf (s, rate_part, pfps);
    g_string_append_printf (s,
        "videoscale method=1 ! videoconvert ! video/x-raw,format=RGBA,width=%u,height=%u ! "
        "appsink name=previewsink max-buffers=1 drop=true sync=false",
        pw, ph);
}

static void
append_audio_branch (GString *s, const SavedRecordConfig *cfg)
{
    if (!cfg->mic_enabled || !cfg->microphones || cfg->microphones->len == 0)
        return;

    const gchar *audio_enc = (cfg->container == RECGO_CONTAINER_MP4 && recgo_has_gst_element ("avenc_aac"))
        ? "avenc_aac"
        : "opusenc";

    if (cfg->microphones->len == 1) {
        const RecGoDeviceInfo *mic = g_ptr_array_index (cfg->microphones, 0);
        gchar *src = recgo_device_info_build_source_pipeline (mic);
        if (!src)
            return;
        g_string_append_printf (s,
            " %s ! queue max-size-buffers=12 leaky=downstream ! "
            "audioconvert ! audioresample ! audiorate ! %s ! queue ! mux.",
            src, audio_enc);
        g_free (src);
        return;
    }

    g_string_append_printf (s,
        " audiomixer name=amix ! audioconvert ! audioresample ! %s ! queue ! mux.",
        audio_enc);

    for (guint i = 0; i < cfg->microphones->len; i++) {
        const RecGoDeviceInfo *mic = g_ptr_array_index (cfg->microphones, i);
        gchar *src = recgo_device_info_build_source_pipeline (mic);
        if (!src)
            continue;
        g_string_append_printf (s,
            " %s ! queue max-size-buffers=12 leaky=downstream ! "
            "audioconvert ! audioresample ! audiorate ! amix.sink_%u",
            src, i);
        g_free (src);
    }
}

typedef struct {
    gint x;
    gint y;
    gint w;
    gint h;
    gint mask_inset;
    gboolean apply_mask;
    RecGoWebcamShape mask_shape;
    gboolean mask_shadow;
} CamBox;

static void
compute_pip_box (const SavedRecordConfig *cfg, guint screen_w, guint screen_h, guint index, CamBox *out)
{
    memset (out, 0, sizeof (*out));
    if (index == 0) {
        gdouble cam_w_f, cam_h_f;
        if (cfg->shape == RECGO_SHAPE_CIRCLE || cfg->shape == RECGO_SHAPE_SQUARE) {
            gdouble side = CLAMP ((gdouble) screen_w * cfg->overlay_scale, 120.0, (gdouble) screen_h * 0.85);
            cam_w_f = side;
            cam_h_f = side;
        } else {
            gdouble w = CLAMP ((gdouble) screen_w * cfg->overlay_scale * 1.3, 160.0, (gdouble) screen_w * 0.85);
            cam_w_f = w;
            cam_h_f = w * 9.0 / 16.0;
        }
        gdouble max_x = MAX (0.0, (gdouble) screen_w - cam_w_f);
        gdouble max_y = MAX (0.0, (gdouble) screen_h - cam_h_f);
        gint vis_x = (gint) lround (CLAMP (cfg->overlay_x * max_x, 0.0, max_x));
        gint vis_y = (gint) lround (CLAMP (cfg->overlay_y * max_y, 0.0, max_y));
        gint vis_w = even_i (MAX (64, (gint) lround (cam_w_f)));
        gint vis_h = even_i (MAX (64, (gint) lround (cam_h_f)));

        gint pad = recgo_video_fx_get_shadow_pad (cfg->image_config.shadow_enabled);
        out->x = vis_x - pad;
        out->y = vis_y - pad;
        out->w = even_i (vis_w + 2 * pad);
        out->h = even_i (vis_h + 2 * pad);
        out->mask_inset = pad;
        out->apply_mask = TRUE;
        out->mask_shape = cfg->shape;
        out->mask_shadow = cfg->image_config.shadow_enabled;
    } else {
        gint tile_w = even_i (MAX (160, (gint) (screen_w * 0.18)));
        gint tile_h = even_i ((tile_w * 9) / 16);
        gint margin = 20;
        out->w = tile_w;
        out->h = tile_h;
        out->x = margin + (gint) (index - 1) * (tile_w + 12);
        out->y = MAX (margin, (gint) screen_h - tile_h - margin);
        out->mask_inset = 0;
        out->apply_mask = TRUE;
        out->mask_shape = RECGO_SHAPE_RECTANGLE;
        out->mask_shadow = FALSE;
    }
}

static gboolean
build_pipeline_attempt (RecGoRecorder *self,
                        gboolean use_gl,
                        const gchar *enc_name,
                        const gchar *backend_label,
                        gboolean is_hw_enc,
                        GError **error)
{
    const SavedRecordConfig *cfg = &self->cfg;

    /* Clean up any previous attempt */
    if (self->preview_feed) {
        recgo_preview_feed_stop (self->preview_feed);
        g_clear_object (&self->preview_feed);
    }
    if (self->cam_fx_list)
        g_ptr_array_set_size (self->cam_fx_list, 0);
    if (self->pipeline) {
        gst_element_set_state (self->pipeline, GST_STATE_NULL);
        g_clear_object (&self->pipeline);
    }

    const gchar *muxer_spec = (cfg->container == RECGO_CONTAINER_MP4)
        ? "mp4mux fragment-duration=1000"
        : recgo_get_muxer_for_container (cfg->container);

    GString *pre_encode = g_string_new ("");
    GString *enc_opts = g_string_new ("");
    const gchar *post_encode = "";
    append_pre_encode (pre_encode, enc_name, cfg, enc_opts, &post_encode);

    guint screen_w = 1920, screen_h = 1080;
    compute_output_dimensions (cfg, &screen_w, &screen_h);
    guint fps = effective_fps (cfg);
    guint threads = effective_threads (cfg);

    gboolean has_videorate = recgo_has_gst_element ("videorate");
    gboolean has_aspectcrop = recgo_has_gst_element ("aspectratiocrop");

    guint n_cams = cfg->cameras ? cfg->cameras->len : 0;
    gboolean has_screen = (cfg->screen_fd >= 0 && cfg->layout != RECGO_LAYOUT_CAMERA_ONLY);
    int dup_screen_fd = -1;

    if (has_screen) {
        dup_screen_fd = fcntl (cfg->screen_fd, F_DUPFD_CLOEXEC, 3);
        if (dup_screen_fd < 0) {
            g_string_free (pre_encode, TRUE);
            g_string_free (enc_opts, TRUE);
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to duplicate PipeWire screen FD");
            return FALSE;
        }
    }

    GString *pipeline_str = g_string_new ("");
    GArray *boxes = g_array_new (FALSE, FALSE, sizeof (CamBox));

    if (!has_screen && cfg->layout == RECGO_LAYOUT_CAMERA_ONLY && n_cams == 1) {
        const RecGoDeviceInfo *cam = g_ptr_array_index (cfg->cameras, 0);
        gchar *src = recgo_device_info_build_source_pipeline (cam);
        if (!src) {
            g_array_free (boxes, TRUE);
            g_string_free (pipeline_str, TRUE);
            g_string_free (pre_encode, TRUE);
            g_string_free (enc_opts, TRUE);
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "Invalid camera source configuration");
            return FALSE;
        }

        guint cam_out_w = (cfg->performance_mode == RECGO_PERF_QUALITY) ? 1920 : 1280;
        guint cam_out_h = (cfg->performance_mode == RECGO_PERF_QUALITY) ? 1080 : 720;

        CamBox box = { 0, 0, (gint) cam_out_w, (gint) cam_out_h, 0, FALSE, cfg->shape, FALSE };
        g_array_append_val (boxes, box);

        g_string_append_printf (pipeline_str,
            "%s ! queue max-size-buffers=3 leaky=downstream ! ", src);
        if (has_videorate)
            g_string_append_printf (pipeline_str, "videorate drop-only=true max-rate=%u ! ", fps);
        if (has_aspectcrop)
            g_string_append (pipeline_str, "aspectratiocrop aspect-ratio=16/9 ! ");
        g_string_append_printf (pipeline_str,
            "videoconvert n-threads=%u ! videoscale method=1 n-threads=%u ! "
            "video/x-raw,format=RGBA,width=%u,height=%u ! identity name=camfx_0 ! tee name=t "
            "t. ! queue max-size-buffers=8 ! %s ! %s %s %s ! queue ! %s name=mux ! filesink location=\"%s\"",
            threads, threads, cam_out_w, cam_out_h,
            pre_encode->str, enc_name, enc_opts->str, post_encode, muxer_spec, cfg->output_path);
        append_preview_branch (pipeline_str, cfg);
        g_free (src);
    } else if (has_screen && (cfg->layout == RECGO_LAYOUT_SCREEN_ONLY || n_cams == 0)) {
        g_string_append_printf (pipeline_str,
            "pipewiresrc fd=%d path=%u do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! ",
            dup_screen_fd, cfg->screen_node_id);
        if (has_videorate)
            g_string_append_printf (pipeline_str, "videorate drop-only=true max-rate=%u ! ", fps);
        g_string_append_printf (pipeline_str,
            "videoconvert n-threads=%u ! videoscale method=1 n-threads=%u ! video/x-raw,width=%u,height=%u ! tee name=t "
            "t. ! queue max-size-buffers=8 ! %s ! %s %s %s ! queue ! %s name=mux ! filesink location=\"%s\"",
            threads, threads, screen_w, screen_h,
            pre_encode->str, enc_name, enc_opts->str, post_encode, muxer_spec, cfg->output_path);
        append_preview_branch (pipeline_str, cfg);
    } else {
        /* Multi-input compositing (DesktopPiP, SideBySide, or Multi-Cam CameraOnly) */
        const gchar *post_mixer = use_gl ? "gldownload ! videoconvert" : "videoconvert";
        guint sink_idx = 0;

        if (use_gl) {
            g_string_append (pipeline_str, "glvideomixer name=comp background=black ");
        } else {
            g_string_append_printf (pipeline_str, "compositor name=comp background=black max-threads=%u ", threads);
        }

        if (has_screen) {
            guint sw = even_u ((cfg->layout == RECGO_LAYOUT_SIDE_BY_SIDE) ? (screen_w / 2) : screen_w);
            g_string_append_printf (pipeline_str,
                "sink_0::xpos=0 sink_0::ypos=0 sink_0::width=%u sink_0::height=%u ",
                sw, screen_h);
            sink_idx = 1;
        }

        for (guint i = 0; i < n_cams; i++) {
            CamBox box = { 0 };
            if (cfg->layout == RECGO_LAYOUT_SIDE_BY_SIDE && has_screen) {
                guint half_w = even_u (screen_w / 2);
                guint per_h = even_u (MAX (120U, screen_h / n_cams));
                box.x = (gint) half_w;
                box.y = (gint) (i * per_h);
                box.w = (gint) half_w;
                box.h = (gint) per_h;
                box.apply_mask = FALSE;
            } else if (!has_screen && cfg->layout == RECGO_LAYOUT_CAMERA_ONLY) {
                guint grid_w = (cfg->performance_mode == RECGO_PERF_LOW_POWER) ? 1280 : 1920;
                guint grid_h = (cfg->performance_mode == RECGO_PERF_LOW_POWER) ? 720 : 1080;
                if (n_cams == 2) {
                    box.x = (gint) (i * (grid_w / 2));
                    box.y = 0;
                    box.w = (gint) (grid_w / 2);
                    box.h = (gint) grid_h;
                } else {
                    box.x = (gint) ((i % 2) * (grid_w / 2));
                    box.y = (gint) ((i / 2) * (grid_h / 2));
                    box.w = (gint) (grid_w / 2);
                    box.h = (gint) (grid_h / 2);
                }
                box.apply_mask = FALSE;
            } else {
                compute_pip_box (cfg, screen_w, screen_h, i, &box);
            }
            g_array_append_val (boxes, box);
            g_string_append_printf (pipeline_str,
                "sink_%u::xpos=%d sink_%u::ypos=%d sink_%u::width=%d sink_%u::height=%d ",
                sink_idx + i, box.x,
                sink_idx + i, box.y,
                sink_idx + i, box.w,
                sink_idx + i, box.h);
        }

        g_string_append_printf (pipeline_str,
            "! %s ! tee name=t "
            "t. ! queue max-size-buffers=8 ! %s ! %s %s %s ! queue ! %s name=mux ! filesink location=\"%s\"",
            post_mixer, pre_encode->str, enc_name, enc_opts->str, post_encode, muxer_spec, cfg->output_path);
        append_preview_branch (pipeline_str, cfg);

        if (has_screen) {
            guint sw = even_u ((cfg->layout == RECGO_LAYOUT_SIDE_BY_SIDE) ? (screen_w / 2) : screen_w);
            g_string_append_printf (pipeline_str,
                " pipewiresrc fd=%d path=%u do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! ",
                dup_screen_fd, cfg->screen_node_id);
            if (has_videorate)
                g_string_append_printf (pipeline_str, "videorate drop-only=true max-rate=%u ! ", fps);
            if (use_gl) {
                g_string_append_printf (pipeline_str,
                    "glupload ! glcolorconvert ! glcolorscale ! video/x-raw(memory:GLMemory),width=%u,height=%u ! comp.sink_0",
                    sw, screen_h);
            } else {
                g_string_append_printf (pipeline_str,
                    "videoconvert n-threads=%u ! videoscale method=1 n-threads=%u ! video/x-raw,width=%u,height=%u ! comp.sink_0",
                    threads, threads, sw, screen_h);
            }
        }

        for (guint i = 0; i < n_cams; i++) {
            const RecGoDeviceInfo *cam = g_ptr_array_index (cfg->cameras, i);
            CamBox box = g_array_index (boxes, CamBox, i);
            gchar *src = recgo_device_info_build_source_pipeline (cam);
            if (!src)
                continue;

            g_string_append_printf (pipeline_str,
                " %s ! queue max-size-buffers=3 leaky=downstream ! ", src);
            if (has_videorate)
                g_string_append_printf (pipeline_str, "videorate drop-only=true max-rate=%u ! ", fps);

            if (has_aspectcrop) {
                if (box.apply_mask && (box.mask_shape == RECGO_SHAPE_CIRCLE || box.mask_shape == RECGO_SHAPE_SQUARE)) {
                    g_string_append (pipeline_str, "aspectratiocrop aspect-ratio=1/1 ! ");
                } else if (box.w > 0 && box.h > 0) {
                    g_string_append_printf (pipeline_str, "aspectratiocrop aspect-ratio=%d/%d ! ", box.w, box.h);
                }
            }

            if (use_gl) {
                g_string_append_printf (pipeline_str,
                    "videoconvert n-threads=%u ! videoscale method=1 n-threads=%u ! "
                    "video/x-raw,format=RGBA,width=%d,height=%d ! identity name=camfx_%u ! "
                    "glupload ! glcolorconvert ! comp.sink_%u",
                    threads, threads, box.w, box.h, i, sink_idx + i);
            } else {
                g_string_append_printf (pipeline_str,
                    "videoconvert n-threads=%u ! videoscale method=1 n-threads=%u ! "
                    "video/x-raw,format=RGBA,width=%d,height=%d ! identity name=camfx_%u ! comp.sink_%u",
                    threads, threads, box.w, box.h, i, sink_idx + i);
            }
            g_free (src);
        }
    }

    append_audio_branch (pipeline_str, cfg);

    g_string_free (pre_encode, TRUE);
    g_string_free (enc_opts, TRUE);

    GError *parse_err = NULL;
    GstElement *el = gst_parse_launch (pipeline_str->str, &parse_err);
    g_string_free (pipeline_str, TRUE);

    if (!el) {
        if (dup_screen_fd >= 0)
            close (dup_screen_fd);
        g_array_free (boxes, TRUE);
        g_propagate_error (error, parse_err);
        return FALSE;
    }

    /* Attach RecGoVideoFx pad probes to every camera branch for image FX + shape mask */
    for (guint i = 0; i < boxes->len; i++) {
        CamBox box = g_array_index (boxes, CamBox, i);
        gchar *el_name = g_strdup_printf ("camfx_%u", i);
        GstElement *fx_el = gst_bin_get_by_name (GST_BIN (el), el_name);
        g_free (el_name);
        if (fx_el) {
            GstPad *src_pad = gst_element_get_static_pad (fx_el, "src");
            if (src_pad) {
                RecGoWebcamImageConfig cam_img = cfg->image_config;
                if (!box.mask_shadow)
                    cam_img.shadow_enabled = FALSE;
                RecGoVideoFx *fx = recgo_video_fx_new (&cam_img,
                                                       box.apply_mask,
                                                       box.mask_shape,
                                                       box.mask_inset);
                recgo_video_fx_attach_to_pad (fx, src_pad);
                g_ptr_array_add (self->cam_fx_list, fx);
                gst_object_unref (src_pad);
            }
            gst_object_unref (fx_el);
        }
    }
    g_array_free (boxes, TRUE);

    self->pipeline = el;
    self->using_gl = use_gl;
    self->using_hw_enc = is_hw_enc;

    g_free (self->encoder_description);
    self->encoder_description = g_strdup_printf ("%s (%s%s · %ufps)",
                                                 enc_name,
                                                 backend_label,
                                                 use_gl ? " + GL" : "",
                                                 fps);

    GstElement *sink_el = gst_bin_get_by_name (GST_BIN (el), "previewsink");
    if (sink_el) {
        if (GST_IS_APP_SINK (sink_el))
            self->preview_feed = recgo_preview_feed_new_from_appsink (GST_APP_SINK (sink_el));
        gst_object_unref (sink_el);
    }

    return TRUE;
}

RecGoRecorder *
recgo_recorder_new (const RecGoRecordConfig *cfg, GError **error)
{
    g_return_val_if_fail (cfg != NULL, NULL);

    if (!validate_output_path (cfg->output_path)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Invalid output file path");
        return NULL;
    }

    guint n_cams = cfg->cameras ? cfg->cameras->len : 0;
    gboolean has_screen = (cfg->screen_fd >= 0 && cfg->layout != RECGO_LAYOUT_CAMERA_ONLY);
    if (!has_screen && (cfg->layout == RECGO_LAYOUT_SCREEN_ONLY || n_cams == 0)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Screen stream or at least one camera is required");
        return NULL;
    }

    RecGoRecorder *self = g_object_new (RECGO_TYPE_RECORDER, NULL);
    self->cfg.screen_fd = -1;
    if (has_screen) {
        self->cfg.screen_fd = fcntl (cfg->screen_fd, F_DUPFD_CLOEXEC, 3);
        if (self->cfg.screen_fd < 0) {
            g_object_unref (self);
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to duplicate PipeWire screen FD");
            return NULL;
        }
    }

    self->cfg.screen_node_id = cfg->screen_node_id;
    self->cfg.screen_width = cfg->screen_width;
    self->cfg.screen_height = cfg->screen_height;
    self->cfg.cameras = clone_device_array (cfg->cameras);
    self->cfg.microphones = clone_device_array (cfg->microphones);
    self->cfg.mic_enabled = cfg->mic_enabled;
    self->cfg.layout = cfg->layout;
    self->cfg.shape = cfg->shape;
    self->cfg.overlay_x = cfg->overlay_x;
    self->cfg.overlay_y = cfg->overlay_y;
    self->cfg.overlay_scale = cfg->overlay_scale;
    self->cfg.image_config = cfg->image_config;
    self->cfg.codec = cfg->codec;
    self->cfg.container = cfg->container;
    self->cfg.prefer_gpu_compositing = cfg->prefer_gpu_compositing;
    self->cfg.target_fps = cfg->target_fps;
    self->cfg.performance_mode = cfg->performance_mode;
    self->cfg.output_path = g_strdup (cfg->output_path);

    gchar *enc_name = NULL;
    gboolean is_hw = FALSE;
    gchar *backend_label = NULL;
    recgo_detect_encoder (cfg->codec, &enc_name, &is_hw, &backend_label);

    gboolean use_gl = cfg->prefer_gpu_compositing
                   && (cfg->performance_mode != RECGO_PERF_LOW_POWER)
                   && recgo_has_gst_element ("glvideomixer")
                   && recgo_has_gst_element ("glupload")
                   && recgo_has_gst_element ("gldownload");

    GError *build_err = NULL;
    if (!build_pipeline_attempt (self, use_gl, enc_name, backend_label, is_hw, &build_err)) {
        if (use_gl) {
            g_clear_error (&build_err);
            use_gl = FALSE;
            if (!build_pipeline_attempt (self, FALSE, enc_name, backend_label, is_hw, &build_err) && is_hw) {
                g_clear_error (&build_err);
                gchar *sw_enc = NULL, *sw_label = NULL;
                recgo_detect_software_encoder (cfg->codec, &sw_enc, &sw_label);
                gboolean ok = build_pipeline_attempt (self, FALSE, sw_enc, sw_label, FALSE, &build_err);
                g_free (sw_enc);
                g_free (sw_label);
                if (!ok) {
                    g_free (enc_name);
                    g_free (backend_label);
                    g_object_unref (self);
                    g_propagate_error (error, build_err);
                    return NULL;
                }
            } else if (build_err) {
                g_free (enc_name);
                g_free (backend_label);
                g_object_unref (self);
                g_propagate_error (error, build_err);
                return NULL;
            }
        } else if (is_hw) {
            g_clear_error (&build_err);
            gchar *sw_enc = NULL, *sw_label = NULL;
            recgo_detect_software_encoder (cfg->codec, &sw_enc, &sw_label);
            gboolean ok = build_pipeline_attempt (self, FALSE, sw_enc, sw_label, FALSE, &build_err);
            g_free (sw_enc);
            g_free (sw_label);
            if (!ok) {
                g_free (enc_name);
                g_free (backend_label);
                g_object_unref (self);
                g_propagate_error (error, build_err);
                return NULL;
            }
        } else {
            g_free (enc_name);
            g_free (backend_label);
            g_object_unref (self);
            g_propagate_error (error, build_err);
            return NULL;
        }
    }

    g_free (enc_name);
    g_free (backend_label);
    return self;
}

static void
finish_stop (RecGoRecorder *self)
{
    if (self->stop_timeout_id != 0) {
        g_source_remove (self->stop_timeout_id);
        self->stop_timeout_id = 0;
    }
    if (self->bus_watch_id != 0) {
        g_source_remove (self->bus_watch_id);
        self->bus_watch_id = 0;
    }
    if (self->preview_feed) {
        recgo_preview_feed_stop (self->preview_feed);
        g_clear_object (&self->preview_feed);
    }
    if (self->pipeline) {
        gst_element_set_state (self->pipeline, GST_STATE_NULL);
        gst_element_get_state (self->pipeline, NULL, NULL, 500 * GST_MSECOND);
        g_clear_object (&self->pipeline);
    }
    if (self->cam_fx_list)
        g_ptr_array_set_size (self->cam_fx_list, 0);

    self->stopping = FALSE;
    g_signal_emit (self, signals[SIGNAL_STOPPED], 0);
}

static gboolean
on_bus_message (GstBus *bus, GstMessage *msg, gpointer user_data)
{
    (void) bus;
    RecGoRecorder *self = RECGO_RECORDER (user_data);

    switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
        if (self->stopping)
            finish_stop (self);
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error (msg, &err, &dbg);
        g_signal_emit (self, signals[SIGNAL_ERROR], 0, err ? err->message : "GStreamer pipeline error");
        g_clear_error (&err);
        g_free (dbg);
        /* Always tear down cleanly on error so UI never stays stuck in zombie recording state */
        finish_stop (self);
        break;
    }
    default:
        break;
    }
    return TRUE;
}

static gboolean
on_stop_safety_timeout (gpointer user_data)
{
    RecGoRecorder *self = RECGO_RECORDER (user_data);
    self->stop_timeout_id = 0;
    if (self->stopping)
        finish_stop (self);
    return G_SOURCE_REMOVE;
}

static gboolean
try_start_current_pipeline (RecGoRecorder *self)
{
    if (!self->pipeline)
        return FALSE;
    GstStateChangeReturn ret = gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
        return FALSE;

    /* Wait briefly for preroll (ASYNC_DONE) or immediate error (e.g. broken EGL/VA-API init) */
    GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
    GstMessage *msg = gst_bus_timed_pop_filtered (bus, 350 * GST_MSECOND,
                                                  GST_MESSAGE_ERROR | GST_MESSAGE_ASYNC_DONE);
    gst_object_unref (bus);

    if (msg) {
        gboolean failed = (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        gst_message_unref (msg);
        if (failed) {
            gst_element_set_state (self->pipeline, GST_STATE_NULL);
            return FALSE;
        }
    }
    return TRUE;
}

gboolean
recgo_recorder_start (RecGoRecorder *self, GError **error)
{
    g_return_val_if_fail (RECGO_IS_RECORDER (self), FALSE);
    g_return_val_if_fail (self->pipeline != NULL, FALSE);

    if (!try_start_current_pipeline (self)) {
        /* Automatic reliability fallback for older GPUs / drivers */
        gboolean recovered = FALSE;

        if (self->using_gl) {
            gchar *enc_name = NULL, *backend = NULL;
            gboolean is_hw = FALSE;
            recgo_detect_encoder (self->cfg.codec, &enc_name, &is_hw, &backend);
            if (build_pipeline_attempt (self, FALSE, enc_name, backend, is_hw, NULL)) {
                recovered = try_start_current_pipeline (self);
            }
            g_free (enc_name);
            g_free (backend);
        }

        if (!recovered && self->using_hw_enc) {
            gchar *sw_enc = NULL, *sw_label = NULL;
            recgo_detect_software_encoder (self->cfg.codec, &sw_enc, &sw_label);
            if (build_pipeline_attempt (self, FALSE, sw_enc, sw_label, FALSE, NULL)) {
                recovered = try_start_current_pipeline (self);
            }
            g_free (sw_enc);
            g_free (sw_label);
        }

        if (!recovered) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to transition recording pipeline to PLAYING (including CPU fallback)");
            return FALSE;
        }
    }

    GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
    self->bus_watch_id = gst_bus_add_watch (bus, on_bus_message, self);
    gst_object_unref (bus);
    return TRUE;
}

gboolean
recgo_recorder_pause (RecGoRecorder *self, GError **error)
{
    g_return_val_if_fail (RECGO_IS_RECORDER (self), FALSE);
    if (!self->pipeline)
        return TRUE;
    if (gst_element_set_state (self->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to pause pipeline");
        return FALSE;
    }
    return TRUE;
}

gboolean
recgo_recorder_resume (RecGoRecorder *self, GError **error)
{
    g_return_val_if_fail (RECGO_IS_RECORDER (self), FALSE);
    if (!self->pipeline)
        return TRUE;
    if (gst_element_set_state (self->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to resume pipeline");
        return FALSE;
    }
    return TRUE;
}

void
recgo_recorder_stop_async (RecGoRecorder *self)
{
    g_return_if_fail (RECGO_IS_RECORDER (self));
    if (!self->pipeline || self->stopping) {
        finish_stop (self);
        return;
    }

    self->stopping = TRUE;

    /* Send EOS to every source element so encoders & muxers flush cleanly */
    GstIterator *it = gst_bin_iterate_sources (GST_BIN (self->pipeline));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
        GstElement *src = g_value_get_object (&item);
        if (src)
            gst_element_send_event (src, gst_event_new_eos ());
        g_value_reset (&item);
    }
    g_value_unset (&item);
    gst_iterator_free (it);

    gst_element_send_event (self->pipeline, gst_event_new_eos ());

    /* Safety net (15s) in case a GStreamer plugin stalls on EOS */
    self->stop_timeout_id = g_timeout_add_seconds (15, on_stop_safety_timeout, self);
}

void
recgo_recorder_update_image_config (RecGoRecorder *self,
                                    const RecGoWebcamImageConfig *image_cfg)
{
    g_return_if_fail (RECGO_IS_RECORDER (self));
    g_return_if_fail (image_cfg != NULL);

    self->cfg.image_config = *image_cfg;
    if (!self->cam_fx_list)
        return;

    for (guint i = 0; i < self->cam_fx_list->len; i++) {
        RecGoVideoFx *fx = g_ptr_array_index (self->cam_fx_list, i);
        if (fx) {
            RecGoWebcamImageConfig per_cam = *image_cfg;
            if (i > 0)
                per_cam.shadow_enabled = FALSE;
            recgo_video_fx_update_image_config (fx, &per_cam);
        }
    }
}

const gchar *
recgo_recorder_get_encoder_description (RecGoRecorder *self)
{
    g_return_val_if_fail (RECGO_IS_RECORDER (self), "");
    return self->encoder_description ? self->encoder_description : "";
}

RecGoPreviewFeed *
recgo_recorder_get_preview_feed (RecGoRecorder *self)
{
    g_return_val_if_fail (RECGO_IS_RECORDER (self), NULL);
    return self->preview_feed;
}

static void
recgo_recorder_dispose (GObject *object)
{
    RecGoRecorder *self = RECGO_RECORDER (object);
    if (self->stop_timeout_id != 0) {
        g_source_remove (self->stop_timeout_id);
        self->stop_timeout_id = 0;
    }
    if (self->bus_watch_id != 0) {
        g_source_remove (self->bus_watch_id);
        self->bus_watch_id = 0;
    }
    if (self->preview_feed) {
        recgo_preview_feed_stop (self->preview_feed);
        g_clear_object (&self->preview_feed);
    }
    if (self->pipeline) {
        gst_element_set_state (self->pipeline, GST_STATE_NULL);
        g_clear_object (&self->pipeline);
    }
    g_clear_pointer (&self->cam_fx_list, g_ptr_array_unref);
    saved_config_clear (&self->cfg);
    G_OBJECT_CLASS (recgo_recorder_parent_class)->dispose (object);
}

static void
recgo_recorder_finalize (GObject *object)
{
    RecGoRecorder *self = RECGO_RECORDER (object);
    g_free (self->encoder_description);
    G_OBJECT_CLASS (recgo_recorder_parent_class)->finalize (object);
}

static void
recgo_recorder_class_init (RecGoRecorderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = recgo_recorder_dispose;
    object_class->finalize = recgo_recorder_finalize;

    signals[SIGNAL_STOPPED] = g_signal_new (
        "stopped",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_ERROR] = g_signal_new (
        "error",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
recgo_recorder_init (RecGoRecorder *self)
{
    self->cfg.screen_fd = -1;
    self->cam_fx_list = g_ptr_array_new_with_free_func (g_object_unref);
}
