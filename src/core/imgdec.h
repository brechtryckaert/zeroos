/* Decode a JPEG/PNG/... (from memory or a file) into a small RGB565 LVGL
 * image, scaled to fit (or fill) a box. Uses MuPDF, which ZeroOS already
 * links for the reader; large JPEGs are decoded at reduced size, so an 8 MP
 * photo becomes a thumbnail without ever existing at full size.
 * Thread-safe: call from worker threads. Free with zos_img_free. */
#ifndef ZOS_IMGDEC_H
#define ZOS_IMGDEC_H

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

/* cover: fill the whole box, cropping the overflow (thumbnails);
 * otherwise fit inside it. Returns NULL on failure. */
lv_image_dsc_t *zos_img_decode_mem(const void *data, size_t len, int box_w, int box_h, bool cover);
lv_image_dsc_t *zos_img_decode_file(const char *path, int box_w, int box_h, bool cover);
void zos_img_free(lv_image_dsc_t *img);

#endif
