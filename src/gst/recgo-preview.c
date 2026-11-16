#include "recgo-preview.h"
#include <gst/video/video.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

struct _RecGoPreviewFeed {
    GObject parent_instance;

    gchar *device_id;
    GstElement *pipeline;
    GstAppSink *appsink;

    GMutex frame_lock;
    guint8 *staging_data;
    gsize staging_cap;
    gsize staging_len;
    gint width;
    gint height;
    gint stride;
    gboolean has_new_frame;
    gint idle_scheduled; /* atomic */
    gboolean stopped;
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

    if (self->stopped) {
        g_object_unref (self);
        return G_SOURCE_REMOVE;
    }

    GdkTexture *texture = NULL;

    g_mutex_lock (&self->frame_lock);
    if (self->has_new_frame && self->staging_data && self->staging_len > 0
        && self->width > 0 && self->height > 0) {
        gpointer copy = g_memdup2 (self->staging_data, self->staging_len);
        GBytes *bytes = g_bytes_new_take (copy, self->staging_len);
        texture = gdk_memory_texture_new (self->width,
                                          self->height,
                                          GDK_MEMORY_R8G8B8A8,
                                          bytes,
                                          (gsize) self->stride);
        g_bytes_unref (bytes);
        self->has_new_frame = FALSE;
    }
    g_mutex_unlock (&self->frame_lock);

    if (texture) {
        g_signal_emit (self, signals[SIGNAL_FRAME_READY], 0, texture);
        g_object_unref (texture);
    }

    g_object_unref (self);
    return G_SOURCE_REMOVE;
}

static GstFlowReturn
on_new_sample (GstAppSink *sink, gpointer user_data)
{
    RecGoPreviewFeed *self = RECGO_PREVIEW_FEED (user_data);
    if (self->stopped)
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

    g_mutex_lock (&self->frame_lock);
    if (map.size > self->staging_cap) {
        g_free (self->staging_data);
        self->staging_data = g_malloc (map.size);
        self->staging_cap = map.size;
    }
    memcpy (self->staging_data, map.data, map.size);
    self->staging_len = map.size;
    self->width = (gint) GST_VIDEO_INFO_WIDTH (&info);
    self->height = (gint) GST_VIDEO_INFO_HEIGHT (&info);
    self->stride = (gint) GST_VIDEO_INFO_PLANE_STRIDE (&info, 0);
    self->has_new_frame = TRUE;
    g_mutex_unlock (&self->frame_lock);

    gst_buffer_unmap (buffer, &map);
    gst_sample_unref (sample);

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
create_from_launch (const gchar *desc, const gchar *device_id, GError **error)
{
    GstElement *el = gst_parse_launch (desc, error);
    if (!el)
        return NULL;

    GstElement *sink_el = gst_bin_get_by_name (GST_BIN (el), "sink");
    if (!sink_el || !GST_IS_APP_SINK (sink_el)) {
        if (sink_el)
            gst_object_unref (sink_el);
        gst_object_unref (el);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Preview pipeline missing appsink");
        return NULL;
    }

    RecGoPreviewFeed *self = g_object_new (RECGO_TYPE_PREVIEW_FEED, NULL);
    self->device_id = g_strdup (device_id);
    self->pipeline = el;
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
recgo_preview_feed_new_camera (const RecGoDeviceInfo *device, GError **error)
{
    g_return_val_if_fail (device != NULL, NULL);

    gchar *src_snippet = recgo_device_info_build_source_pipeline (device);
    if (!src_snippet) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Invalid camera device configuration");
        return NULL;
    }

    gchar *desc = g_strdup_printf (
        "%s ! videoconvert ! videoscale ! video/x-raw,format=RGBA,width=640,height=360,pixel-aspect-ratio=1/1 ! "
        "appsink name=sink max-buffers=1 drop=true sync=false",
        src_snippet);
    g_free (src_snippet);

    RecGoPreviewFeed *feed = create_from_launch (desc, device->id, error);
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

    gchar *desc = g_strdup_printf (
        "pipewiresrc fd=%d path=%u do-timestamp=true ! "
        "videoconvert ! videoscale ! video/x-raw,format=RGBA,width=960,height=540 ! "
        "appsink name=sink max-buffers=1 drop=true sync=false",
        dup_fd, node_id);

    RecGoPreviewFeed *feed = create_from_launch (desc, "screen", error);
    g_free (desc);

    if (!feed) {
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
recgo_preview_feed_stop (RecGoPreviewFeed *self)
{
    g_return_if_fail (RECGO_IS_PREVIEW_FEED (self));
    self->stopped = TRUE;

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
    g_free (self->staging_data);
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
