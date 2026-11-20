#include "recgo-video-fx.h"
#include <gst/video/video.h>
#include <math.h>
#include <string.h>

typedef struct {
    guint8 kind;      /* 0 = transparent, 1 = opaque interior, 2 = antialiased/border/shadow */
    guint8 v_scale;   /* 0..255 video weight */
    guint8 b_add;     /* 0..255 white border additive weight */
    guint8 out_alpha; /* 0..255 straight alpha */
} RecGoMaskPixel;

struct _RecGoVideoFx {
    GObject parent_instance;

    GMutex lock;
    RecGoWebcamImageConfig cfg;
    gboolean mask_enabled;
    RecGoWebcamShape shape;
    gint mask_inset;

    /* Precomputed RGB tone LUT (brightness + contrast + gamma + warmth + auto_enhance) */
    guint8 lut_r[256];
    guint8 lut_g[256];
    guint8 lut_b[256];
    gboolean lut_dirty;
    gboolean has_tone_lut;

    /* Auto low-light / white-balance EMA state */
    gdouble auto_black;
    gdouble auto_white;
    gdouble auto_gamma;
    gdouble auto_wb_r;
    gdouble auto_wb_g;
    gdouble auto_wb_b;
    guint frame_counter;

    /* Reusable scratch buffers for zoom / sharpen / temporal denoise */
    guint8 *scratch;
    gsize scratch_cap;
    guint8 *prev_frame;
    gsize prev_cap;
    gint prev_w;
    gint prev_h;

    /* Precomputed per-pixel SDF mask LUT */
    RecGoMaskPixel *mask_lut;
    gint *row_opaque_start;
    gint *row_opaque_end;
    gint mask_w;
    gint mask_h;
    RecGoWebcamShape mask_cached_shape;
    gint mask_cached_inset;
    gboolean mask_cached_border;
    gboolean mask_cached_shadow;

    /* Cached GstVideoInfo for pad probe */
    GstVideoInfo vinfo;
    gboolean has_vinfo;
};

G_DEFINE_FINAL_TYPE (RecGoVideoFx, recgo_video_fx, G_TYPE_OBJECT)

static inline guint8
clamp_u8 (gint v)
{
    if (v <= 0) return 0;
    if (v >= 255) return 255;
    return (guint8) v;
}

static inline gdouble
smoothstep_d (gdouble edge0, gdouble edge1, gdouble x)
{
    if (edge1 <= edge0)
        return x < edge0 ? 0.0 : 1.0;
    gdouble t = CLAMP ((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void
recgo_webcam_image_config_init_defaults (RecGoWebcamImageConfig *cfg)
{
    if (!cfg)
        return;
    memset (cfg, 0, sizeof (*cfg));
    cfg->mirror = FALSE;
    cfg->auto_enhance = FALSE;
    cfg->brightness = 0.0;
    cfg->contrast = 1.0;
    cfg->saturation = 1.0;
    cfg->warmth = 0.0;
    cfg->gamma = 1.0;
    cfg->sharpness = 0.0;
    cfg->denoise = 0.0;
    cfg->zoom = 1.0;
    cfg->border_enabled = TRUE;
    cfg->shadow_enabled = TRUE;
}

gint
recgo_video_fx_get_shadow_pad (gboolean shadow_enabled)
{
    /* Even pixel padding around the visible shape so drop shadow fades to 0 without clipping */
    return shadow_enabled ? 14 : 0;
}

static void
sanitize_image_config (RecGoWebcamImageConfig *cfg)
{
    cfg->brightness = CLAMP (cfg->brightness, -0.5, 0.5);
    cfg->contrast   = CLAMP (cfg->contrast,    0.5, 2.0);
    cfg->saturation = CLAMP (cfg->saturation,  0.0, 2.0);
    cfg->warmth     = CLAMP (cfg->warmth,     -1.0, 1.0);
    cfg->gamma      = CLAMP (cfg->gamma,       0.5, 2.0);
    cfg->sharpness  = CLAMP (cfg->sharpness,   0.0, 1.0);
    cfg->denoise    = CLAMP (cfg->denoise,     0.0, 1.0);
    cfg->zoom       = CLAMP (cfg->zoom,        1.0, 2.5);
}

static void
rebuild_tone_lut_locked (RecGoVideoFx *self)
{
    const RecGoWebcamImageConfig *c = &self->cfg;

    gboolean needs_lut = c->auto_enhance
                      || fabs (c->brightness) > 0.004
                      || fabs (c->contrast - 1.0) > 0.004
                      || fabs (c->gamma - 1.0) > 0.004
                      || fabs (c->warmth) > 0.004;

    self->has_tone_lut = needs_lut;
    self->lut_dirty = FALSE;

    if (!needs_lut)
        return;

    gdouble eff_gamma = CLAMP (c->gamma * (c->auto_enhance ? self->auto_gamma : 1.0), 0.35, 3.0);
    gdouble inv_gamma = 1.0 / eff_gamma;

    gdouble black = c->auto_enhance ? self->auto_black : 0.0;
    gdouble white = c->auto_enhance ? self->auto_white : 1.0;
    gdouble inv_range = 1.0 / MAX (0.25, white - black);

    gdouble gain_r = (1.0 + c->warmth * 0.16) * (c->auto_enhance ? self->auto_wb_r : 1.0);
    gdouble gain_g = (1.0 + c->warmth * 0.03) * (c->auto_enhance ? self->auto_wb_g : 1.0);
    gdouble gain_b = (1.0 - c->warmth * 0.18) * (c->auto_enhance ? self->auto_wb_b : 1.0);

    for (gint i = 0; i < 256; i++) {
        gdouble v = (gdouble) i / 255.0;
        if (c->auto_enhance)
            v = CLAMP ((v - black) * inv_range, 0.0, 1.0);
        if (fabs (inv_gamma - 1.0) > 0.002)
            v = pow (CLAMP (v, 0.0, 1.0), inv_gamma);
        v = (v - 0.5) * c->contrast + 0.5 + c->brightness;

        gdouble r = CLAMP (v * gain_r, 0.0, 1.0);
        gdouble g = CLAMP (v * gain_g, 0.0, 1.0);
        gdouble b = CLAMP (v * gain_b, 0.0, 1.0);

        self->lut_r[i] = (guint8) lround (r * 255.0);
        self->lut_g[i] = (guint8) lround (g * 255.0);
        self->lut_b[i] = (guint8) lround (b * 255.0);
    }
}

static void
update_auto_enhance_stats_locked (RecGoVideoFx *self,
                                  const guint8 *rgba,
                                  gint width,
                                  gint height,
                                  gint stride)
{
    /* Subsample every 8th pixel (~3000 samples on 640x360) for negligible CPU cost */
    guint hist[64] = { 0 };
    guint64 sum_r = 0, sum_g = 0, sum_b = 0, sum_y = 0;
    guint count = 0;

    gint step_y = MAX (4, height / 48);
    gint step_x = MAX (4, width / 64);

    for (gint y = step_y / 2; y < height; y += step_y) {
        const guint8 *row = rgba + (gsize) y * (gsize) stride;
        for (gint x = step_x / 2; x < width; x += step_x) {
            const guint8 *px = row + x * 4;
            guint r = px[0], g = px[1], b = px[2];
            guint yv = (77 * r + 150 * g + 29 * b) >> 8;
            hist[yv >> 2]++;
            sum_r += r;
            sum_g += g;
            sum_b += b;
            sum_y += yv;
            count++;
        }
    }

    if (count < 16)
        return;

    /* 2nd and 98th percentile luma */
    guint lo_thresh = MAX (1U, count / 50);
    guint hi_thresh = count - lo_thresh;
    guint acc = 0;
    gint p_low = 0, p_high = 255;
    for (gint b = 0; b < 64; b++) {
        acc += hist[b];
        if (p_low == 0 && acc >= lo_thresh)
            p_low = b * 4;
        if (acc >= hi_thresh) {
            p_high = b * 4 + 3;
            break;
        }
    }

    gdouble avg_y = (gdouble) sum_y / (gdouble) count;
    gdouble target_black = CLAMP ((gdouble) p_low / 255.0 * 0.55, 0.0, 0.14);
    gdouble target_white = CLAMP ((gdouble) p_high / 255.0, 0.72, 1.0);
    if (target_white - target_black < 0.45)
        target_white = MIN (1.0, target_black + 0.45);

    /* Gently lift shadows when scene is dim without overexposing well-lit scenes */
    gdouble target_gamma = 1.0;
    if (avg_y < 122.0) {
        target_gamma = CLAMP (1.0 + (122.0 - avg_y) / 160.0, 1.0, 1.48);
    } else if (avg_y > 195.0) {
        target_gamma = CLAMP (1.0 - (avg_y - 195.0) / 220.0, 0.80, 1.0);
    }

    /* Mild gray-world white balance correction (clamped to avoid unnatural tints) */
    gdouble avg_r = MAX (8.0, (gdouble) sum_r / (gdouble) count);
    gdouble avg_g = MAX (8.0, (gdouble) sum_g / (gdouble) count);
    gdouble avg_b = MAX (8.0, (gdouble) sum_b / (gdouble) count);
    gdouble gray = (avg_r + avg_g + avg_b) / 3.0;

    gdouble wb_r = CLAMP (1.0 + (gray / avg_r - 1.0) * 0.45, 0.88, 1.15);
    gdouble wb_g = CLAMP (1.0 + (gray / avg_g - 1.0) * 0.45, 0.92, 1.08);
    gdouble wb_b = CLAMP (1.0 + (gray / avg_b - 1.0) * 0.45, 0.88, 1.15);

    const gdouble ema = 0.18;
    self->auto_black += (target_black - self->auto_black) * ema;
    self->auto_white += (target_white - self->auto_white) * ema;
    self->auto_gamma += (target_gamma - self->auto_gamma) * ema;
    self->auto_wb_r  += (wb_r - self->auto_wb_r) * ema;
    self->auto_wb_g  += (wb_g - self->auto_wb_g) * ema;
    self->auto_wb_b  += (wb_b - self->auto_wb_b) * ema;

    self->lut_dirty = TRUE;
}

static inline gdouble
sd_rounded_box (gdouble px, gdouble py, gdouble bx, gdouble by, gdouble r)
{
    gdouble qx = fabs (px) - bx + r;
    gdouble qy = fabs (py) - by + r;
    gdouble out_x = MAX (qx, 0.0);
    gdouble out_y = MAX (qy, 0.0);
    return MIN (MAX (qx, qy), 0.0) + hypot (out_x, out_y) - r;
}

static gdouble
eval_shape_sdf (RecGoWebcamShape shape, gdouble px, gdouble py, gdouble half_w, gdouble half_h)
{
    if (shape == RECGO_SHAPE_CIRCLE) {
        gdouble radius = MIN (half_w, half_h);
        return hypot (px, py) - radius;
    } else if (shape == RECGO_SHAPE_SQUARE) {
        gdouble half = MIN (half_w, half_h);
        gdouble corner = CLAMP (half * 0.18, 8.0, 26.0);
        return sd_rounded_box (px, py, half, half, corner);
    } else {
        gdouble corner = CLAMP (MIN (half_w, half_h) * 0.14, 6.0, 18.0);
        return sd_rounded_box (px, py, half_w, half_h, corner);
    }
}

static void
ensure_mask_lut_locked (RecGoVideoFx *self, gint width, gint height)
{
    if (!self->mask_enabled)
        return;

    if (self->mask_lut
        && self->mask_w == width
        && self->mask_h == height
        && self->mask_cached_shape == self->shape
        && self->mask_cached_inset == self->mask_inset
        && self->mask_cached_border == self->cfg.border_enabled
        && self->mask_cached_shadow == self->cfg.shadow_enabled) {
        return;
    }

    g_free (self->mask_lut);
    g_free (self->row_opaque_start);
    g_free (self->row_opaque_end);

    gsize total = (gsize) width * (gsize) height;
    self->mask_lut = g_new0 (RecGoMaskPixel, total);
    self->row_opaque_start = g_new (gint, (gsize) height);
    self->row_opaque_end   = g_new (gint, (gsize) height);

    self->mask_w = width;
    self->mask_h = height;
    self->mask_cached_shape = self->shape;
    self->mask_cached_inset = self->mask_inset;
    self->mask_cached_border = self->cfg.border_enabled;
    self->mask_cached_shadow = self->cfg.shadow_enabled;

    gdouble inset = CLAMP ((gdouble) self->mask_inset, 0.0, MIN (width, height) * 0.25);
    gdouble cx = (gdouble) width * 0.5;
    gdouble cy = (gdouble) height * 0.5;
    gdouble half_w = MAX (4.0, cx - inset - 1.0);
    gdouble half_h = MAX (4.0, cy - inset - 1.0);

    gdouble border_w = self->cfg.border_enabled
        ? CLAMP (MIN (half_w, half_h) * 0.022, 2.0, 4.0)
        : 0.0;
    const gdouble border_opacity = 0.92;
    const gdouble shadow_dy = 4.5;
    const gdouble shadow_blur = MAX (4.0, inset - 3.5);
    const gdouble shadow_max_alpha = 0.62;

    for (gint y = 0; y < height; y++) {
        gdouble py = ((gdouble) y + 0.5) - cy;
        gint op_start = width;
        gint op_end = -1;

        for (gint x = 0; x < width; x++) {
            gdouble px = ((gdouble) x + 0.5) - cx;
            gdouble d = eval_shape_sdf (self->shape, px, py, half_w, half_h);

            /* Outer silhouette coverage (1 inside shape, 0 outside, 1.4px AA) */
            gdouble shape_cov = 1.0 - smoothstep_d (-0.7, 0.7, d);

            gdouble border_wgt = 0.0;
            gdouble video_wgt = shape_cov;
            if (border_w > 0.0) {
                /* Inner edge of border ring sits at d = -border_w */
                gdouble inner_cov = 1.0 - smoothstep_d (-border_w - 0.6, -border_w + 0.6, d);
                gdouble ring_cov = MAX (0.0, shape_cov - inner_cov);
                border_wgt = ring_cov * border_opacity;
                video_wgt = inner_cov + ring_cov * (1.0 - border_opacity);
            }

            gdouble fg_a = CLAMP (video_wgt + border_wgt, 0.0, 1.0);

            gdouble shadow_a = 0.0;
            if (self->cfg.shadow_enabled && inset >= 4.0 && fg_a < 0.999) {
                gdouble ds = eval_shape_sdf (self->shape, px, py - shadow_dy, half_w, half_h);
                if (ds < shadow_blur) {
                    gdouble s_falloff = 1.0 - smoothstep_d (-1.0, shadow_blur, ds);
                    shadow_a = s_falloff * shadow_max_alpha;
                }
            }

            gdouble out_a = CLAMP (fg_a + shadow_a * (1.0 - fg_a), 0.0, 1.0);

            RecGoMaskPixel *mp = &self->mask_lut[(gsize) y * (gsize) width + (gsize) x];
            if (out_a <= 0.002) {
                mp->kind = 0;
                mp->v_scale = 0;
                mp->b_add = 0;
                mp->out_alpha = 0;
            } else if (video_wgt >= 0.998 && border_wgt <= 0.002 && out_a >= 0.998) {
                mp->kind = 1;
                mp->v_scale = 255;
                mp->b_add = 0;
                mp->out_alpha = 255;
                if (op_start == width)
                    op_start = x;
                op_end = x;
            } else {
                mp->kind = 2;
                gint vs = (gint) lround (255.0 * (video_wgt / out_a));
                gint ba = (gint) lround (255.0 * (border_wgt / out_a));
                if (vs + ba > 255)
                    ba = MAX (0, 255 - vs);
                mp->v_scale = clamp_u8 (vs);
                mp->b_add = clamp_u8 (ba);
                mp->out_alpha = clamp_u8 ((gint) lround (255.0 * out_a));
            }
        }

        self->row_opaque_start[y] = op_start;
        self->row_opaque_end[y]   = op_end;
    }
}

static guint8 *
ensure_scratch_locked (RecGoVideoFx *self, gsize needed)
{
    if (needed > self->scratch_cap) {
        g_free (self->scratch);
        self->scratch = g_malloc (needed);
        self->scratch_cap = needed;
    }
    return self->scratch;
}

static void
apply_zoom_and_mirror_locked (RecGoVideoFx *self,
                              guint8 *rgba,
                              gint width,
                              gint height,
                              gint stride,
                              gdouble zoom,
                              gboolean mirror)
{
    if (zoom <= 1.01) {
        if (!mirror)
            return;
        for (gint y = 0; y < height; y++) {
            guint32 *row = (guint32 *) (rgba + (gsize) y * (gsize) stride);
            gint left = 0, right = width - 1;
            while (left < right) {
                guint32 tmp = row[left];
                row[left] = row[right];
                row[right] = tmp;
                left++;
                right--;
            }
        }
        return;
    }

    gsize packed_stride = (gsize) width * 4;
    gsize frame_bytes = packed_stride * (gsize) height;
    guint8 *src_copy = ensure_scratch_locked (self, frame_bytes);
    for (gint y = 0; y < height; y++) {
        memcpy (src_copy + (gsize) y * packed_stride,
                rgba + (gsize) y * (gsize) stride,
                packed_stride);
    }

    gdouble crop_w = (gdouble) width / zoom;
    gdouble crop_h = (gdouble) height / zoom;
    gdouble start_x = ((gdouble) width - crop_w) * 0.5;
    gdouble start_y = ((gdouble) height - crop_h) * 0.5;
    gdouble step_x = crop_w / (gdouble) MAX (1, width);
    gdouble step_y = crop_h / (gdouble) MAX (1, height);

    /* Fast fixed-point 16.16 bilinear sampling (also folds horizontal mirror for free) */
    gint32 sx_step = (gint32) lround ((mirror ? -step_x : step_x) * 65536.0);
    gint32 sx_init = (gint32) lround ((mirror ? (start_x + crop_w - step_x) : start_x) * 65536.0);
    gint max_x = width - 1;
    gint max_y = height - 1;

    for (gint y = 0; y < height; y++) {
        gdouble fy = start_y + ((gdouble) y + 0.5) * step_y - 0.5;
        gint y0 = CLAMP ((gint) floor (fy), 0, max_y);
        gint y1 = MIN (y0 + 1, max_y);
        guint wy = (guint) CLAMP ((gint) lround ((fy - (gdouble) y0) * 256.0), 0, 256);
        guint iwy = 256 - wy;

        const guint8 *row0 = src_copy + (gsize) y0 * packed_stride;
        const guint8 *row1 = src_copy + (gsize) y1 * packed_stride;
        guint8 *dst = rgba + (gsize) y * (gsize) stride;

        gint32 fx = sx_init;
        for (gint x = 0; x < width; x++) {
            gint x0 = fx >> 16;
            guint wx = (guint) ((fx >> 8) & 0xFF);
            if (x0 < 0) {
                x0 = 0;
                wx = 0;
            } else if (x0 >= max_x) {
                x0 = max_x;
                wx = 0;
            }
            gint x1 = MIN (x0 + 1, max_x);
            guint iwx = 256 - wx;

            const guint8 *p00 = row0 + x0 * 4;
            const guint8 *p10 = row0 + x1 * 4;
            const guint8 *p01 = row1 + x0 * 4;
            const guint8 *p11 = row1 + x1 * 4;

            guint w00 = iwx * iwy;
            guint w10 = wx  * iwy;
            guint w01 = iwx * wy;
            guint w11 = wx  * wy;

            dst[0] = (guint8) ((p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11 + 32768) >> 16);
            dst[1] = (guint8) ((p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 + 32768) >> 16);
            dst[2] = (guint8) ((p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11 + 32768) >> 16);
            dst[3] = 255;

            dst += 4;
            fx += sx_step;
        }
    }
}

static void
apply_temporal_denoise_locked (RecGoVideoFx *self,
                               guint8 *rgba,
                               gint width,
                               gint height,
                               gint stride,
                               gdouble strength)
{
    gsize packed_stride = (gsize) width * 4;
    gsize frame_bytes = packed_stride * (gsize) height;

    if (self->prev_w != width || self->prev_h != height || self->prev_cap < frame_bytes) {
        g_free (self->prev_frame);
        self->prev_frame = g_malloc (frame_bytes);
        self->prev_cap = frame_bytes;
        self->prev_w = width;
        self->prev_h = height;
        for (gint y = 0; y < height; y++) {
            memcpy (self->prev_frame + (gsize) y * packed_stride,
                    rgba + (gsize) y * (gsize) stride,
                    packed_stride);
        }
        return;
    }

    gint thresh3 = (10 + (gint) lround (strength * 26.0)) * 3;
    gint prev_w = (gint) lround (strength * 164.0); /* 0..164 out of 256 */
    gint curr_w = 256 - prev_w;

    for (gint y = 0; y < height; y++) {
        guint8 *cur = rgba + (gsize) y * (gsize) stride;
        guint8 *prv = self->prev_frame + (gsize) y * packed_stride;
        for (gint x = 0; x < width; x++) {
            gint dr = (gint) cur[0] - (gint) prv[0];
            gint dg = (gint) cur[1] - (gint) prv[1];
            gint db = (gint) cur[2] - (gint) prv[2];
            gint sad = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);

            if (sad < thresh3) {
                guint8 r = (guint8) ((cur[0] * curr_w + prv[0] * prev_w + 128) >> 8);
                guint8 g = (guint8) ((cur[1] * curr_w + prv[1] * prev_w + 128) >> 8);
                guint8 b = (guint8) ((cur[2] * curr_w + prv[2] * prev_w + 128) >> 8);
                cur[0] = prv[0] = r;
                cur[1] = prv[1] = g;
                cur[2] = prv[2] = b;
            } else {
                prv[0] = cur[0];
                prv[1] = cur[1];
                prv[2] = cur[2];
            }
            cur += 4;
            prv += 4;
        }
    }
}

static void
apply_sharpness_locked (RecGoVideoFx *self,
                        guint8 *rgba,
                        gint width,
                        gint height,
                        gint stride,
                        gdouble strength)
{
    if (width < 3 || height < 3)
        return;

    gsize packed_stride = (gsize) width * 4;
    gsize frame_bytes = packed_stride * (gsize) height;
    guint8 *src = ensure_scratch_locked (self, frame_bytes);
    for (gint y = 0; y < height; y++) {
        memcpy (src + (gsize) y * packed_stride,
                rgba + (gsize) y * (gsize) stride,
                packed_stride);
    }

    /* Fast integer 5-tap unsharp mask on RGB */
    gint amt = (gint) lround (strength * 224.0);

    for (gint y = 1; y < height - 1; y++) {
        const guint8 *top = src + (gsize) (y - 1) * packed_stride;
        const guint8 *mid = src + (gsize) y * packed_stride;
        const guint8 *bot = src + (gsize) (y + 1) * packed_stride;
        guint8 *dst = rgba + (gsize) y * (gsize) stride;

        for (gint x = 1; x < width - 1; x++) {
            gsize off = (gsize) x * 4;
            for (gint c = 0; c < 3; c++) {
                gint center = mid[off + c];
                gint detail = (center << 2)
                            - top[off + c]
                            - bot[off + c]
                            - mid[off - 4 + c]
                            - mid[off + 4 + c];
                dst[off + c] = clamp_u8 (center + ((detail * amt) >> 10));
            }
        }
    }
}

static void
apply_mask_locked (RecGoVideoFx *self,
                   guint8 *rgba,
                   gint width,
                   gint height,
                   gint stride)
{
    ensure_mask_lut_locked (self, width, height);
    if (!self->mask_lut)
        return;

    for (gint y = 0; y < height; y++) {
        guint8 *row = rgba + (gsize) y * (gsize) stride;
        const RecGoMaskPixel *lut_row = self->mask_lut + (gsize) y * (gsize) width;
        gint op_start = self->row_opaque_start[y];
        gint op_end   = self->row_opaque_end[y];

        /* Process pixels before the opaque interior span */
        gint left_limit = (op_start <= op_end) ? op_start : width;
        for (gint x = 0; x < left_limit; x++) {
            const RecGoMaskPixel *mp = &lut_row[x];
            guint8 *px = row + (gsize) x * 4;
            if (mp->kind == 0) {
                px[0] = 0;
                px[1] = 0;
                px[2] = 0;
                px[3] = 0;
            } else if (mp->kind == 2) {
                guint vs = mp->v_scale;
                guint ba = mp->b_add;
                px[0] = clamp_u8 ((gint) (((guint) px[0] * vs + 127) / 255 + ba));
                px[1] = clamp_u8 ((gint) (((guint) px[1] * vs + 127) / 255 + ba));
                px[2] = clamp_u8 ((gint) (((guint) px[2] * vs + 127) / 255 + ba));
                px[3] = mp->out_alpha;
            }
        }

        /* Interior span [op_start..op_end] is 100% opaque video: skip entirely! */

        /* Process pixels after the opaque interior span */
        if (op_start <= op_end) {
            for (gint x = op_end + 1; x < width; x++) {
                const RecGoMaskPixel *mp = &lut_row[x];
                guint8 *px = row + (gsize) x * 4;
                if (mp->kind == 0) {
                    px[0] = 0;
                    px[1] = 0;
                    px[2] = 0;
                    px[3] = 0;
                } else if (mp->kind == 2) {
                    guint vs = mp->v_scale;
                    guint ba = mp->b_add;
                    px[0] = clamp_u8 ((gint) (((guint) px[0] * vs + 127) / 255 + ba));
                    px[1] = clamp_u8 ((gint) (((guint) px[1] * vs + 127) / 255 + ba));
                    px[2] = clamp_u8 ((gint) (((guint) px[2] * vs + 127) / 255 + ba));
                    px[3] = mp->out_alpha;
                }
            }
        }
    }
}

static gboolean
needs_any_processing_locked (const RecGoVideoFx *self)
{
    if (self->mask_enabled)
        return TRUE;
    const RecGoWebcamImageConfig *c = &self->cfg;
    return c->mirror
        || c->auto_enhance
        || fabs (c->brightness) > 0.004
        || fabs (c->contrast - 1.0) > 0.004
        || fabs (c->saturation - 1.0) > 0.004
        || fabs (c->warmth) > 0.004
        || fabs (c->gamma - 1.0) > 0.004
        || c->sharpness > 0.01
        || c->denoise > 0.01
        || c->zoom > 1.01;
}

void
recgo_video_fx_process_rgba (RecGoVideoFx *self,
                             guint8 *rgba,
                             gint width,
                             gint height,
                             gint stride)
{
    g_return_if_fail (RECGO_IS_VIDEO_FX (self));
    g_return_if_fail (rgba != NULL && width > 0 && height > 0);

    g_mutex_lock (&self->lock);

    if (!needs_any_processing_locked (self)) {
        g_mutex_unlock (&self->lock);
        return;
    }

    /* 1. Digital Zoom + Horizontal Mirror */
    if (self->cfg.zoom > 1.01 || self->cfg.mirror) {
        apply_zoom_and_mirror_locked (self, rgba, width, height, stride,
                                      self->cfg.zoom, self->cfg.mirror);
    }

    /* 2. Auto Low-Light Enhance periodic subsampled statistics */
    if (self->cfg.auto_enhance) {
        if ((self->frame_counter++ % 4) == 0) {
            update_auto_enhance_stats_locked (self, rgba, width, height, stride);
        }
    }

    /* 3. Rebuild RGB tone LUT if needed */
    if (self->lut_dirty) {
        rebuild_tone_lut_locked (self);
    }

    /* 4. Apply 256-byte RGB tone LUT + fast integer saturation in a single pass */
    gboolean do_sat = fabs (self->cfg.saturation - 1.0) > 0.004;
    if (self->has_tone_lut || do_sat) {
        const guint8 *lr = self->lut_r;
        const guint8 *lg = self->lut_g;
        const guint8 *lb = self->lut_b;
        gint sat_fp = (gint) lround (self->cfg.saturation * 256.0);

        if (self->has_tone_lut && !do_sat) {
            for (gint y = 0; y < height; y++) {
                guint8 *px = rgba + (gsize) y * (gsize) stride;
                for (gint x = 0; x < width; x++) {
                    px[0] = lr[px[0]];
                    px[1] = lg[px[1]];
                    px[2] = lb[px[2]];
                    px += 4;
                }
            }
        } else if (!self->has_tone_lut && do_sat) {
            for (gint y = 0; y < height; y++) {
                guint8 *px = rgba + (gsize) y * (gsize) stride;
                for (gint x = 0; x < width; x++) {
                    gint r = px[0], g = px[1], b = px[2];
                    gint luma = (77 * r + 150 * g + 29 * b) >> 8;
                    px[0] = clamp_u8 (luma + (((r - luma) * sat_fp) >> 8));
                    px[1] = clamp_u8 (luma + (((g - luma) * sat_fp) >> 8));
                    px[2] = clamp_u8 (luma + (((b - luma) * sat_fp) >> 8));
                    px += 4;
                }
            }
        } else {
            for (gint y = 0; y < height; y++) {
                guint8 *px = rgba + (gsize) y * (gsize) stride;
                for (gint x = 0; x < width; x++) {
                    gint r = lr[px[0]];
                    gint g = lg[px[1]];
                    gint b = lb[px[2]];
                    gint luma = (77 * r + 150 * g + 29 * b) >> 8;
                    px[0] = clamp_u8 (luma + (((r - luma) * sat_fp) >> 8));
                    px[1] = clamp_u8 (luma + (((g - luma) * sat_fp) >> 8));
                    px[2] = clamp_u8 (luma + (((b - luma) * sat_fp) >> 8));
                    px += 4;
                }
            }
        }
    }

    /* 5. Edge-preserving temporal noise reduction */
    if (self->cfg.denoise > 0.01) {
        apply_temporal_denoise_locked (self, rgba, width, height, stride, self->cfg.denoise);
    } else if (self->prev_frame) {
        g_clear_pointer (&self->prev_frame, g_free);
        self->prev_cap = 0;
        self->prev_w = 0;
        self->prev_h = 0;
    }

    /* 6. Fast 5-tap unsharp mask */
    if (self->cfg.sharpness > 0.01) {
        apply_sharpness_locked (self, rgba, width, height, stride, self->cfg.sharpness);
    }

    /* 7. Precomputed antialiased SDF mask + border + drop shadow */
    if (self->mask_enabled) {
        apply_mask_locked (self, rgba, width, height, stride);
    }

    g_mutex_unlock (&self->lock);
}

static GstPadProbeReturn
on_pad_buffer_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    RecGoVideoFx *self = RECGO_VIDEO_FX (user_data);

    g_mutex_lock (&self->lock);
    gboolean active = needs_any_processing_locked (self);
    g_mutex_unlock (&self->lock);

    if (!active)
        return GST_PAD_PROBE_OK;

    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
    if (!buf)
        return GST_PAD_PROBE_OK;

    if (!self->has_vinfo) {
        GstCaps *caps = gst_pad_get_current_caps (pad);
        if (!caps)
            return GST_PAD_PROBE_OK;
        gboolean ok = gst_video_info_from_caps (&self->vinfo, caps);
        gst_caps_unref (caps);
        if (!ok || GST_VIDEO_INFO_FORMAT (&self->vinfo) != GST_VIDEO_FORMAT_RGBA)
            return GST_PAD_PROBE_OK;
        self->has_vinfo = TRUE;
    }

    buf = gst_buffer_make_writable (buf);
    GST_PAD_PROBE_INFO_DATA (info) = buf;

    GstVideoFrame frame;
    if (!gst_video_frame_map (&frame, &self->vinfo, buf, GST_MAP_READWRITE))
        return GST_PAD_PROBE_OK;

    guint8 *data = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);
    gint width = GST_VIDEO_FRAME_WIDTH (&frame);
    gint height = GST_VIDEO_FRAME_HEIGHT (&frame);
    gint stride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);

    recgo_video_fx_process_rgba (self, data, width, height, stride);

    gst_video_frame_unmap (&frame);
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
on_pad_caps_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void) pad;
    RecGoVideoFx *self = RECGO_VIDEO_FX (user_data);
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
    if (event && GST_EVENT_TYPE (event) == GST_EVENT_CAPS) {
        GstCaps *caps = NULL;
        gst_event_parse_caps (event, &caps);
        if (caps && gst_video_info_from_caps (&self->vinfo, caps)) {
            self->has_vinfo = (GST_VIDEO_INFO_FORMAT (&self->vinfo) == GST_VIDEO_FORMAT_RGBA);
        } else {
            self->has_vinfo = FALSE;
        }
    }
    return GST_PAD_PROBE_OK;
}

RecGoVideoFx *
recgo_video_fx_new (const RecGoWebcamImageConfig *image_cfg,
                    gboolean mask_enabled,
                    RecGoWebcamShape shape,
                    gint mask_inset)
{
    RecGoVideoFx *self = g_object_new (RECGO_TYPE_VIDEO_FX, NULL);
    if (image_cfg) {
        self->cfg = *image_cfg;
        sanitize_image_config (&self->cfg);
    }
    self->mask_enabled = mask_enabled;
    self->shape = shape;
    self->mask_inset = MAX (0, mask_inset);
    self->lut_dirty = TRUE;
    return self;
}

void
recgo_video_fx_update_image_config (RecGoVideoFx *self,
                                    const RecGoWebcamImageConfig *image_cfg)
{
    g_return_if_fail (RECGO_IS_VIDEO_FX (self));
    g_return_if_fail (image_cfg != NULL);

    g_mutex_lock (&self->lock);
    self->cfg = *image_cfg;
    sanitize_image_config (&self->cfg);
    self->lut_dirty = TRUE;
    g_mutex_unlock (&self->lock);
}

void
recgo_video_fx_update_mask (RecGoVideoFx *self,
                            gboolean mask_enabled,
                            RecGoWebcamShape shape,
                            gint mask_inset)
{
    g_return_if_fail (RECGO_IS_VIDEO_FX (self));

    g_mutex_lock (&self->lock);
    self->mask_enabled = mask_enabled;
    self->shape = shape;
    self->mask_inset = MAX (0, mask_inset);
    g_mutex_unlock (&self->lock);
}

void
recgo_video_fx_attach_to_pad (RecGoVideoFx *self, GstPad *pad)
{
    g_return_if_fail (RECGO_IS_VIDEO_FX (self));
    g_return_if_fail (GST_IS_PAD (pad));

    gst_pad_add_probe (pad,
                       GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                       on_pad_caps_probe,
                       g_object_ref (self),
                       g_object_unref);

    gst_pad_add_probe (pad,
                       GST_PAD_PROBE_TYPE_BUFFER,
                       on_pad_buffer_probe,
                       g_object_ref (self),
                       g_object_unref);
}

static void
recgo_video_fx_finalize (GObject *object)
{
    RecGoVideoFx *self = RECGO_VIDEO_FX (object);
    g_free (self->scratch);
    g_free (self->prev_frame);
    g_free (self->mask_lut);
    g_free (self->row_opaque_start);
    g_free (self->row_opaque_end);
    g_mutex_clear (&self->lock);
    G_OBJECT_CLASS (recgo_video_fx_parent_class)->finalize (object);
}

static void
recgo_video_fx_class_init (RecGoVideoFxClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->finalize = recgo_video_fx_finalize;
}

static void
recgo_video_fx_init (RecGoVideoFx *self)
{
    g_mutex_init (&self->lock);
    recgo_webcam_image_config_init_defaults (&self->cfg);
    self->lut_dirty = TRUE;
    self->auto_black = 0.0;
    self->auto_white = 1.0;
    self->auto_gamma = 1.0;
    self->auto_wb_r = 1.0;
    self->auto_wb_g = 1.0;
    self->auto_wb_b = 1.0;
}
