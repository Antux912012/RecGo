#include "recgo-codecs.h"
#include <gst/gst.h>

void
recgo_codec_info_free (RecGoCodecInfo *info)
{
    if (!info)
        return;
    g_free (info->id);
    g_free (info->display_name);
    g_free (info->element_name);
    g_free (info->backend_label);
    g_free (info);
}

gboolean
recgo_has_gst_element (const gchar *name)
{
    if (!name || !*name)
        return FALSE;
    GstElementFactory *factory = gst_element_factory_find (name);
    if (factory) {
        gst_object_unref (factory);
        return TRUE;
    }
    return FALSE;
}

RecGoGpuMode
recgo_detect_gpu_acceleration (gchar **summary_out)
{
    gboolean has_vaapi = recgo_has_gst_element ("vapostproc")
                      || recgo_has_gst_element ("vah264enc")
                      || recgo_has_gst_element ("vah265enc")
                      || recgo_has_gst_element ("vavp8enc")
                      || recgo_has_gst_element ("vaav1enc");
    gboolean has_vulkan = recgo_has_gst_element ("vulkanupload")
                       && recgo_has_gst_element ("vulkancolorconvert");
    gboolean has_opengl = recgo_has_gst_element ("glupload")
                       && recgo_has_gst_element ("glcolorconvert");

    if (summary_out) {
        GPtrArray *badges = g_ptr_array_new ();
        if (has_vaapi)
            g_ptr_array_add (badges, "VA-API (Intel/AMD)");
        if (has_vulkan)
            g_ptr_array_add (badges, "Vulkan");
        if (has_opengl)
            g_ptr_array_add (badges, "OpenGL");

        if (badges->len == 0) {
            *summary_out = g_strdup ("Software CPU");
        } else {
            g_ptr_array_add (badges, NULL);
            *summary_out = g_strjoinv (" + ", (gchar **) badges->pdata);
        }
        g_ptr_array_free (badges, TRUE);
    }

    if (has_vaapi)
        return RECGO_GPU_VAAPI;
    if (has_vulkan)
        return RECGO_GPU_VULKAN;
    if (has_opengl)
        return RECGO_GPU_OPENGL;
    return RECGO_GPU_SOFTWARE;
}

typedef struct {
    const gchar *element;
    gboolean is_hw;
    const gchar *backend;
} EncoderCandidate;

static void
pick_first_available (const EncoderCandidate *candidates,
                      gsize n_candidates,
                      const gchar *fallback_element,
                      const gchar *fallback_backend,
                      gchar **element_out,
                      gboolean *is_hw_out,
                      gchar **backend_out)
{
    for (gsize i = 0; i < n_candidates; i++) {
        if (recgo_has_gst_element (candidates[i].element)) {
            *element_out = g_strdup (candidates[i].element);
            *is_hw_out = candidates[i].is_hw;
            *backend_out = g_strdup (candidates[i].backend);
            return;
        }
    }
    *element_out = g_strdup (fallback_element);
    *is_hw_out = FALSE;
    *backend_out = g_strdup (fallback_backend);
}

void
recgo_detect_encoder (RecGoVideoCodec codec,
                      gchar **element_out,
                      gboolean *is_hw_out,
                      gchar **backend_out)
{
    static const EncoderCandidate h264_candidates[] = {
        { "vah264enc",     TRUE,  "VA-API GPU (Intel/AMD)" },
        { "vah264lpenc",   TRUE,  "VA-API Low-Power GPU (Intel/AMD)" },
        { "vulkanh264enc", TRUE,  "Vulkan Video GPU" },
        { "vaapih264enc",  TRUE,  "VA-API Legacy GPU" },
        { "nvh264enc",     TRUE,  "NVIDIA NVENC GPU" },
        { "x264enc",       FALSE, "Software CPU (x264)" },
    };
    static const EncoderCandidate h265_candidates[] = {
        { "vah265enc",     TRUE,  "VA-API GPU (Intel/AMD)" },
        { "vah265lpenc",   TRUE,  "VA-API Low-Power GPU (Intel/AMD)" },
        { "vulkanh265enc", TRUE,  "Vulkan Video GPU" },
        { "vaapih265enc",  TRUE,  "VA-API Legacy GPU" },
        { "nvh265enc",     TRUE,  "NVIDIA NVENC GPU" },
    };
    static const EncoderCandidate vp8_candidates[] = {
        { "vavp8enc",    TRUE, "VA-API GPU (Intel/AMD)" },
        { "vaapivp8enc", TRUE, "VA-API Legacy GPU" },
    };
    static const EncoderCandidate vp9_candidates[] = {
        { "vavp9enc",    TRUE, "VA-API GPU (Intel/AMD)" },
        { "vaapivp9enc", TRUE, "VA-API Legacy GPU" },
    };
    static const EncoderCandidate av1_candidates[] = {
        { "vaav1enc",     TRUE, "VA-API GPU (Intel/AMD)" },
        { "vulkanav1enc", TRUE, "Vulkan Video GPU" },
        { "nvav1enc",     TRUE, "NVIDIA NVENC GPU" },
    };

    switch (codec) {
    case RECGO_CODEC_H264:
        pick_first_available (h264_candidates, G_N_ELEMENTS (h264_candidates),
                              "openh264enc", "Software CPU (OpenH264)",
                              element_out, is_hw_out, backend_out);
        break;
    case RECGO_CODEC_H265:
        pick_first_available (h265_candidates, G_N_ELEMENTS (h265_candidates),
                              "x265enc", "Software CPU (x265)",
                              element_out, is_hw_out, backend_out);
        break;
    case RECGO_CODEC_VP8:
        pick_first_available (vp8_candidates, G_N_ELEMENTS (vp8_candidates),
                              "vp8enc", "Software CPU (libvpx)",
                              element_out, is_hw_out, backend_out);
        break;
    case RECGO_CODEC_VP9:
        pick_first_available (vp9_candidates, G_N_ELEMENTS (vp9_candidates),
                              "vp9enc", "Software CPU (libvpx)",
                              element_out, is_hw_out, backend_out);
        break;
    case RECGO_CODEC_AV1:
        pick_first_available (av1_candidates, G_N_ELEMENTS (av1_candidates),
                              "svtav1enc", "Software CPU (SVT-AV1)",
                              element_out, is_hw_out, backend_out);
        break;
    }
}

void
recgo_detect_software_encoder (RecGoVideoCodec codec,
                               gchar **element_out,
                               gchar **backend_out)
{
    static const struct {
        const gchar *element;
        const gchar *backend;
    } sw_fallbacks[] = {
        { "x264enc",     "Software CPU Fallback (x264)" },
        { "openh264enc", "Software CPU Fallback (OpenH264)" },
        { "vp8enc",      "Software CPU Fallback (libvpx)" },
        { "vp9enc",      "Software CPU Fallback (libvpx)" },
    };

    if (codec == RECGO_CODEC_VP8 && recgo_has_gst_element ("vp8enc")) {
        *element_out = g_strdup ("vp8enc");
        *backend_out = g_strdup ("Software CPU Fallback (libvpx)");
        return;
    }
    if (codec == RECGO_CODEC_VP9 && recgo_has_gst_element ("vp9enc")) {
        *element_out = g_strdup ("vp9enc");
        *backend_out = g_strdup ("Software CPU Fallback (libvpx)");
        return;
    }
    if (codec == RECGO_CODEC_H265 && recgo_has_gst_element ("x265enc")) {
        *element_out = g_strdup ("x265enc");
        *backend_out = g_strdup ("Software CPU Fallback (x265)");
        return;
    }
    if (codec == RECGO_CODEC_AV1 && recgo_has_gst_element ("svtav1enc")) {
        *element_out = g_strdup ("svtav1enc");
        *backend_out = g_strdup ("Software CPU Fallback (SVT-AV1)");
        return;
    }

    for (gsize i = 0; i < G_N_ELEMENTS (sw_fallbacks); i++) {
        if (recgo_has_gst_element (sw_fallbacks[i].element)) {
            *element_out = g_strdup (sw_fallbacks[i].element);
            *backend_out = g_strdup (sw_fallbacks[i].backend);
            return;
        }
    }

    *element_out = g_strdup ("x264enc");
    *backend_out = g_strdup ("Software CPU Fallback");
}

GPtrArray *
recgo_get_supported_codecs (void)
{
    static const struct {
        RecGoVideoCodec codec;
        RecGoContainerFormat container;
        const gchar *id;
        const gchar *base_name;
    } specs[] = {
        { RECGO_CODEC_H264, RECGO_CONTAINER_MP4,  "h264", "H.264 / MP4" },
        { RECGO_CODEC_H265, RECGO_CONTAINER_MP4,  "h265", "H.265 (HEVC) / MP4" },
        { RECGO_CODEC_VP8,  RECGO_CONTAINER_WEBM, "vp8",  "VP8 / WebM" },
        { RECGO_CODEC_VP9,  RECGO_CONTAINER_WEBM, "vp9",  "VP9 / WebM (GNOME)" },
        { RECGO_CODEC_AV1,  RECGO_CONTAINER_MKV,  "av1",  "AV1 / MKV" },
    };

    GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) recgo_codec_info_free);

    for (gsize i = 0; i < G_N_ELEMENTS (specs); i++) {
        gchar *element = NULL;
        gboolean is_hw = FALSE;
        gchar *backend = NULL;
        recgo_detect_encoder (specs[i].codec, &element, &is_hw, &backend);

        if (recgo_has_gst_element (element)) {
            RecGoCodecInfo *info = g_new0 (RecGoCodecInfo, 1);
            info->codec = specs[i].codec;
            info->container = specs[i].container;
            info->id = g_strdup (specs[i].id);
            info->element_name = element;
            info->is_hardware = is_hw;
            info->backend_label = backend;
            info->display_name = g_strdup_printf ("%s [%s - %s]",
                                                  specs[i].base_name,
                                                  is_hw ? "GPU HW" : "CPU",
                                                  info->element_name);
            g_ptr_array_add (list, info);
        } else {
            g_free (element);
            g_free (backend);
        }
    }

    return list;
}

const gchar *
recgo_get_muxer_for_container (RecGoContainerFormat container)
{
    switch (container) {
    case RECGO_CONTAINER_WEBM: return "webmmux";
    case RECGO_CONTAINER_MKV:  return "matroskamux";
    case RECGO_CONTAINER_MP4:
    default:                   return "mp4mux";
    }
}

const gchar *
recgo_get_extension_for_container (RecGoContainerFormat container)
{
    switch (container) {
    case RECGO_CONTAINER_WEBM: return "webm";
    case RECGO_CONTAINER_MKV:  return "mkv";
    case RECGO_CONTAINER_MP4:
    default:                   return "mp4";
    }
}
