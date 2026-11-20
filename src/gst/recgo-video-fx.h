#pragma once

#include <glib-object.h>
#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum {
    RECGO_SHAPE_RECTANGLE = 0,
    RECGO_SHAPE_SQUARE    = 1,
    RECGO_SHAPE_CIRCLE    = 2
} RecGoWebcamShape;

typedef struct {
    gboolean mirror;
    gboolean auto_enhance;
    gdouble brightness;      /* -0.5 .. +0.5, default 0.0 */
    gdouble contrast;        /*  0.5 ..  2.0, default 1.0 */
    gdouble saturation;      /*  0.0 ..  2.0, default 1.0 */
    gdouble warmth;          /* -1.0 .. +1.0, default 0.0 */
    gdouble gamma;           /*  0.5 ..  2.0, default 1.0 */
    gdouble sharpness;       /*  0.0 ..  1.0, default 0.0 */
    gdouble denoise;         /*  0.0 ..  1.0, default 0.0 */
    gdouble zoom;            /*  1.0 ..  2.5, default 1.0 */
    gboolean border_enabled; /* default TRUE */
    gboolean shadow_enabled; /* default TRUE */
} RecGoWebcamImageConfig;

void recgo_webcam_image_config_init_defaults (RecGoWebcamImageConfig *cfg);

gint recgo_video_fx_get_shadow_pad (gboolean shadow_enabled);

#define RECGO_TYPE_VIDEO_FX (recgo_video_fx_get_type ())
G_DECLARE_FINAL_TYPE (RecGoVideoFx, recgo_video_fx, RECGO, VIDEO_FX, GObject)

RecGoVideoFx *recgo_video_fx_new (const RecGoWebcamImageConfig *image_cfg,
                                  gboolean mask_enabled,
                                  RecGoWebcamShape shape,
                                  gint mask_inset);

void recgo_video_fx_update_image_config (RecGoVideoFx *self,
                                         const RecGoWebcamImageConfig *image_cfg);

void recgo_video_fx_update_mask (RecGoVideoFx *self,
                                 gboolean mask_enabled,
                                 RecGoWebcamShape shape,
                                 gint mask_inset);

void recgo_video_fx_attach_to_pad (RecGoVideoFx *self, GstPad *pad);

/* Direct buffer processing helper (also used by unit tests) */
void recgo_video_fx_process_rgba (RecGoVideoFx *self,
                                  guint8 *rgba,
                                  gint width,
                                  gint height,
                                  gint stride);

G_END_DECLS
