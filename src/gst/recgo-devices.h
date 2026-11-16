#pragma once

#include <glib-object.h>
#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum {
    RECGO_DEVICE_CAMERA = 0,
    RECGO_DEVICE_MICROPHONE = 1
} RecGoDeviceKind;

typedef struct {
    gchar *id;           /* Stable identifier: e.g. /dev/video0, pipewire node id, or pulse source name */
    gchar *display_name; /* Human-readable name */
    gchar *gst_element;  /* e.g. "v4l2src", "pipewiresrc", "pulsesrc" */
    gchar *gst_prop;     /* e.g. "device", "target-object", "device" */
    gchar *gst_value;    /* Property value to pass to the element */
    RecGoDeviceKind kind;
    gboolean is_default;
} RecGoDeviceInfo;

void recgo_device_info_free (RecGoDeviceInfo *info);
RecGoDeviceInfo *recgo_device_info_copy (const RecGoDeviceInfo *info);

#define RECGO_TYPE_DEVICE_MANAGER (recgo_device_manager_get_type ())
G_DECLARE_FINAL_TYPE (RecGoDeviceManager, recgo_device_manager, RECGO, DEVICE_MANAGER, GObject)

RecGoDeviceManager *recgo_device_manager_new (void);

GPtrArray *recgo_device_manager_get_cameras (RecGoDeviceManager *self);
GPtrArray *recgo_device_manager_get_microphones (RecGoDeviceManager *self);

/* Builds a safe GStreamer source launch snippet for a camera or mic device */
gchar *recgo_device_info_build_source_pipeline (const RecGoDeviceInfo *info);

G_END_DECLS
