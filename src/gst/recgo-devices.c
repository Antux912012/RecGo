#include "recgo-devices.h"
#include <string.h>

struct _RecGoDeviceManager {
    GObject parent_instance;
    GstDeviceMonitor *monitor;
    GMutex lock;
    GPtrArray *cameras;
    GPtrArray *microphones;
    guint refresh_idle_id;
};

G_DEFINE_FINAL_TYPE (RecGoDeviceManager, recgo_device_manager, G_TYPE_OBJECT)

enum {
    SIGNAL_DEVICES_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

void
recgo_device_info_free (RecGoDeviceInfo *info)
{
    if (!info)
        return;
    g_free (info->id);
    g_free (info->display_name);
    g_free (info->gst_element);
    g_free (info->gst_prop);
    g_free (info->gst_value);
    g_free (info);
}

RecGoDeviceInfo *
recgo_device_info_copy (const RecGoDeviceInfo *info)
{
    if (!info)
        return NULL;
    RecGoDeviceInfo *copy = g_new0 (RecGoDeviceInfo, 1);
    copy->id = g_strdup (info->id);
    copy->display_name = g_strdup (info->display_name);
    copy->gst_element = g_strdup (info->gst_element);
    copy->gst_prop = g_strdup (info->gst_prop);
    copy->gst_value = g_strdup (info->gst_value);
    copy->kind = info->kind;
    copy->is_default = info->is_default;
    return copy;
}

static gboolean
has_usable_video_caps (GstDevice *device)
{
    GstCaps *caps = gst_device_get_caps (device);
    if (!caps)
        return TRUE;
    gboolean usable = FALSE;
    guint n = gst_caps_get_size (caps);
    for (guint i = 0; i < n; i++) {
        const GstStructure *st = gst_caps_get_structure (caps, i);
        const gchar *name = gst_structure_get_name (st);
        if (g_str_has_prefix (name, "video/x-raw") || g_str_has_prefix (name, "image/jpeg")) {
            usable = TRUE;
            break;
        }
    }
    gst_caps_unref (caps);
    return usable;
}

static RecGoDeviceInfo *
device_to_info (GstDevice *device, RecGoDeviceKind kind)
{
    GstStructure *props = gst_device_get_properties (device);
    gchar *display_name = gst_device_get_display_name (device);
    const gchar *device_class = gst_device_get_device_class (device);
    (void) device_class;

    RecGoDeviceInfo *info = NULL;

    if (kind == RECGO_DEVICE_CAMERA) {
        if (!has_usable_video_caps (device))
            goto out;

        const gchar *api = props ? gst_structure_get_string (props, "device.api") : NULL;
        const gchar *dev_path = props ? gst_structure_get_string (props, "device.path") : NULL;
        if (!dev_path && props)
            dev_path = gst_structure_get_string (props, "api.v4l2.path");

        if (g_strcmp0 (api, "pipewire") == 0 || !dev_path) {
            const gchar *pw_serial = props ? gst_structure_get_string (props, "object.serial") : NULL;
            gint node_id = -1;
            if (props)
                gst_structure_get_int (props, "node.id", &node_id);

            gchar *target = NULL;
            if (pw_serial && *pw_serial)
                target = g_strdup (pw_serial);
            else if (node_id >= 0)
                target = g_strdup_printf ("%d", node_id);

            if (target) {
                info = g_new0 (RecGoDeviceInfo, 1);
                info->id = g_strdup_printf ("pw:%s", target);
                info->display_name = g_strdup (display_name ? display_name : "PipeWire Camera");
                info->gst_element = g_strdup ("pipewiresrc");
                info->gst_prop = g_strdup ("target-object");
                info->gst_value = target;
                info->kind = RECGO_DEVICE_CAMERA;
            }
        } else if (dev_path && g_str_has_prefix (dev_path, "/dev/video")) {
            info = g_new0 (RecGoDeviceInfo, 1);
            info->id = g_strdup (dev_path);
            info->display_name = g_strdup (display_name ? display_name : dev_path);
            info->gst_element = g_strdup ("v4l2src");
            info->gst_prop = g_strdup ("device");
            info->gst_value = g_strdup (dev_path);
            info->kind = RECGO_DEVICE_CAMERA;
        }
    } else {
        const gchar *node_name = props ? gst_structure_get_string (props, "node.name") : NULL;
        const gchar *pa_name = props ? gst_structure_get_string (props, "device.string") : NULL;
        const gchar *api = props ? gst_structure_get_string (props, "device.api") : NULL;

        /* Skip monitor loopback sources unless explicitly needed */
        if (node_name && g_str_has_suffix (node_name, ".monitor"))
            goto out;

        const gchar *source_id = node_name ? node_name : pa_name;
        if (source_id && *source_id) {
            info = g_new0 (RecGoDeviceInfo, 1);
            info->id = g_strdup (source_id);
            info->display_name = g_strdup (display_name ? display_name : source_id);
            if (g_strcmp0 (api, "pipewire") == 0) {
                info->gst_element = g_strdup ("pipewiresrc");
                info->gst_prop = g_strdup ("target-object");
            } else {
                info->gst_element = g_strdup ("pulsesrc");
                info->gst_prop = g_strdup ("device");
            }
            info->gst_value = g_strdup (source_id);
            info->kind = RECGO_DEVICE_MICROPHONE;
            if (props)
                gst_structure_get_boolean (props, "is-default", &info->is_default);
        }
    }

out:
    if (props)
        gst_structure_free (props);
    g_free (display_name);
    return info;
}

static gboolean
contains_device_id (GPtrArray *arr, const gchar *id)
{
    for (guint i = 0; i < arr->len; i++) {
        RecGoDeviceInfo *item = g_ptr_array_index (arr, i);
        if (g_strcmp0 (item->id, id) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
rebuild_device_lists (RecGoDeviceManager *self)
{
    GList *devices = gst_device_monitor_get_devices (self->monitor);
    GPtrArray *new_cams = g_ptr_array_new_with_free_func ((GDestroyNotify) recgo_device_info_free);
    GPtrArray *new_mics = g_ptr_array_new_with_free_func ((GDestroyNotify) recgo_device_info_free);

    for (GList *l = devices; l != NULL; l = l->next) {
        GstDevice *dev = GST_DEVICE (l->data);
        const gchar *klass = gst_device_get_device_class (dev);
        if (klass && strstr (klass, "Video/Source")) {
            RecGoDeviceInfo *info = device_to_info (dev, RECGO_DEVICE_CAMERA);
            if (info) {
                if (!contains_device_id (new_cams, info->id))
                    g_ptr_array_add (new_cams, info);
                else
                    recgo_device_info_free (info);
            }
        } else if (klass && strstr (klass, "Audio/Source")) {
            RecGoDeviceInfo *info = device_to_info (dev, RECGO_DEVICE_MICROPHONE);
            if (info) {
                if (!contains_device_id (new_mics, info->id))
                    g_ptr_array_add (new_mics, info);
                else
                    recgo_device_info_free (info);
            }
        }
    }
    g_list_free_full (devices, gst_object_unref);

    /* Fallback probe for /dev/video* when running inside minimal containers */
    if (new_cams->len == 0) {
        static const gchar *fallback_nodes[] = { "/dev/video0", "/dev/video1", "/dev/video2", "/dev/video4" };
        for (gsize i = 0; i < G_N_ELEMENTS (fallback_nodes); i++) {
            if (g_file_test (fallback_nodes[i], G_FILE_TEST_EXISTS)) {
                RecGoDeviceInfo *info = g_new0 (RecGoDeviceInfo, 1);
                info->id = g_strdup (fallback_nodes[i]);
                info->display_name = g_strdup_printf ("Camera (%s)", fallback_nodes[i]);
                info->gst_element = g_strdup ("v4l2src");
                info->gst_prop = g_strdup ("device");
                info->gst_value = g_strdup (fallback_nodes[i]);
                info->kind = RECGO_DEVICE_CAMERA;
                g_ptr_array_add (new_cams, info);
            }
        }
    }

    g_mutex_lock (&self->lock);
    g_clear_pointer (&self->cameras, g_ptr_array_unref);
    g_clear_pointer (&self->microphones, g_ptr_array_unref);
    self->cameras = new_cams;
    self->microphones = new_mics;
    g_mutex_unlock (&self->lock);
}

static gboolean
emit_devices_changed_idle (gpointer user_data)
{
    RecGoDeviceManager *self = RECGO_DEVICE_MANAGER (user_data);
    self->refresh_idle_id = 0;
    rebuild_device_lists (self);
    g_signal_emit (self, signals[SIGNAL_DEVICES_CHANGED], 0);
    return G_SOURCE_REMOVE;
}

static gboolean
on_monitor_bus_message (GstBus *bus, GstMessage *msg, gpointer user_data)
{
    (void) bus;
    RecGoDeviceManager *self = RECGO_DEVICE_MANAGER (user_data);

    switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_DEVICE_ADDED:
    case GST_MESSAGE_DEVICE_REMOVED:
    case GST_MESSAGE_DEVICE_CHANGED:
        if (self->refresh_idle_id == 0)
            self->refresh_idle_id = g_idle_add (emit_devices_changed_idle, self);
        break;
    default:
        break;
    }
    return TRUE;
}

static void
recgo_device_manager_dispose (GObject *object)
{
    RecGoDeviceManager *self = RECGO_DEVICE_MANAGER (object);
    if (self->refresh_idle_id != 0) {
        g_source_remove (self->refresh_idle_id);
        self->refresh_idle_id = 0;
    }
    if (self->monitor) {
        gst_device_monitor_stop (self->monitor);
        g_clear_object (&self->monitor);
    }
    G_OBJECT_CLASS (recgo_device_manager_parent_class)->dispose (object);
}

static void
recgo_device_manager_finalize (GObject *object)
{
    RecGoDeviceManager *self = RECGO_DEVICE_MANAGER (object);
    g_clear_pointer (&self->cameras, g_ptr_array_unref);
    g_clear_pointer (&self->microphones, g_ptr_array_unref);
    g_mutex_clear (&self->lock);
    G_OBJECT_CLASS (recgo_device_manager_parent_class)->finalize (object);
}

static void
recgo_device_manager_class_init (RecGoDeviceManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = recgo_device_manager_dispose;
    object_class->finalize = recgo_device_manager_finalize;

    signals[SIGNAL_DEVICES_CHANGED] = g_signal_new (
        "devices-changed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

static void
recgo_device_manager_init (RecGoDeviceManager *self)
{
    g_mutex_init (&self->lock);
    self->cameras = g_ptr_array_new_with_free_func ((GDestroyNotify) recgo_device_info_free);
    self->microphones = g_ptr_array_new_with_free_func ((GDestroyNotify) recgo_device_info_free);

    self->monitor = gst_device_monitor_new ();
    gst_device_monitor_add_filter (self->monitor, "Video/Source", NULL);
    gst_device_monitor_add_filter (self->monitor, "Audio/Source", NULL);

    GstBus *bus = gst_device_monitor_get_bus (self->monitor);
    gst_bus_add_watch (bus, on_monitor_bus_message, self);
    gst_object_unref (bus);

    gst_device_monitor_start (self->monitor);
    rebuild_device_lists (self);
}

RecGoDeviceManager *
recgo_device_manager_new (void)
{
    return g_object_new (RECGO_TYPE_DEVICE_MANAGER, NULL);
}

static GPtrArray *
copy_device_array (RecGoDeviceManager *self, GPtrArray *src)
{
    GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) recgo_device_info_free);
    g_mutex_lock (&self->lock);
    for (guint i = 0; i < src->len; i++) {
        const RecGoDeviceInfo *item = g_ptr_array_index (src, i);
        g_ptr_array_add (out, recgo_device_info_copy (item));
    }
    g_mutex_unlock (&self->lock);
    return out;
}

GPtrArray *
recgo_device_manager_get_cameras (RecGoDeviceManager *self)
{
    g_return_val_if_fail (RECGO_IS_DEVICE_MANAGER (self), NULL);
    return copy_device_array (self, self->cameras);
}

GPtrArray *
recgo_device_manager_get_microphones (RecGoDeviceManager *self)
{
    g_return_val_if_fail (RECGO_IS_DEVICE_MANAGER (self), NULL);
    return copy_device_array (self, self->microphones);
}

gchar *
recgo_device_info_build_source_pipeline (const RecGoDeviceInfo *info)
{
    g_return_val_if_fail (info != NULL, NULL);

    /* Validate values to avoid GStreamer parse_launch injection */
    for (const gchar *p = info->gst_value; p && *p; p++) {
        if (g_ascii_iscntrl (*p) || *p == '"' || *p == '!')
            return NULL;
    }

    if (g_strcmp0 (info->gst_element, "pipewiresrc") == 0) {
        return g_strdup_printf ("pipewiresrc %s=\"%s\" do-timestamp=true",
                                info->gst_prop, info->gst_value);
    }
    return g_strdup_printf ("%s %s=\"%s\"",
                            info->gst_element, info->gst_prop, info->gst_value);
}
