#include <stdio.h>
#include <string.h>
#include "../zos.h"
#include "ui.h"

lv_obj_t *zos_ui_box(lv_obj_t *p)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *zos_ui_label(lv_obj_t *p, const lv_font_t *f, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *zos_ui_hint(lv_obj_t *p, const char *text)
{
    lv_obj_t *h = zos_ui_label(p, &lv_font_montserrat_10, 0x5b6472);
    lv_label_set_text(h, text ? text : "");
    lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(h, ZOS_HOR_RES - 6, lv_font_get_line_height(&lv_font_montserrat_10));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -1);
    return h;
}

lv_obj_t *zos_ui_page(lv_obj_t *scr)
{
    lv_obj_t *p = zos_ui_box(scr);
    lv_obj_set_size(p, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

void zos_ui_show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void zos_ui_fmt_time(double s, char *out, size_t n)
{
    if (s < 0) s = 0;
    int t = (int)s;
    if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

void zos_ui_fmt_size(double b, char *out, size_t n)
{
    if (b >= 1e9) snprintf(out, n, "%.1f GB", b / 1e9);
    else if (b >= 1e6) snprintf(out, n, "%.0f MB", b / 1e6);
    else snprintf(out, n, "%.0f kB", b / 1e3);
}

/* ---------------- virtual list ---------------- */

#define MAX_ROWS 12

typedef struct {
    lv_obj_t *obj, *slot, *img, *sym, *title, *sub, *right;
} row_objs_t;

struct zos_vlist {
    lv_obj_t *box, *empty, *bar, *knob;
    row_objs_t rows[MAX_ROWS];
    int nrows, row_h, thumb, w;
    int count, sel, top;
    zos_vlist_fill_cb fill;
    void *ud;
};

static void vlist_deleted(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

zos_vlist_t *zos_vlist_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t row_h, int32_t thumb)
{
    zos_vlist_t *l = lv_malloc_zeroed(sizeof *l);
    l->row_h = row_h;
    l->thumb = thumb;
    l->w = w;
    l->box = zos_ui_box(parent);
    lv_obj_set_pos(l->box, x, y);
    lv_obj_set_size(l->box, w, h);
    lv_obj_add_event_cb(l->box, vlist_deleted, LV_EVENT_DELETE, l);
    l->nrows = h / row_h;
    if (l->nrows > MAX_ROWS) l->nrows = MAX_ROWS;
    if (l->nrows < 1) l->nrows = 1;

    int32_t text_x = thumb ? thumb + 8 : 7;
    for (int i = 0; i < l->nrows; i++) {
        row_objs_t *r = &l->rows[i];
        r->obj = zos_ui_box(l->box);
        lv_obj_set_pos(r->obj, 0, i * row_h);
        lv_obj_set_size(r->obj, w - 4, row_h - 1);
        lv_obj_set_style_radius(r->obj, 6, 0);
        lv_obj_set_style_bg_color(r->obj, lv_color_hex(ZOS_COLOR_SURFACE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(r->obj, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(r->obj, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(r->obj, 3, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(r->obj, lv_color_hex(ZOS_COLOR_ACCENT), LV_STATE_CHECKED);
        if (thumb) {
            r->slot = zos_ui_box(r->obj);
            lv_obj_set_size(r->slot, thumb, thumb);
            lv_obj_set_style_radius(r->slot, 4, 0);
            lv_obj_set_style_clip_corner(r->slot, true, 0);
            lv_obj_set_style_bg_opa(r->slot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r->slot, lv_color_hex(0x232a33), 0);
            lv_obj_align(r->slot, LV_ALIGN_LEFT_MID, 5, 0);
            r->img = lv_image_create(r->slot);
            lv_obj_center(r->img);
            r->sym = zos_ui_label(r->slot, thumb >= 24 ? &lv_font_montserrat_14 : &lv_font_montserrat_12, 0x6b7280);
            lv_obj_center(r->sym);
        }
        r->right = zos_ui_label(r->obj, zos_font.small, ZOS_COLOR_MUTED);
        lv_obj_align(r->right, LV_ALIGN_RIGHT_MID, -6, 0);
        r->title = zos_ui_label(r->obj, zos_font.body, ZOS_COLOR_TEXT);
        r->sub = zos_ui_label(r->obj, zos_font.small, ZOS_COLOR_MUTED);
        (void)text_x;
    }
    l->bar = zos_ui_box(l->box);
    lv_obj_set_size(l->bar, 2, h - 4);
    lv_obj_align(l->bar, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_set_style_bg_opa(l->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l->bar, lv_color_hex(0x232a33), 0);
    l->knob = zos_ui_box(l->bar);
    lv_obj_set_width(l->knob, 2);
    lv_obj_set_style_bg_opa(l->knob, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l->knob, lv_color_hex(0x6b7280), 0);

    l->empty = zos_ui_label(l->box, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_set_width(l->empty, w - 20);
    lv_label_set_long_mode(l->empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l->empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l->empty, LV_ALIGN_CENTER, 0, 0);
    return l;
}

lv_obj_t *zos_vlist_obj(zos_vlist_t *l) { return l->box; }

void zos_vlist_set_empty_text(zos_vlist_t *l, const char *text) { lv_label_set_text(l->empty, text); }

static void layout_row(zos_vlist_t *l, row_objs_t *r, const zos_vrow_t *v)
{
    int32_t tx = l->thumb ? l->thumb + 10 : 8;
    int32_t right_w = 0;
    if (v->right[0]) {
        lv_label_set_text(r->right, v->right);
        right_w = lv_text_get_width(v->right, strlen(v->right), zos_font.small, 0) + 10;
        zos_ui_show(r->right, true);
    } else {
        zos_ui_show(r->right, false);
    }
    int32_t tw = l->w - 4 - tx - right_w - 6;
    lv_label_set_text(r->title, v->title);
    lv_obj_set_style_text_color(r->title, lv_color_hex(v->title_color ? v->title_color : ZOS_COLOR_TEXT), 0);
    zos_label_one_line(r->title, tw);
    if (v->sub[0]) {
        lv_label_set_text(r->sub, v->sub);
        zos_label_one_line(r->sub, tw);
        zos_ui_show(r->sub, true);
        lv_obj_align(r->title, LV_ALIGN_TOP_LEFT, tx, l->row_h >= 28 ? 1 : 0);
        lv_obj_align(r->sub, LV_ALIGN_BOTTOM_LEFT, tx, -2);
    } else {
        zos_ui_show(r->sub, false);
        lv_obj_align(r->title, LV_ALIGN_LEFT_MID, tx, 0);
    }
    if (l->thumb) {
        if (v->img) {
            lv_image_set_src(r->img, v->img);
            zos_ui_show(r->img, true);
            zos_ui_show(r->sym, false);
        } else {
            zos_ui_show(r->img, false);
            lv_label_set_text(r->sym, v->symbol ? v->symbol : "");
            lv_obj_set_style_text_color(r->sym, lv_color_hex(v->symbol_color ? v->symbol_color : 0x6b7280), 0);
            zos_ui_show(r->sym, v->symbol != NULL);
        }
        zos_ui_show(r->slot, v->img || v->symbol);
    }
}

static void header_row(zos_vlist_t *l, row_objs_t *r, const zos_vrow_t *v)
{
    lv_label_set_text(r->title, v->title);
    lv_obj_set_style_text_color(r->title, lv_color_hex(ZOS_COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(r->title, zos_font.small, 0);
    zos_label_one_line(r->title, l->w - 20);
    lv_obj_align(r->title, LV_ALIGN_BOTTOM_LEFT, 6, -2);
    zos_ui_show(r->sub, false);
    zos_ui_show(r->right, false);
    if (r->slot) zos_ui_show(r->slot, false);
}

void zos_vlist_refresh(zos_vlist_t *l)
{
    zos_ui_show(l->empty, l->count == 0);
    for (int i = 0; i < l->nrows; i++) {
        row_objs_t *r = &l->rows[i];
        int idx = l->top + i;
        if (idx >= l->count || !l->fill) {
            zos_ui_show(r->obj, false);
            continue;
        }
        zos_ui_show(r->obj, true);
        static zos_vrow_t v;
        memset(&v, 0, sizeof v);
        l->fill(idx, &v, l->ud);
        lv_obj_set_style_text_font(r->title, zos_font.body, 0);
        if (v.header) header_row(l, r, &v);
        else layout_row(l, r, &v);
        if (idx == l->sel && !v.header) lv_obj_add_state(r->obj, LV_STATE_CHECKED);
        else lv_obj_remove_state(r->obj, LV_STATE_CHECKED);
    }
    bool scroll = l->count > l->nrows;
    zos_ui_show(l->bar, scroll);
    if (scroll) {
        int32_t bh = lv_obj_get_height(l->bar);
        int32_t kh = bh * l->nrows / l->count;
        if (kh < 8) kh = 8;
        int32_t ky = (bh - kh) * l->top / (l->count - l->nrows);
        lv_obj_set_height(l->knob, kh);
        lv_obj_set_y(l->knob, ky);
    }
}

static bool is_header(zos_vlist_t *l, int i)
{
    if (i < 0 || i >= l->count || !l->fill) return false;
    static zos_vrow_t v;
    memset(&v, 0, sizeof v);
    l->fill(i, &v, l->ud);
    return v.header;
}

void zos_vlist_select(zos_vlist_t *l, int i)
{
    if (l->count == 0) {
        l->sel = 0;
        l->top = 0;
        zos_vlist_refresh(l);
        return;
    }
    if (i < 0) i = 0;
    if (i >= l->count) i = l->count - 1;
    int dir = i >= l->sel ? 1 : -1;
    while (is_header(l, i)) {
        int n = i + dir;
        if (n < 0 || n >= l->count) { dir = -dir; n = i + dir; }
        if (n < 0 || n >= l->count) break;
        i = n;
    }
    l->sel = i;
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + l->nrows) l->top = l->sel - l->nrows + 1;
    /* keep a heading visible above the first item of its section */
    if (l->top > 0 && l->top == l->sel && is_header(l, l->sel - 1)) l->top--;
    if (l->top > l->count - l->nrows) l->top = l->count - l->nrows;
    if (l->top < 0) l->top = 0;
    zos_vlist_refresh(l);
}

void zos_vlist_set(zos_vlist_t *l, int count, zos_vlist_fill_cb fill, void *ud)
{
    l->count = count;
    l->fill = fill;
    l->ud = ud;
    if (l->sel >= count) l->sel = count ? count - 1 : 0;
    zos_vlist_select(l, l->sel);
}

int zos_vlist_selected(const zos_vlist_t *l) { return l->count ? l->sel : -1; }
int zos_vlist_count(const zos_vlist_t *l) { return l->count; }

void zos_vlist_visible(const zos_vlist_t *l, int *first, int *last)
{
    *first = l->top;
    *last = l->top + l->nrows - 1;
    if (*last >= l->count) *last = l->count - 1;
}

bool zos_vlist_key(zos_vlist_t *l, uint32_t k)
{
    switch (k) {
    case LV_KEY_UP: zos_vlist_select(l, l->sel - 1); return true;
    case LV_KEY_DOWN: zos_vlist_select(l, l->sel + 1); return true;
    case ZOS_KEY_PGUP: zos_vlist_select(l, l->sel - l->nrows); return true;
    case ZOS_KEY_PGDN: zos_vlist_select(l, l->sel + l->nrows); return true;
    case LV_KEY_HOME: zos_vlist_select(l, 0); return true;
    case LV_KEY_END: zos_vlist_select(l, l->count - 1); return true;
    default: return false;
    }
}
