#include "recgo-preview.h"
#include "recgo-codecs.h"
#include <gst/video/video.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

struct _RecGoPreviewFeed {
    GObject parent_instance;

    gchar *device_id;
    GstElement *pipeline;
    GstAppSink *appsink;
    RecGoVideoFx *video_fx;

    GMutex frame_lock;
    GBytes *latest_bytes;
    gint width;
    gint height;
    gint stride;
    gint idle_scheduled; /* atomic */
    gint stopped;        /* atomic */
};

G_DEFINE_FINAL_TYPE (RecGoPreviewFeed, recgo_preview_feed, G_TYPE_OBJECT)

enum {
    SIGNAL_FRAME_READY,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static gboolean
deliver_frame_on_main (gpointer user_data)
{
    RecGoPreviewFeed *self = RECGO_PREVIEW_FEED (user_data);

    g_atomic_int_set (&self->idle_scheduled, 0);

    if (g_atomic_int_get (&self->stopped)) {
        g_object_unref (self);
        return G_SOURCE_REMOVE;
    }

    GBytes *bytes = NULL;
    gint w = 0, h = 0, stride = 0;

    g_mutex_lock (&self->frame_lock);
    if (self->latest_bytes) {
        bytes = self->latest_bytes;
        self->latest_bytes = NULL;
        w = self->width;
        h = self->height;
        stride = self->stride;
    }
    g_mutex_unlock (&self->frame_lock);

    if (bytes && w > 0 && h > 0 && stride > 0) {
        GdkTexture *texture = gdk_memory_texture_new (w,
                                                      h,
                                                      GDK_MEMORY_R8G8B8A8,
                                                      bytes,
                                                      (gsize) stride);
        g_bytes_unref (bytes);
        if (texture) {
            g_signal_emit (self, signals[SIGNAL_FRAME_READY], 0, texture);
            g_object_unref (texture);
        }
    } else if (bytes) {
        g_bytes_unref (bytes);
    }

    g_object_unref (self);
    return G_SOURCE_REMOVE;
}

static GstFlowReturn
on_new_sample (GstAppSink *sink, gpointer user_data)
{
    RecGoPreviewFeed *self = RECGO_PREVIEW_FEED (user_data);
    if (g_atomic_int_get (&self->stopped))
        return GST_FLOW_EOS;

    GstSample *sample = gst_app_sink_pull_sample (sink);
    if (!sample)
        return GST_FLOW_EOS;

    GstBuffer *buffer = gst_sample_get_buffer (sample);
    GstCaps *caps = gst_sample_get_caps (sample);
    GstVideoInfo info;

    if (!buffer || !caps || !gst_video_info_from_caps (&info, caps)) {
        gst_sample_unref (sample);
        return GST_FLOW_ERROR;
    }

    GstMapInfo map;
    if (!gst_buffer_map (buffer, &map, GST_MAP_READ)) {
        gst_sample_unref (sample);
        return GST_FLOW_ERROR;
    }

    /* Single allocation copy outside mutex (eliminates double-copy in preview path) */
    gpointer copy = g_memdup2 (map.data, map.size);
    GBytes *bytes = g_bytes_new_take (copy, map.size);
    gint w = (gint) GST_VIDEO_INFO_WIDTH (&info);
    gint h = (gint) GST_VIDEO_INFO_HEIGHT (&info);
    gint stride = (gint) GST_VIDEO_INFO_PLANE_STRIDE (&info, 0);

    gst_buffer_unmap (buffer, &map);
    gst_sample_unref (sample);

    g_mutex_lock (&self->frame_lock);
    g_clear_pointer (&self->latest_bytes, g_bytes_unref);
    self->latest_bytes = bytes;
    self->width = w;
    self->height = h;
    self->stride = stride;
    g_mutex_unlock (&self->frame_lock);

    if (g_atomic_int_compare_and_exchange (&self->idle_scheduled, 0, 1)) {
        g_idle_add_full (G_PRIORITY_HIGH_IDLE,
                         deliver_frame_on_main,
                         g_object_ref (self),
                         NULL);
    }

    return GST_FLOW_OK;
}

static void
attach_appsink_callbacks (RecGoPreviewFeed *self, GstAppSink *appsink)
{
    self->appsink = g_object_ref (appsink);
    GstAppSinkCallbacks cbs = { 0 };
    cbs.new_sample = on_new_sample;
    gst_app_sink_set_callbacks (appsink, &cbs, self, NULL);
}

static RecGoPreviewFeed *
create_from_launch (const gchar *desc,
                    const gchar *device_id,
                    const RecGoWebcamImageConfig *image_cfg,
                    gboolean *pipeline_created_out,
                    GError **error)
{
    if (pipeline_created_out)
        *pipeline_created_out = FALSE;

    GstElement *el = gst_parse_launch (desc, error);
    if (!el)
        return NULL;

    if (pipeline_created_out)
        *pipeline_created_out = TRUE;

    GstElement *sink_el = gst_bin_get_by_name (GST_BIN (el), "sink");
    if (!sink_el || !GST_IS_APP_SINK (sink_el)) {
        if (sink_el)
            gst_object_unref (sink_el);
        gst_element_set_state (el, GST_STATE_NULL);
        gst_object_unref (el);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Preview pipeline missing appsink");
        return NULL;
    }

    RecGoPreviewFeed *self = g_object_new (RECGO_TYPE_PREVIEW_FEED, NULL);
    self->device_id = g_strdup (device_id);
    self->pipeline = el;

    if (image_cfg) {
        GstElement *fx_el = gst_bin_get_by_name (GST_BIN (el), "camfx");
        if (fx_el) {
            GstPad *pad = gst_element_get_static_pad (fx_el, "src");
            if (pad) {
                self->video_fx = recgo_video_fx_new (image_cfg, FALSE, RECGO_SHAPE_RECTANGLE, 0);
                recgo_video_fx_attach_to_pad (self->video_fx, pad);
                gst_object_unref (pad);
            }
            gst_object_unref (fx_el);
        }
    }

    attach_appsink_callbacks (self, GST_APP_SINK (sink_el));
    gst_object_unref (sink_el);

    GstStateChangeReturn ret = gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        recgo_preview_feed_stop (self);
        g_object_unref (self);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to start preview pipeline");
        return NULL;
    }

    return self;
}

RecGoPreviewFeed *
recgo_preview_feed_new_camera (const RecGoDeviceInfo *device,
                               const RecGoWebcamImageConfig *image_cfg,
                               GError **error)
{
    g_return_val_if_fail (device != NULL, NULL);

    gchar *src_snippet = recgo_device_info_build_source_pipeline (device);
    if (!src_snippet) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Invalid camera device configuration");
        return NULL;
    }

    RecGoWebcamImageConfig default_cfg;
    if (!image_cfg) {
        recgo_webcam_image_config_init_defaults (&default_cfg);
        image_cfg = &default_cfg;
    }

    /* Rate-limit preview to 24fps and use bilinear scaling for low CPU load on older laptops */
    const gchar *rate_stage = recgo_has_gst_element ("videorate")
        ? "videorate drop-only=true max-rate=24 ! "
        : "";
    const gchar *crop_stage = recgo_has_gst_element ("aspectratiocrop")
        ? "aspectratiocrop aspect-ratio=16/9 ! "
        : "";

    gchar *desc = g_strdup_printf (
        "%s ! queue max-size-buffers=2 leaky=downstream ! %s%s"
        "videoconvert n-threads=2 ! videoscale method=1 n-threads=2 ! "
        "video/x-raw,format=RGBA,width=640,height=360,pixel-aspect-ratio=1/1 ! "
        "identity name=camfx ! "
        "appsink name=sink max-buffers=1 drop=true sync=false",
        src_snippet, rate_stage, crop_stage);
    g_free (src_snippet);

    RecGoPreviewFeed *feed = create_from_launch (desc, device->id, image_cfg, NULL, error);
    g_free (desc);
    return feed;
}

RecGoPreviewFeed *
recgo_preview_feed_new_screen (int pw_fd, guint32 node_id, GError **error)
{
    int dup_fd = fcntl (pw_fd, F_DUPFD_CLOEXEC, 3);
    if (dup_fd < 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to duplicate PipeWire file descriptor");
        return NULL;
    }

    const gchar *rate_stage = recgo_has_gst_element ("videorate")
        ? "videorate drop-only=true max-rate=20 ! "
        : "";

    gchar *desc = g_strdup_printf (
        "pipewiresrc fd=%d path=%u do-timestamp=true ! "
        "queue max-size-buffers=2 leaky=downstream ! %s"
        "videoconvert n-threads=2 ! videoscale method=1 add-borders=true n-threads=2 ! "
        "video/x-raw,format=RGBA,width=854,height=480,pixel-aspect-ratio=1/1 ! "
        "appsink name=sink max-buffers=1 drop=true sync=false",
        dup_fd, node_id, rate_stage);

    gboolean pipeline_created = FALSE;
    RecGoPreviewFeed *feed = create_from_launch (desc, "screen", NULL, &pipeline_created, error);
    g_free (desc);

    /* Only close dup_fd if gst_parse_launch failed before pipewiresrc took ownership */
    if (!feed && !pipeline_created) {
        close (dup_fd);
    }
    return feed;
}

RecGoPreviewFeed *
recgo_preview_feed_new_from_appsink (GstAppSink *appsink)
{
    g_return_val_if_fail (GST_IS_APP_SINK (appsink), NULL);
    RecGoPreviewFeed *self = g_object_new (RECGO_TYPE_PREVIEW_FEED, NULL);
    self->device_id = g_strdup ("compositor-preview");
    attach_appsink_callbacks (self, appsink);
    return self;
}

void
recgo_preview_feed_update_image_config (RecGoPreviewFeed *self,
                                        const RecGoWebcamImageConfig *image_cfg)
{
    g_return_if_fail (RECGO_IS_PREVIEW_FEED (self));
    g_return_if_fail (image_cfg != NULL);

    if (self->video_fx) {
        recgo_video_fx_update_image_config (self->video_fx, image_cfg);
    }
}

void
recgo_preview_feed_stop (RecGoPreviewFeed *self)
{
    g_return_if_fail (RECGO_IS_PREVIEW_FEED (self));
    g_atomic_int_set (&self->stopped, 1);

    if (self->appsink) {
        GstAppSinkCallbacks empty = { 0 };
        gst_app_sink_set_callbacks (self->appsink, &empty, NULL, NULL);
        g_clear_object (&self->appsink);
    }

    if (self->pipeline) {
        gst_element_set_state (self->pipeline, GST_STATE_NULL);
        gst_element_get_state (self->pipeline, NULL, NULL, 500 * GST_MSECOND);
        g_clear_object (&self->pipeline);
    }

    g_clear_object (&self->video_fx);

    g_mutex_lock (&self->frame_lock);
    g_clear_pointer (&self->latest_bytes, g_bytes_unref);
    g_mutex_unlock (&self->frame_lock);
}

const gchar *
recgo_preview_feed_get_device_id (RecGoPreviewFeed *self)
{
    g_return_val_if_fail (RECGO_IS_PREVIEW_FEED (self), NULL);
    return self->device_id;
}

static void
recgo_preview_feed_dispose (GObject *object)
{
    RecGoPreviewFeed *self = RECGO_PREVIEW_FEED (object);
    recgo_preview_feed_stop (self);
    G_OBJECT_CLASS (recgo_preview_feed_parent_class)->dispose (object);
}

static void
recgo_preview_feed_finalize (GObject *object)
{
    RecGoPreviewFeed *self = RECGO_PREVIEW_FEED (object);
    g_free (self->device_id);
    g_clear_pointer (&self->latest_bytes, g_bytes_unref);
    g_mutex_clear (&self->frame_lock);
    G_OBJECT_CLASS (recgo_preview_feed_parent_class)->finalize (object);
}

static void
recgo_preview_feed_class_init (RecGoPreviewFeedClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = recgo_preview_feed_dispose;
    object_class->finalize = recgo_preview_feed_finalize;

    signals[SIGNAL_FRAME_READY] = g_signal_new (
        "frame-ready",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, GDK_TYPE_TEXTURE);
}

static void
recgo_preview_feed_init (RecGoPreviewFeed *self)
{
    g_mutex_init (&self->frame_lock);
}
