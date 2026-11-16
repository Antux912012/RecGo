#pragma once

#include "recgo-codecs.h"
#include "recgo-devices.h"
#include "recgo-preview.h"
#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
    RECGO_LAYOUT_DESKTOP_PIP = 0,
    RECGO_LAYOUT_SIDE_BY_SIDE = 1,
    RECGO_LAYOUT_SCREEN_ONLY = 2,
    RECGO_LAYOUT_CAMERA_ONLY = 3
} RecGoLayoutPreset;

typedef enum {
    RECGO_SHAPE_RECTANGLE = 0,
    RECGO_SHAPE_SQUARE    = 1,
    RECGO_SHAPE_CIRCLE    = 2
} RecGoWebcamShape;

typedef struct {
    int screen_fd;          /* -1 if no screen stream */
    guint32 screen_node_id;
    guint screen_width;
    guint screen_height;

    GPtrArray *cameras;     /* element type: RecGoDeviceInfo* (nullable/empty) */
    GPtrArray *microphones; /* element type: RecGoDeviceInfo* (nullable/empty) */
    gboolean mic_enabled;

    RecGoLayoutPreset layout;
    RecGoWebcamShape shape;
    gdouble overlay_x;
    gdouble overlay_y;
    gdouble overlay_scale;

    RecGoVideoCodec codec;
    RecGoContainerFormat container;
    gboolean prefer_gpu_compositing;
    const gchar *output_path;
} RecGoRecordConfig;

#define RECGO_TYPE_RECORDER (recgo_recorder_get_type ())
G_DECLARE_FINAL_TYPE (RecGoRecorder, recgo_recorder, RECGO, RECORDER, GObject)

RecGoRecorder *recgo_recorder_new (const RecGoRecordConfig *config, GError **error);

gboolean recgo_recorder_start (RecGoRecorder *self, GError **error);
gboolean recgo_recorder_pause (RecGoRecorder *self, GError **error);
gboolean recgo_recorder_resume (RecGoRecorder *self, GError **error);
void recgo_recorder_stop_async (RecGoRecorder *self);

const gchar *recgo_recorder_get_encoder_description (RecGoRecorder *self);
RecGoPreviewFeed *recgo_recorder_get_preview_feed (RecGoRecorder *self);

G_END_DECLS
