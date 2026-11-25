#include "recgo-editor.h"
#include <gio/gio.h>
#include <gst/gst.h>
#include <string.h>

struct _RecGoEditor {
    GObject parent_instance;
    GstElement *pipeline;
    guint bus_watch_id;
    guint progress_id;
    guint64 total_duration_ns;
    gboolean exporting;
};

G_DEFINE_FINAL_TYPE (RecGoEditor, recgo_editor, G_TYPE_OBJECT)

enum {
    SIGNAL_PROGRESS,
    SIGNAL_FINISHED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
cleanup_pipeline (RecGoEditor *self)
{
    if (self->progress_id != 0) {
        g_source_remove (self->progress_id);
        self->progress_id = 0;
    }
    if (self->bus_watch_id != 0) {
        g_source_remove (self->bus_watch_id);
        self->bus_watch_id = 0;
    }
    if (self->pipeline) {
        gst_element_set_state (self->pipeline, GST_STATE_NULL);
        g_clear_object (&self->pipeline);
    }
    self->exporting = FALSE;
}

static gboolean
on_progress_tick (gpointer user_data)
{
    RecGoEditor *self = RECGO_EDITOR (user_data);
    if (!self->pipeline || !self->exporting)
        return G_SOURCE_REMOVE;

    gint64 pos = 0;
    if (self->total_duration_ns > 0 &&
        gst_element_query_position (self->pipeline, GST_FORMAT_TIME, &pos) &&
        pos >= 0) {
        gdouble frac = CLAMP ((gdouble) pos / (gdouble) self->total_duration_ns, 0.0, 1.0);
        g_signal_emit (self, signals[SIGNAL_PROGRESS], 0, frac);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
on_bus_message (GstBus *bus, GstMessage *msg, gpointer user_data)
{
    (void) bus;
    RecGoEditor *self = RECGO_EDITOR (user_data);

    switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
        cleanup_pipeline (self);
        g_signal_emit (self, signals[SIGNAL_PROGRESS], 0, 1.0);
        g_signal_emit (self, signals[SIGNAL_FINISHED], 0, TRUE, NULL);
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error (msg, &err, &dbg);
        gchar *msg_copy = g_strdup (err ? err->message : "Export pipeline error");
        g_clear_error (&err);
        g_free (dbg);
        cleanup_pipeline (self);
        g_signal_emit (self, signals[SIGNAL_FINISHED], 0, FALSE, msg_copy);
        g_free (msg_copy);
        break;
    }
    default:
        break;
    }
    return TRUE;
}

static gboolean
validate_path (const gchar *path)
{
    if (!path || !*path)
        return FALSE;
    for (const gchar *p = path; *p; p++) {
        if (g_ascii_iscntrl (*p) || *p == '"')
            return FALSE;
    }
    return TRUE;
}

gboolean
recgo_editor_export_async (RecGoEditor *self,
                           const gchar *input_path,
                           const gchar *output_path,
                           const RecGoEditSegment *segments,
                           guint n_segments,
                           RecGoVideoCodec codec,
                           RecGoContainerFormat container,
                           GError **error)
{
    g_return_val_if_fail (RECGO_IS_EDITOR (self), FALSE);

    if (self->exporting) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY, "An export is already in progress");
        return FALSE;
    }
    if (!validate_path (input_path) || !validate_path (output_path)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid file path");
        return FALSE;
    }
    if (g_strcmp0 (input_path, output_path) == 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Output path must differ from source file");
        return FALSE;
    }

    gchar *enc_name = NULL;
    gboolean is_hw = FALSE;
    gchar *backend = NULL;
    recgo_detect_encoder (codec, &enc_name, &is_hw, &backend);
    g_free (backend);

    const gchar *muxer = recgo_get_muxer_for_container (container);
    const gchar *audio_enc = (container == RECGO_CONTAINER_MP4 && recgo_has_gst_element ("avenc_aac"))
        ? "avenc_aac"
        : "opusenc";
    const gchar *parse_part = "";
    if (codec == RECGO_CODEC_H264) parse_part = "! h264parse ";
    else if (codec == RECGO_CODEC_H265) parse_part = "! h265parse ";
    else if (codec == RECGO_CODEC_AV1) parse_part = "! av1parse ";

    gchar *uri = g_filename_to_uri (input_path, NULL, error);
    if (!uri) {
        g_free (enc_name);
        return FALSE;
    }

    /* Build a clean transcode/trim pipeline; seek to [start, end] before PLAYING */
    gchar *desc = g_strdup_printf (
        "uridecodebin uri=\"%s\" name=dec "
        "%s name=mux ! filesink location=\"%s\" "
        "dec. ! queue ! videoconvert ! %s %s! queue ! mux. "
        "dec. ! queue ! audioconvert ! audioresample ! %s ! queue ! mux.",
        uri, muxer, output_path, enc_name, parse_part, audio_enc);
    g_free (uri);
    g_free (enc_name);

    GError *parse_err = NULL;
    GstElement *el = gst_parse_launch (desc, &parse_err);
    g_free (desc);
    if (!el) {
        g_propagate_error (error, parse_err);
        return FALSE;
    }

    self->pipeline = el;
    self->exporting = TRUE;

    guint64 start_ms = (segments && n_segments > 0) ? segments[0].start_ms : 0;
    guint64 end_ms = (segments && n_segments > 0) ? segments[n_segments - 1].end_ms : 0;
    self->total_duration_ns = (end_ms > start_ms) ? (end_ms - start_ms) * GST_MSECOND : 0;

    GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (el));
    self->bus_watch_id = gst_bus_add_watch (bus, on_bus_message, self);
    gst_object_unref (bus);

    if (gst_element_set_state (el, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        cleanup_pipeline (self);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to preroll export pipeline");
        return FALSE;
    }

    gst_element_get_state (el, NULL, NULL, 3 * GST_SECOND);

    if (end_ms > start_ms) {
        gst_element_seek (el,
                          1.0,
                          GST_FORMAT_TIME,
                          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                          GST_SEEK_TYPE_SET, (gint64) (start_ms * GST_MSECOND),
                          GST_SEEK_TYPE_SET, (gint64) (end_ms * GST_MSECOND));
    }

    if (gst_element_set_state (el, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        cleanup_pipeline (self);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start export pipeline");
        return FALSE;
    }

    self->progress_id = g_timeout_add (150, on_progress_tick, self);
    return TRUE;
}

void
recgo_editor_cancel (RecGoEditor *self)
{
    g_return_if_fail (RECGO_IS_EDITOR (self));
    if (!self->exporting)
        return;
    cleanup_pipeline (self);
    g_signal_emit (self, signals[SIGNAL_FINISHED], 0, FALSE, "Export cancelled");
}

static void
recgo_editor_dispose (GObject *object)
{
    cleanup_pipeline (RECGO_EDITOR (object));
    G_OBJECT_CLASS (recgo_editor_parent_class)->dispose (object);
}

static void
recgo_editor_class_init (RecGoEditorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = recgo_editor_dispose;

    signals[SIGNAL_PROGRESS] = g_signal_new (
        "progress",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_DOUBLE);

    signals[SIGNAL_FINISHED] = g_signal_new (
        "finished",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_BOOLEAN, G_TYPE_STRING);
}

static void
recgo_editor_init (RecGoEditor *self)
{
    (void) self;
}

RecGoEditor *
recgo_editor_new (void)
{
    return g_object_new (RECGO_TYPE_EDITOR, NULL);
}
