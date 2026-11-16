#include "recgo-codecs.h"
#include "recgo-devices.h"
#include <gst/gst.h>
#include <assert.h>
#include <string.h>

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

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gst_init (&argc, &argv);

    g_test_add_func ("/recgo/codecs_and_containers", test_codecs_and_containers);
    g_test_add_func ("/recgo/device_pipeline_validation", test_device_pipeline_validation);
    g_test_add_func ("/recgo/device_manager_lifecycle", test_device_manager_lifecycle);

    return g_test_run ();
}
