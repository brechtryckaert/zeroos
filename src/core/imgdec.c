#include <mupdf/fitz.h>
#include <stdlib.h>
#include <string.h>
#include "imgdec.h"

static lv_image_dsc_t *decode(fz_context *ctx, fz_image *im, int bw, int bh, bool cover)
{
    lv_image_dsc_t *out = NULL;
    fz_pixmap *pix = NULL, *scaled = NULL, *rgb = NULL;
    fz_var(out);
    fz_var(pix);
    fz_var(scaled);
    fz_var(rgb);
    fz_try(ctx) {
        int iw = im->w, ih = im->h;
        if (iw <= 0 || ih <= 0) fz_throw(ctx, FZ_ERROR_GENERIC, "empty image");
        float sx = (float)bw / iw, sy = (float)bh / ih;
        float s = cover ? fz_max(sx, sy) : fz_min(sx, sy);
        int tw = (int)(iw * s + 0.5f), th = (int)(ih * s + 0.5f);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        /* Asking for the target size lets MuPDF subsample while decoding. */
        fz_matrix ctm = fz_scale((float)tw, (float)th);
        int w = tw, h = th;
        pix = fz_get_pixmap_from_image(ctx, im, NULL, &ctm, &w, &h);
        if (pix->w != tw || pix->h != th) {
            scaled = fz_scale_pixmap(ctx, pix, 0, 0, (float)tw, (float)th, NULL);
            if (!scaled) fz_throw(ctx, FZ_ERROR_GENERIC, "scale failed");
        } else {
            scaled = fz_keep_pixmap(ctx, pix);
        }
        if (scaled->n - scaled->alpha != 3 || scaled->colorspace != fz_device_rgb(ctx))
            rgb = fz_convert_pixmap(ctx, scaled, fz_device_rgb(ctx), NULL, NULL, fz_default_color_params, 0);
        else
            rgb = fz_keep_pixmap(ctx, scaled);

        int ow = cover ? (tw < bw ? tw : bw) : tw;
        int oh = cover ? (th < bh ? th : bh) : th;
        int ox = (tw - ow) / 2, oy = (th - oh) / 2;
        size_t sz = (size_t)ow * oh * 2;
        out = malloc(sizeof *out + sz);
        if (!out) fz_throw(ctx, FZ_ERROR_GENERIC, "oom");
        memset(out, 0, sizeof *out);
        uint8_t *dst = (uint8_t *)(out + 1);
        int n = rgb->n;
        for (int y = 0; y < oh; y++) {
            const uint8_t *s0 = rgb->samples + (size_t)(y + oy) * rgb->stride + (size_t)ox * n;
            uint16_t *d = (uint16_t *)(dst + (size_t)y * ow * 2);
            for (int x = 0; x < ow; x++, s0 += n)
                d[x] = (uint16_t)(((s0[0] & 0xf8) << 8) | ((s0[1] & 0xfc) << 3) | (s0[2] >> 3));
        }
        out->header.magic = LV_IMAGE_HEADER_MAGIC;
        out->header.cf = LV_COLOR_FORMAT_RGB565;
        out->header.w = (uint32_t)ow;
        out->header.h = (uint32_t)oh;
        out->header.stride = (uint32_t)ow * 2;
        out->data_size = (uint32_t)sz;
        out->data = dst;
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, rgb);
        fz_drop_pixmap(ctx, scaled);
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) {
        free(out);
        out = NULL;
    }
    return out;
}

static lv_image_dsc_t *run(const void *data, size_t len, const char *path, int bw, int bh, bool cover)
{
    fz_context *ctx = fz_new_context(NULL, NULL, 8 << 20);
    if (!ctx) return NULL;
    lv_image_dsc_t *out = NULL;
    fz_image *im = NULL;
    fz_buffer *buf = NULL;
    fz_var(out);
    fz_var(im);
    fz_var(buf);
    fz_try(ctx) {
        if (path) im = fz_new_image_from_file(ctx, path);
        else {
            buf = fz_new_buffer_from_copied_data(ctx, data, len);
            im = fz_new_image_from_buffer(ctx, buf);
        }
        out = decode(ctx, im, bw, bh, cover);
    }
    fz_always(ctx) {
        fz_drop_image(ctx, im);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) { out = NULL; }
    fz_drop_context(ctx);
    return out;
}

lv_image_dsc_t *zos_img_decode_mem(const void *data, size_t len, int bw, int bh, bool cover)
{
    return data && len ? run(data, len, NULL, bw, bh, cover) : NULL;
}

lv_image_dsc_t *zos_img_decode_file(const char *path, int bw, int bh, bool cover)
{
    return run(NULL, 0, path, bw, bh, cover);
}

void zos_img_free(lv_image_dsc_t *img)
{
    if (!img) return;
    lv_image_cache_drop(img);
    free(img);
}
