/* Small UI building blocks shared by the media and settings apps.
 *
 * zos_vlist is a "virtual" list: it keeps one LVGL object per visible row
 * and refills them as the selection moves, so a list of 5,000 songs costs
 * no more than a list of 5. Rows come from a fill callback. */
#ifndef ZOS_UI_H
#define ZOS_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

lv_obj_t *zos_ui_box(lv_obj_t *parent);
lv_obj_t *zos_ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);
/* The one-line key hint along the bottom edge. */
lv_obj_t *zos_ui_hint(lv_obj_t *parent, const char *text);
/* A full-screen page (child of the app screen) that views switch between. */
lv_obj_t *zos_ui_page(lv_obj_t *scr);
void zos_ui_show(lv_obj_t *o, bool on);
/* "1:02:03" or "4:05" */
void zos_ui_fmt_time(double s, char *out, size_t n);
/* "1.4 GB", "820 MB" */
void zos_ui_fmt_size(double bytes, char *out, size_t n);

typedef struct {
    char title[160];
    char sub[160];
    char right[40];
    const char *symbol;              /* LV_SYMBOL_* shown in the thumbnail slot */
    const lv_image_dsc_t *img;       /* or an image (caller keeps it alive) */
    uint32_t title_color;            /* 0 = default */
    uint32_t symbol_color;           /* 0 = muted */
    bool header;                     /* a section heading, not selectable */
} zos_vrow_t;

typedef void (*zos_vlist_fill_cb)(int index, zos_vrow_t *row, void *ud);

typedef struct zos_vlist zos_vlist_t;

/* `thumb` > 0 reserves a square image/symbol slot of that size per row. */
zos_vlist_t *zos_vlist_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t row_h, int32_t thumb);
void zos_vlist_set(zos_vlist_t *l, int count, zos_vlist_fill_cb fill, void *ud);
void zos_vlist_select(zos_vlist_t *l, int index);
int zos_vlist_selected(const zos_vlist_t *l);
int zos_vlist_count(const zos_vlist_t *l);
/* Indexes currently on screen (for loading thumbnails). */
void zos_vlist_visible(const zos_vlist_t *l, int *first, int *last);
/* Up/Down/PgUp/PgDn/Home/End. Returns true if the key was used. */
bool zos_vlist_key(zos_vlist_t *l, uint32_t key);
/* Re-run the fill callback for the visible rows. */
void zos_vlist_refresh(zos_vlist_t *l);
/* Shown in place of the rows when count is 0. */
void zos_vlist_set_empty_text(zos_vlist_t *l, const char *text);
lv_obj_t *zos_vlist_obj(zos_vlist_t *l);

#endif
