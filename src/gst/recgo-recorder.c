#include "recgo-recorder.h"
#include <gst/app/gstappsink.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

struct _RecGoRecorder {
    GObject parent_instance;

    GstElement *pipeline;
    RecGoPreviewFeed *preview_feed;
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

static void
append_pre_encode (GString *s, const gchar *enc_name, gchar **opts_out, gchar **post_out)
{
    if (g_strcmp0 (enc_name, "vah264enc") == 0 ||
        g_strcmp0 (enc_name, "vah264lpenc") == 0 ||
        g_strcmp0 (enc_name, "vaapih264enc") == 0) {
        g_string_append (s, recgo_has_gst_element ("vapostproc")
            ? "videoconvert ! vapostproc ! video/x-raw,format=NV12"
            : "videoconvert ! video/x-raw,format=NV12");
        *opts_out = "";
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "vah265enc") == 0 ||
               g_strcmp0 (enc_name, "vah265lpenc") == 0 ||
               g_strcmp0 (enc_name, "vaapih265enc") == 0) {
        g_string_append (s, recgo_has_gst_element ("vapostproc")
            ? "videoconvert ! vapostproc ! video/x-raw,format=NV12"
            : "videoconvert ! video/x-raw,format=NV12");
        *opts_out = "";
        *post_out = "! h265parse";
    } else if (g_strcmp0 (enc_name, "vavp8enc") == 0 ||
               g_strcmp0 (enc_name, "vaapivp8enc") == 0 ||
               g_strcmp0 (enc_name, "vavp9enc") == 0 ||
               g_strcmp0 (enc_name, "vaapivp9enc") == 0) {
        g_string_append (s, recgo_has_gst_element ("vapostproc")
            ? "videoconvert ! vapostproc ! video/x-raw,format=NV12"
            : "videoconvert ! video/x-raw,format=NV12");
        *opts_out = "";
        *post_out = "";
    } else if (g_strcmp0 (enc_name, "vaav1enc") == 0) {
        g_string_append (s, recgo_has_gst_element ("vapostproc")
            ? "videoconvert ! vapostproc ! video/x-raw,format=NV12"
            : "videoconvert ! video/x-raw,format=NV12");
        *opts_out = "";
        *post_out = "! av1parse";
    } else if (g_strcmp0 (enc_name, "vulkanh264enc") == 0) {
        g_string_append (s, "videoconvert ! vulkanupload ! vulkancolorconvert");
        *opts_out = "";
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "vulkanh265enc") == 0) {
        g_string_append (s, "videoconvert ! vulkanupload ! vulkancolorconvert");
        *opts_out = "";
        *post_out = "! h265parse";
    } else if (g_strcmp0 (enc_name, "nvh264enc") == 0) {
        g_string_append (s, "videoconvert ! video/x-raw,format=NV12");
        *opts_out = "";
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "nvh265enc") == 0) {
        g_string_append (s, "videoconvert ! video/x-raw,format=NV12");
        *opts_out = "";
        *post_out = "! h265parse";
    } else if (g_strcmp0 (enc_name, "vp8enc") == 0 || g_strcmp0 (enc_name, "vp9enc") == 0) {
        g_string_append (s, "videoconvert ! video/x-raw,format=I420");
        *opts_out = "deadline=1 cpu-used=4";
        *post_out = "";
    } else if (g_strcmp0 (enc_name, "x264enc") == 0) {
        g_string_append (s, "videoconvert ! video/x-raw,format=I420");
        *opts_out = "speed-preset=ultrafast tune=zerolatency";
        *post_out = "! h264parse";
    } else if (g_strcmp0 (enc_name, "x265enc") == 0) {
        g_string_append (s, "videoconvert ! video/x-raw,format=I420");
        *opts_out = "speed-preset=ultrafast tune=zerolatency";
        *post_out = "! h265parse";
    } else {
        g_string_append (s, "videoconvert");
        *opts_out = "";
        *post_out = "";
    }
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

static void
append_audio_branch (GString *s, const RecGoRecordConfig *cfg)
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
            " %s ! queue max-size-buffers=8 leaky=downstream ! "
            "audioconvert ! audioresample ! audiorate ! %s ! queue ! mux.",
            src, audio_enc);
        g_free (src);
        return;
    }

    /* Multi-microphone mixing via audiomixer */
    g_string_append_printf (s,
        " audiomixer name=amix ! audioconvert ! audioresample ! %s ! queue ! mux.",
        audio_enc);

    for (guint i = 0; i < cfg->microphones->len; i++) {
        const RecGoDeviceInfo *mic = g_ptr_array_index (cfg->microphones, i);
        gchar *src = recgo_device_info_build_source_pipeline (mic);
        if (!src)
            continue;
        g_string_append_printf (s,
            " %s ! queue max-size-buffers=8 leaky=downstream ! "
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
} CamBox;

static void
compute_pip_box (const RecGoRecordConfig *cfg, guint screen_w, guint screen_h, guint index, CamBox *out)
{
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
        out->x = (gint) lround (CLAMP (cfg->overlay_x * max_x, 0.0, max_x));
        out->y = (gint) lround (CLAMP (cfg->overlay_y * max_y, 0.0, max_y));
        out->w = MAX (64, (gint) lround (cam_w_f));
        out->h = MAX (64, (gint) lround (cam_h_f));
    } else {
        /* Secondary cameras tiled neatly along the bottom-left */
        gint tile_w = MAX (160, (gint) (screen_w * 0.18));
        gint tile_h = (tile_w * 9) / 16;
        gint margin = 20;
        out->w = tile_w;
        out->h = tile_h;
        out->x = margin + (gint) (index - 1) * (tile_w + 12);
        out->y = MAX (margin, (gint) screen_h - tile_h - margin);
    }
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

    gchar *enc_name = NULL;
    gboolean is_hw = FALSE;
    gchar *backend_label = NULL;
    recgo_detect_encoder (cfg->codec, &enc_name, &is_hw, &backend_label);

    const gchar *muxer_spec = (cfg->container == RECGO_CONTAINER_MP4)
        ? "mp4mux faststart=true"
        : recgo_get_muxer_for_container (cfg->container);

    GString *pre_encode = g_string_new ("");
    gchar *enc_opts = "";
    gchar *post_encode = "";
    append_pre_encode (pre_encode, enc_name, &enc_opts, &post_encode);

    guint screen_w = MAX (640, cfg->screen_width ? cfg->screen_width : 1920);
    guint screen_h = MAX (480, cfg->screen_height ? cfg->screen_height : 1080);

    guint n_cams = cfg->cameras ? cfg->cameras->len : 0;
    gboolean has_screen = (cfg->screen_fd >= 0 && cfg->layout != RECGO_LAYOUT_CAMERA_ONLY);
    int dup_screen_fd = -1;

    if (has_screen) {
        dup_screen_fd = fcntl (cfg->screen_fd, F_DUPFD_CLOEXEC, 3);
        if (dup_screen_fd < 0) {
            g_string_free (pre_encode, TRUE);
            g_free (enc_name);
            g_free (backend_label);
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to duplicate PipeWire screen FD");
            return NULL;
        }
    } else if (cfg->layout == RECGO_LAYOUT_SCREEN_ONLY || n_cams == 0) {
        g_string_free (pre_encode, TRUE);
        g_free (enc_name);
        g_free (backend_label);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Screen stream or at least one camera is required");
        return NULL;
    }

    gboolean use_gl = cfg->prefer_gpu_compositing
                   && recgo_has_gst_element ("glvideomixer")
                   && recgo_has_gst_element ("glupload")
                   && recgo_has_gst_element ("gldownload");

    GString *pipeline_str = g_string_new ("");

    if (!has_screen && cfg->layout == RECGO_LAYOUT_CAMERA_ONLY && n_cams == 1) {
        const RecGoDeviceInfo *cam = g_ptr_array_index (cfg->cameras, 0);
        gchar *src = recgo_device_info_build_source_pipeline (cam);
        g_string_append_printf (pipeline_str,
            "%s ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! videoscale ! "
            "video/x-raw,width=1920,height=1080 ! tee name=t "
            "t. ! queue ! %s ! %s %s %s ! queue ! %s name=mux ! filesink location=\"%s\" "
            "t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! "
            "appsink name=previewsink max-buffers=1 drop=true sync=false",
            src, pre_encode->str, enc_name, enc_opts, post_encode, muxer_spec, cfg->output_path);
        g_free (src);
    } else if (has_screen && (cfg->layout == RECGO_LAYOUT_SCREEN_ONLY || n_cams == 0)) {
        g_string_append_printf (pipeline_str,
            "pipewiresrc fd=%d path=%u do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! "
            "videoconvert ! videoscale ! video/x-raw,width=%u,height=%u ! tee name=t "
            "t. ! queue ! %s ! %s %s %s ! queue ! %s name=mux ! filesink location=\"%s\" "
            "t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! "
            "appsink name=previewsink max-buffers=1 drop=true sync=false",
            dup_screen_fd, cfg->screen_node_id, screen_w, screen_h,
            pre_encode->str, enc_name, enc_opts, post_encode, muxer_spec, cfg->output_path);
    } else {
        /* Multi-input compositing (DesktopPiP, SideBySide, or Multi-Cam CameraOnly) */
        const gchar *mixer_name = use_gl ? "glvideomixer" : "compositor";
        const gchar *post_mixer = use_gl ? "gldownload ! videoconvert" : "videoconvert";
        guint sink_idx = 0;

        g_string_append_printf (pipeline_str, "%s name=comp background=black ", mixer_name);

        if (has_screen) {
            guint sw = (cfg->layout == RECGO_LAYOUT_SIDE_BY_SIDE) ? (screen_w / 2) : screen_w;
            g_string_append_printf (pipeline_str,
                "sink_0::xpos=0 sink_0::ypos=0 sink_0::width=%u sink_0::height=%u ",
                sw, screen_h);
            sink_idx = 1;
        }

        GArray *boxes = g_array_new (FALSE, FALSE, sizeof (CamBox));
        for (guint i = 0; i < n_cams; i++) {
            CamBox box = { 0 };
            if (cfg->layout == RECGO_LAYOUT_SIDE_BY_SIDE && has_screen) {
                guint half_w = screen_w / 2;
                guint per_h = MAX (120, screen_h / n_cams);
                box.x = (gint) half_w;
                box.y = (gint) (i * per_h);
                box.w = (gint) half_w;
                box.h = (gint) per_h;
            } else if (!has_screen && cfg->layout == RECGO_LAYOUT_CAMERA_ONLY) {
                /* Multi-cam grid on 1920x1080 */
                if (n_cams == 2) {
                    box.x = (gint) (i * 960);
                    box.y = 0;
                    box.w = 960;
                    box.h = 1080;
                } else {
                    box.x = (gint) ((i % 2) * 960);
                    box.y = (gint) ((i / 2) * 540);
                    box.w = 960;
                    box.h = 540;
                }
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
            "t. ! queue ! %s ! %s %s %s ! queue ! %s name=mux ! filesink location=\"%s\" "
            "t. ! queue max-size-buffers=1 leaky=downstream ! videoconvert ! video/x-raw,format=RGBA ! "
            "appsink name=previewsink max-buffers=1 drop=true sync=false",
            post_mixer, pre_encode->str, enc_name, enc_opts, post_encode, muxer_spec, cfg->output_path);

        if (has_screen) {
            guint sw = (cfg->layout == RECGO_LAYOUT_SIDE_BY_SIDE) ? (screen_w / 2) : screen_w;
            if (use_gl) {
                g_string_append_printf (pipeline_str,
                    " pipewiresrc fd=%d path=%u do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! "
                    "glupload ! glcolorconvert ! glcolorscale ! video/x-raw(memory:GLMemory),width=%u,height=%u ! comp.sink_0",
                    dup_screen_fd, cfg->screen_node_id, sw, screen_h);
            } else {
                g_string_append_printf (pipeline_str,
                    " pipewiresrc fd=%d path=%u do-timestamp=true ! queue max-size-buffers=3 leaky=downstream ! "
                    "videoconvert ! videoscale ! video/x-raw,width=%u,height=%u ! comp.sink_0",
                    dup_screen_fd, cfg->screen_node_id, sw, screen_h);
            }
        }

        for (guint i = 0; i < n_cams; i++) {
            const RecGoDeviceInfo *cam = g_ptr_array_index (cfg->cameras, i);
            CamBox box = g_array_index (boxes, CamBox, i);
            gchar *src = recgo_device_info_build_source_pipeline (cam);
            if (!src)
                continue;
            if (use_gl) {
                g_string_append_printf (pipeline_str,
                    " %s ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! videoscale ! "
                    "video/x-raw,width=%d,height=%d ! glupload ! glcolorconvert ! comp.sink_%u",
                    src, box.w, box.h, sink_idx + i);
            } else {
                g_string_append_printf (pipeline_str,
                    " %s ! queue max-size-buffers=3 leaky=downstream ! videoconvert ! videoscale ! "
                    "video/x-raw,width=%d,height=%d ! comp.sink_%u",
                    src, box.w, box.h, sink_idx + i);
            }
            g_free (src);
        }
        g_array_free (boxes, TRUE);
    }

    append_audio_branch (pipeline_str, cfg);

    g_string_free (pre_encode, TRUE);

    GError *parse_err = NULL;
    GstElement *el = gst_parse_launch (pipeline_str->str, &parse_err);
    g_string_free (pipeline_str, TRUE);

    if (!el) {
        if (dup_screen_fd >= 0)
            close (dup_screen_fd);
        g_propagate_error (error, parse_err);
        g_free (enc_name);
        g_free (backend_label);
        return NULL;
    }

    RecGoRecorder *self = g_object_new (RECGO_TYPE_RECORDER, NULL);
    self->pipeline = el;
    self->encoder_description = g_strdup_printf ("%s (%s%s)",
                                                 enc_name,
                                                 backend_label,
                                                 use_gl ? " + GL" : "");
    g_free (enc_name);
    g_free (backend_label);

    GstElement *sink_el = gst_bin_get_by_name (GST_BIN (el), "previewsink");
    if (sink_el) {
        if (GST_IS_APP_SINK (sink_el))
            self->preview_feed = recgo_preview_feed_new_from_appsink (GST_APP_SINK (sink_el));
        gst_object_unref (sink_el);
    }

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
        if (self->stopping)
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

gboolean
recgo_recorder_start (RecGoRecorder *self, GError **error)
{
    g_return_val_if_fail (RECGO_IS_RECORDER (self), FALSE);
    g_return_val_if_fail (self->pipeline != NULL, FALSE);

    GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
    self->bus_watch_id = gst_bus_add_watch (bus, on_bus_message, self);
    gst_object_unref (bus);

    GstStateChangeReturn ret = gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to transition recording pipeline to PLAYING");
        return FALSE;
    }
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

const gchar *
recgo_recorder_get_encoder_description (RecGoRecorder *self)
{
    g_return_val_if_fail (RECGO_IS_RECORDER (self), "");
    return self->encoder_description;
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
    (void) self;
}
