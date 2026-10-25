#include <string.h>
#include "../zos.h"
#include "form.h"

#define MAX_FIELDS 6

struct zos_form {
    lv_obj_t *root, *msg;
    lv_group_t *g;
    zos_field_t *fields;
    lv_obj_t *ta[MAX_FIELDS];
    int n, cur;
    zos_form_cb submit, cancel;
    void *ud;
};

static void focus_field(zos_form_t *f, int i)
{
    if (i < 0) i = 0;
    if (i >= f->n) i = f->n - 1;
    f->cur = i;
    lv_group_remove_all_objs(f->g);
    lv_group_add_obj(f->g, f->ta[i]);
    lv_group_focus_obj(f->ta[i]);
    lv_textarea_set_cursor_pos(f->ta[i], LV_TEXTAREA_CURSOR_LAST);
}

static void do_submit(zos_form_t *f)
{
    for (int i = 0; i < f->n; i++) {
        const char *t = lv_textarea_get_text(f->ta[i]);
        size_t cap = f->fields[i].cap;
        strncpy(f->fields[i].buf, t, cap - 1);
        f->fields[i].buf[cap - 1] = 0;
    }
    if (f->submit) f->submit(f, f->ud);
}

static void ta_event(lv_event_t *e)
{
    zos_form_t *f = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {   /* Enter in a one-line textarea */
        if (f->cur < f->n - 1) focus_field(f, f->cur + 1);
        else do_submit(f);
    } else if (code == LV_EVENT_KEY) {
        uint32_t k = lv_event_get_key(e);
        if (k == LV_KEY_DOWN || k == LV_KEY_NEXT) focus_field(f, f->cur + 1);
        else if (k == LV_KEY_UP || k == LV_KEY_PREV) focus_field(f, f->cur - 1);
        else if (k == LV_KEY_ESC && f->cancel) f->cancel(f, f->ud);
    }
}

static void deleted(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

zos_form_t *zos_form_create(lv_obj_t *parent, lv_group_t *g, const char *title, zos_field_t *fields, int n,
                            zos_form_cb submit, zos_form_cb cancel, void *ud)
{
    zos_form_t *f = lv_zalloc(sizeof *f);
    f->g = g;
    f->fields = fields;
    f->n = n > MAX_FIELDS ? MAX_FIELDS : n;
    f->submit = submit;
    f->cancel = cancel;
    f->ud = ud;

    f->root = lv_obj_create(parent);
    lv_obj_remove_style_all(f->root);
    lv_obj_set_size(f->root, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f->root, lv_color_hex(ZOS_COLOR_BG), 0);
    lv_obj_remove_flag(f->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(f->root, deleted, LV_EVENT_DELETE, f);
    zos_statusbar_create(f->root, title);

    int row_h = f->n > 4 ? 23 : 27;
    for (int i = 0; i < f->n; i++) {
        int y = ZOS_STATUSBAR_H + 4 + i * row_h;
        lv_obj_t *l = lv_label_create(f->root);
        lv_obj_set_style_text_font(l, zos_font.small, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(ZOS_COLOR_MUTED), 0);
        lv_label_set_text(l, fields[i].label);
        lv_obj_set_pos(l, 8, y + 5);

        lv_obj_t *ta = lv_textarea_create(f->root);
        lv_textarea_set_one_line(ta, true);
        lv_obj_set_size(ta, ZOS_HOR_RES - 82, row_h - 4);
        lv_obj_set_pos(ta, 74, y);
        lv_obj_set_style_text_font(ta, zos_font.body, 0);
        lv_obj_set_style_pad_ver(ta, 2, 0);
        lv_obj_set_style_pad_hor(ta, 6, 0);
        lv_obj_set_style_radius(ta, 6, 0);
        lv_obj_set_style_bg_color(ta, lv_color_hex(ZOS_COLOR_SURFACE), 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(0x323944), 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(ZOS_COLOR_ACCENT), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_text_color(ta, lv_color_hex(0x6b7280), LV_PART_TEXTAREA_PLACEHOLDER);
        if (fields[i].placeholder) lv_textarea_set_placeholder_text(ta, fields[i].placeholder);
        if (fields[i].password) lv_textarea_set_password_mode(ta, true);
        if (fields[i].digits) lv_textarea_set_accepted_chars(ta, "0123456789");
        lv_textarea_set_max_length(ta, (uint32_t)fields[i].cap - 1);
        lv_textarea_set_text(ta, fields[i].buf);
        lv_obj_add_event_cb(ta, ta_event, LV_EVENT_ALL, f);
        f->ta[i] = ta;
    }

    f->msg = lv_label_create(f->root);
    lv_obj_set_style_text_font(f->msg, zos_font.small, 0);
    lv_obj_set_width(f->msg, ZOS_HOR_RES - 16);
    lv_label_set_long_mode(f->msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(f->msg, 8, ZOS_STATUSBAR_H + 6 + f->n * row_h);
    lv_label_set_text(f->msg, "");

    lv_obj_t *hint = lv_label_create(f->root);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5b6472), 0);
    lv_label_set_text(hint, LV_SYMBOL_UP LV_SYMBOL_DOWN " field    " LV_SYMBOL_NEW_LINE " next / done    Esc cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
    return f;
}

lv_obj_t *zos_form_obj(zos_form_t *f) { return f->root; }

void zos_form_focus(zos_form_t *f)
{
    int first_empty = 0;
    for (int i = 0; i < f->n; i++)
        if (!*lv_textarea_get_text(f->ta[i])) {
            first_empty = i;
            break;
        }
    focus_field(f, first_empty);
}

void zos_form_message(zos_form_t *f, const char *msg, bool error)
{
    lv_label_set_text(f->msg, msg ? msg : "");
    lv_obj_set_style_text_color(f->msg, lv_color_hex(error ? 0xf87171 : ZOS_COLOR_MUTED), 0);
}
