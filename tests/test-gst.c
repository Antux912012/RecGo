#include "recgo-codecs.h"
#include "recgo-devices.h"
#include "recgo-video-fx.h"
#include <gst/gst.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

static void
test_codecs_and_containers (void)
{
    gchar *summary = NULL;
    RecGoGpuMode mode = recgo_detect_gpu_acceleration (&summary);
    g_assert_nonnull (summary);
    g_assert_true (mode >= RECGO_GPU_VAAPI && mode <= RECGO_GPU_SOFTWARE);
    g_free (summary);

    g_assert_cmpstr (recgo_get_muxer_for_container (RECGO_CONTAINER_MP4), ==, "mp4mux");
    g_assert_cmpstr (recgo_get_muxer_for_container (RECGO_CONTAINER_WEBM), ==, "webmmux");
    g_assert_cmpstr (recgo_get_muxer_for_container (RECGO_CONTAINER_MKV), ==, "matroskamux");

    g_assert_cmpstr (recgo_get_extension_for_container (RECGO_CONTAINER_MP4), ==, "mp4");
    g_assert_cmpstr (recgo_get_extension_for_container (RECGO_CONTAINER_WEBM), ==, "webm");
    g_assert_cmpstr (recgo_get_extension_for_container (RECGO_CONTAINER_MKV), ==, "mkv");

    GPtrArray *codecs = recgo_get_supported_codecs ();
    g_assert_nonnull (codecs);
    g_ptr_array_unref (codecs);

    gchar *sw_enc = NULL, *sw_label = NULL;
    recgo_detect_software_encoder (RECGO_CODEC_H264, &sw_enc, &sw_label);
    g_assert_nonnull (sw_enc);
    g_assert_nonnull (sw_label);
    g_free (sw_enc);
    g_free (sw_label);
}

static void
test_device_pipeline_validation (void)
{
    RecGoDeviceInfo valid_v4l2 = {
        .id = "/dev/video0",
        .display_name = "Test Cam",
        .gst_element = "v4l2src",
        .gst_prop = "device",
        .gst_value = "/dev/video0",
        .kind = RECGO_DEVICE_CAMERA,
        .is_default = TRUE
    };
    gchar *pipe = recgo_device_info_build_source_pipeline (&valid_v4l2);
    g_assert_nonnull (pipe);
    g_assert_cmpstr (pipe, ==, "v4l2src device=\"/dev/video0\"");
    g_free (pipe);

    RecGoDeviceInfo injected = valid_v4l2;
    injected.gst_value = "/dev/video0 ! fakesink";
    g_assert_null (recgo_device_info_build_source_pipeline (&injected));

    injected.gst_value = "/dev/video0\"";
    g_assert_null (recgo_device_info_build_source_pipeline (&injected));

    RecGoDeviceInfo valid_pw = {
        .id = "pw:42",
        .display_name = "PW Cam",
        .gst_element = "pipewiresrc",
        .gst_prop = "target-object",
        .gst_value = "42",
        .kind = RECGO_DEVICE_CAMERA,
        .is_default = FALSE
    };
    gchar *pw_pipe = recgo_device_info_build_source_pipeline (&valid_pw);
    g_assert_nonnull (pw_pipe);
    g_assert_cmpstr (pw_pipe, ==, "pipewiresrc target-object=\"42\" do-timestamp=true");
    g_free (pw_pipe);
}

static void
test_device_manager_lifecycle (void)
{
    RecGoDeviceManager *mgr = recgo_device_manager_new ();
    g_assert_nonnull (mgr);
    GPtrArray *cams = recgo_device_manager_get_cameras (mgr);
    GPtrArray *mics = recgo_device_manager_get_microphones (mgr);
    g_assert_nonnull (cams);
    g_assert_nonnull (mics);
    g_ptr_array_unref (cams);
    g_ptr_array_unref (mics);
    g_object_unref (mgr);
}

static void
fill_rgba (guint8 *buf, gint w, gint h, guint8 r, guint8 g, guint8 b, guint8 a)
{
    for (gint i = 0; i < w * h; i++) {
        buf[i * 4 + 0] = r;
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = b;
        buf[i * 4 + 3] = a;
    }
}

static void
test_video_fx_circle_square_rect_masks (void)
{
    const gint w = 160;
    const gint h = 160;
    const gint stride = w * 4;
    const gint pad = recgo_video_fx_get_shadow_pad (TRUE);
    g_assert_cmpint (pad, >, 0);
    g_assert_cmpint (pad % 2, ==, 0);

    guint8 *buf = g_malloc ((gsize) stride * (gsize) h);
    RecGoWebcamImageConfig cfg;
    recgo_webcam_image_config_init_defaults (&cfg);

    /* Circle mask with border + shadow */
    fill_rgba (buf, w, h, 200, 60, 40, 255);
    RecGoVideoFx *fx = recgo_video_fx_new (&cfg, TRUE, RECGO_SHAPE_CIRCLE, pad);
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);

    /* Center pixel must remain opaque red-ish video */
    const guint8 *center = buf + (h / 2) * stride + (w / 2) * 4;
    g_assert_cmpuint (center[0], ==, 200);
    g_assert_cmpuint (center[1], ==, 60);
    g_assert_cmpuint (center[2], ==, 40);
    g_assert_cmpuint (center[3], ==, 255);

    /* Top-left corner (0,0) must be completely transparent */
    g_assert_cmpuint (buf[3], ==, 0);

    /* Bottom shadow just outside the circle edge (cx, cy + radius + 5) must have shadow alpha > 0 and dark RGB */
    const guint8 *shadow_px = buf + (h - pad + 4) * stride + (w / 2) * 4;
    g_assert_cmpuint (shadow_px[3], >, 20);
    g_assert_cmpuint (shadow_px[0], <, 30);

    /* Border pixel on the top rim of the circle must be bright white */
    const guint8 *border_px = buf + (pad + 2) * stride + (w / 2) * 4;
    g_assert_cmpuint (border_px[0], >, 220);
    g_assert_cmpuint (border_px[1], >, 200);
    g_assert_cmpuint (border_px[2], >, 200);
    g_assert_cmpuint (border_px[3], >, 200);

    /* Rounded Square mask without shadow */
    cfg.shadow_enabled = FALSE;
    recgo_video_fx_update_image_config (fx, &cfg);
    recgo_video_fx_update_mask (fx, TRUE, RECGO_SHAPE_SQUARE, 0);
    fill_rgba (buf, w, h, 50, 180, 90, 255);
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    g_assert_cmpuint (buf[3], ==, 0); /* rounded corner clipped */
    g_assert_cmpuint (center[1], ==, 180);
    g_assert_cmpuint (center[3], ==, 255);

    /* Rounded Rectangle mask */
    recgo_video_fx_update_mask (fx, TRUE, RECGO_SHAPE_RECTANGLE, 0);
    fill_rgba (buf, w, h, 30, 90, 210, 255);
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    g_assert_cmpuint (buf[3], ==, 0); /* corner clipped */
    g_assert_cmpuint (center[2], ==, 210);
    g_assert_cmpuint (center[3], ==, 255);

    g_object_unref (fx);
    g_free (buf);
}

static void
test_video_fx_image_enhancements (void)
{
    const gint w = 64;
    const gint h = 64;
    const gint stride = w * 4;
    guint8 *buf = g_malloc ((gsize) stride * (gsize) h);

    /* 1. Horizontal Mirror */
    RecGoWebcamImageConfig cfg;
    recgo_webcam_image_config_init_defaults (&cfg);
    cfg.mirror = TRUE;
    RecGoVideoFx *fx = recgo_video_fx_new (&cfg, FALSE, RECGO_SHAPE_RECTANGLE, 0);

    fill_rgba (buf, w, h, 10, 20, 30, 255);
    buf[0] = 250; buf[1] = 10; buf[2] = 20; buf[3] = 255; /* top-left */
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    /* Top-right should now have the pixel from top-left */
    g_assert_cmpuint (buf[(w - 1) * 4 + 0], ==, 250);
    g_assert_cmpuint (buf[0], ==, 10);

    /* 2. Grayscale (saturation = 0.0) */
    recgo_webcam_image_config_init_defaults (&cfg);
    cfg.saturation = 0.0;
    recgo_video_fx_update_image_config (fx, &cfg);
    fill_rgba (buf, w, h, 220, 80, 40, 255);
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    g_assert_cmpuint (buf[0], ==, buf[1]);
    g_assert_cmpuint (buf[1], ==, buf[2]);

    /* 3. Brightness + Warmth + Auto-enhance on dim frame */
    recgo_webcam_image_config_init_defaults (&cfg);
    cfg.auto_enhance = TRUE;
    cfg.brightness = 0.10;
    cfg.warmth = 0.5;
    recgo_video_fx_update_image_config (fx, &cfg);
    fill_rgba (buf, w, h, 40, 40, 40, 255);
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    g_assert_cmpuint (buf[0], >, 55);          /* lifted */
    g_assert_cmpuint (buf[0], >, buf[2]);      /* warmer R > B */

    /* 4. Digital Zoom (2.0x) pulls center marker outward */
    recgo_webcam_image_config_init_defaults (&cfg);
    cfg.zoom = 2.0;
    recgo_video_fx_update_image_config (fx, &cfg);
    fill_rgba (buf, w, h, 10, 10, 10, 255);
    for (gint y = 16; y < 48; y++) {
        for (gint x = 16; x < 48; x++) {
            guint8 *p = buf + y * stride + x * 4;
            p[0] = 200; p[1] = 200; p[2] = 200; p[3] = 255;
        }
    }
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    /* Outer pixel near corner should now be bright from zoomed center box */
    g_assert_cmpuint (buf[2 * stride + 2 * 4], >, 150);

    /* 5. Temporal Denoise + Sharpness run cleanly without corruption */
    recgo_webcam_image_config_init_defaults (&cfg);
    cfg.denoise = 0.6;
    cfg.sharpness = 0.5;
    recgo_video_fx_update_image_config (fx, &cfg);
    fill_rgba (buf, w, h, 100, 100, 100, 255);
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    fill_rgba (buf, w, h, 106, 106, 106, 255);
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    /* Denoise blends 106 toward 100 */
    g_assert_cmpuint (buf[10 * stride + 10 * 4], <, 106);
    g_assert_cmpuint (buf[10 * stride + 10 * 4], >, 100);

    g_object_unref (fx);
    g_free (buf);
}

static void
test_video_fx_performance_budget (void)
{
    /* Benchmark 460x460 masked + enhanced PiP frame on host CPU */
    const gint w = 460;
    const gint h = 460;
    const gint stride = w * 4;
    const gint pad = recgo_video_fx_get_shadow_pad (TRUE);
    guint8 *buf = g_malloc ((gsize) stride * (gsize) h);
    fill_rgba (buf, w, h, 90, 120, 150, 255);

    RecGoWebcamImageConfig cfg;
    recgo_webcam_image_config_init_defaults (&cfg);
    cfg.mirror = TRUE;
    cfg.auto_enhance = TRUE;
    cfg.brightness = 0.05;
    cfg.contrast = 1.1;
    cfg.warmth = 0.2;
    cfg.saturation = 1.15;

    RecGoVideoFx *fx = recgo_video_fx_new (&cfg, TRUE, RECGO_SHAPE_CIRCLE, pad);
    /* Warm up LUTs */
    recgo_video_fx_process_rgba (fx, buf, w, h, stride);

    gint64 t0 = g_get_monotonic_time ();
    const gint iters = 30;
    for (gint i = 0; i < iters; i++) {
        recgo_video_fx_process_rgba (fx, buf, w, h, stride);
    }
    gint64 elapsed_us = g_get_monotonic_time () - t0;
    gdouble ms_per_frame = (gdouble) elapsed_us / (1000.0 * (gdouble) iters);
    g_test_message ("RecGoVideoFx 460x460 circle+FX: %.2f ms/frame", ms_per_frame);
    /* Must easily stay well under 15 ms/frame even on older dual-core CPUs (33ms budget @ 30fps) */
    g_assert_cmpfloat (ms_per_frame, <, 15.0);

    g_object_unref (fx);
    g_free (buf);
}

static void
test_compositor_mask_integration (void)
{
    if (!recgo_has_gst_element ("compositor")
        || !recgo_has_gst_element ("videotestsrc")
        || !recgo_has_gst_element ("appsink")) {
        g_test_skip ("GStreamer compositor/videotestsrc/appsink unavailable in container");
        return;
    }

    const gint pad = recgo_video_fx_get_shadow_pad (TRUE);
    const gint cam_size = 128 + 2 * pad;
    gchar *desc = g_strdup_printf (
        "compositor name=comp background=black "
        "sink_0::xpos=0 sink_0::ypos=0 sink_0::width=320 sink_0::height=240 "
        "sink_1::xpos=40 sink_1::ypos=40 sink_1::width=%d sink_1::height=%d ! "
        "videoconvert ! video/x-raw,format=RGBA ! appsink name=sink sync=false "
        "videotestsrc num-buffers=2 pattern=solid-color foreground-color=0xff00ff00 ! "
        "video/x-raw,format=RGBA,width=320,height=240 ! comp.sink_0 "
        "videotestsrc num-buffers=2 pattern=solid-color foreground-color=0xffff0000 ! "
        "video/x-raw,format=RGBA,width=%d,height=%d ! identity name=camfx ! comp.sink_1",
        cam_size, cam_size, cam_size, cam_size);

    GError *err = NULL;
    GstElement *pipe = gst_parse_launch (desc, &err);
    g_free (desc);
    g_assert_no_error (err);
    g_assert_nonnull (pipe);

    RecGoWebcamImageConfig cfg;
    recgo_webcam_image_config_init_defaults (&cfg);
    RecGoVideoFx *fx = recgo_video_fx_new (&cfg, TRUE, RECGO_SHAPE_CIRCLE, pad);

    GstElement *camfx = gst_bin_get_by_name (GST_BIN (pipe), "camfx");
    g_assert_nonnull (camfx);
    GstPad *src_pad = gst_element_get_static_pad (camfx, "src");
    g_assert_nonnull (src_pad);
    recgo_video_fx_attach_to_pad (fx, src_pad);
    gst_object_unref (src_pad);
    gst_object_unref (camfx);

    GstElement *sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
    g_assert_nonnull (sink);

    g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);

    GstSample *sample = NULL;
    g_signal_emit_by_name (sink, "pull-sample", &sample);
    g_assert_nonnull (sample);

    GstBuffer *buf = gst_sample_get_buffer (sample);
    GstMapInfo map;
    g_assert_true (gst_buffer_map (buf, &map, GST_MAP_READ));

    /* Corner of the webcam bounding box (42, 42) must show GREEN screen background through the circle mask! */
    const guint8 *corner = map.data + (42 * 320 + 42) * 4;
    g_assert_cmpuint (corner[0], <, 15);
    g_assert_cmpuint (corner[1], >, 230);
    g_assert_cmpuint (corner[2], <, 15);

    /* Center of the webcam circle (40 + cam_size/2, 40 + cam_size/2) must show RED webcam video! */
    gint cx = 40 + cam_size / 2;
    gint cy = 40 + cam_size / 2;
    const guint8 *center = map.data + (cy * 320 + cx) * 4;
    g_assert_cmpuint (center[0], >, 230);
    g_assert_cmpuint (center[1], <, 15);
    g_assert_cmpuint (center[2], <, 15);

    gst_buffer_unmap (buf, &map);
    gst_sample_unref (sample);
    gst_element_set_state (pipe, GST_STATE_NULL);
    gst_object_unref (sink);
    gst_object_unref (pipe);
    g_object_unref (fx);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gst_init (&argc, &argv);

    g_test_add_func ("/recgo/codecs_and_containers", test_codecs_and_containers);
    g_test_add_func ("/recgo/device_pipeline_validation", test_device_pipeline_validation);
    g_test_add_func ("/recgo/device_manager_lifecycle", test_device_manager_lifecycle);
    g_test_add_func ("/recgo/video_fx_masks", test_video_fx_circle_square_rect_masks);
    g_test_add_func ("/recgo/video_fx_image_enhancements", test_video_fx_image_enhancements);
    g_test_add_func ("/recgo/video_fx_performance_budget", test_video_fx_performance_budget);
    g_test_add_func ("/recgo/compositor_mask_integration", test_compositor_mask_integration);

    return g_test_run ();
}
