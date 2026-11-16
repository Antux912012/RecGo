#pragma once

#include "recgo-devices.h"
#include <gdk/gdk.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

G_BEGIN_DECLS

#define RECGO_TYPE_PREVIEW_FEED (recgo_preview_feed_get_type ())
G_DECLARE_FINAL_TYPE (RecGoPreviewFeed, recgo_preview_feed, RECGO, PREVIEW_FEED, GObject)

RecGoPreviewFeed *recgo_preview_feed_new_camera (const RecGoDeviceInfo *device, GError **error);
RecGoPreviewFeed *recgo_preview_feed_new_screen (int pw_fd, guint32 node_id, GError **error);
RecGoPreviewFeed *recgo_preview_feed_new_from_appsink (GstAppSink *appsink);

void recgo_preview_feed_stop (RecGoPreviewFeed *self);
const gchar *recgo_preview_feed_get_device_id (RecGoPreviewFeed *self);

G_END_DECLS
