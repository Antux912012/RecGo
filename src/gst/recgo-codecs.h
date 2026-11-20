#pragma once

#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

typedef enum {
    RECGO_CODEC_H264 = 0,
    RECGO_CODEC_H265 = 1,
    RECGO_CODEC_VP8  = 2,
    RECGO_CODEC_VP9  = 3,
    RECGO_CODEC_AV1  = 4
} RecGoVideoCodec;

typedef enum {
    RECGO_CONTAINER_MP4  = 0,
    RECGO_CONTAINER_WEBM = 1,
    RECGO_CONTAINER_MKV  = 2
} RecGoContainerFormat;

typedef enum {
    RECGO_GPU_VAAPI    = 0,
    RECGO_GPU_VULKAN   = 1,
    RECGO_GPU_OPENGL   = 2,
    RECGO_GPU_SOFTWARE = 3
} RecGoGpuMode;

typedef struct {
    RecGoVideoCodec codec;
    RecGoContainerFormat container;
    gchar *id;
    gchar *display_name;
    gchar *element_name;
    gboolean is_hardware;
    gchar *backend_label;
} RecGoCodecInfo;

void recgo_codec_info_free (RecGoCodecInfo *info);

gboolean recgo_has_gst_element (const gchar *name);

RecGoGpuMode recgo_detect_gpu_acceleration (gchar **summary_out);

GPtrArray *recgo_get_supported_codecs (void);

const gchar *recgo_get_muxer_for_container (RecGoContainerFormat container);
const gchar *recgo_get_extension_for_container (RecGoContainerFormat container);

void recgo_detect_encoder (RecGoVideoCodec codec,
                           gchar **element_out,
                           gboolean *is_hw_out,
                           gchar **backend_out);

void recgo_detect_software_encoder (RecGoVideoCodec codec,
                                    gchar **element_out,
                                    gchar **backend_out);

G_END_DECLS
