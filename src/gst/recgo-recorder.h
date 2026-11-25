#pragma once

#include "recgo-codecs.h"
#include "recgo-devices.h"
#include "recgo-preview.h"
#include "recgo-video-fx.h"
#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
    RECGO_LAYOUT_DESKTOP_PIP = 0,
    RECGO_LAYOUT_SIDE_BY_SIDE = 1,
    RECGO_LAYOUT_SCREEN_ONLY = 2,
    RECGO_LAYOUT_CAMERA_ONLY = 3
} RecGoLayoutPreset;

typedef enum {
    RECGO_PERF_LOW_POWER = 0,
    RECGO_PERF_BALANCED  = 1,
    RECGO_PERF_QUALITY   = 2
} RecGoPerformanceMode;

typedef struct {
    int fd;
    guint32 node_id;
    guint width;
    guint height;
} RecGoScreenStream;

RecGoScreenStream *recgo_screen_stream_new (int fd, guint32 node_id, guint width, guint height);
RecGoScreenStream *recgo_screen_stream_copy (const RecGoScreenStream *stream);
void recgo_screen_stream_free (RecGoScreenStream *stream);

typedef struct {
    int screen_fd;          /* -1 if no screen stream (legacy single-screen field) */
    guint32 screen_node_id;
    guint screen_width;
    guint screen_height;

    GPtrArray *screen_streams; /* element type: RecGoScreenStream* (optional; overrides single screen_* if non-empty) */
    GPtrArray *cameras;        /* element type: RecGoDeviceInfo* (nullable/empty) */
    GPtrArray *microphones;    /* element type: RecGoDeviceInfo* (nullable/empty) */
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
    const gchar *output_path;
} RecGoRecordConfig;

#define RECGO_TYPE_RECORDER (recgo_recorder_get_type ())
G_DECLARE_FINAL_TYPE (RecGoRecorder, recgo_recorder, RECGO, RECORDER, GObject)

RecGoRecorder *recgo_recorder_new (const RecGoRecordConfig *config, GError **error);

gboolean recgo_recorder_start (RecGoRecorder *self, GError **error);
gboolean recgo_recorder_pause (RecGoRecorder *self, GError **error);
gboolean recgo_recorder_resume (RecGoRecorder *self, GError **error);
void recgo_recorder_stop_async (RecGoRecorder *self);

void recgo_recorder_update_image_config (RecGoRecorder *self,
                                         const RecGoWebcamImageConfig *image_cfg);
void recgo_recorder_set_mic_muted (RecGoRecorder *self, gboolean muted);

const gchar *recgo_recorder_get_encoder_description (RecGoRecorder *self);
const gchar *recgo_recorder_get_output_path (RecGoRecorder *self);
RecGoPreviewFeed *recgo_recorder_get_preview_feed (RecGoRecorder *self);

G_END_DECLS
