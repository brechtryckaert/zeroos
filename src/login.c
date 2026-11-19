/* The login screen at boot: pick who you are (←/→), then type your PIN or
 * password if you have one. With no accounts yet it is the welcome screen
 * instead, which makes the first one (the admin, who takes over everything
 * already on the device). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zos.h"
#include "core/form.h"
#include "core/users.h"
#include "core/settings.h"

#define AV 46          /* avatar size */
#define SLOT 74        /* avatar spacing */

static struct {
    lv_obj_t *scr, *strip, *title, *secret, *msg, *hint, *keys;
    lv_obj_t *av[16];
    lv_group_t *g;
    int sel, n;
    bool typing;
    void (*done)(void);
    /* welcome */
    zos_form_t *form;
    zos_field_t fields[3];
    char name[32], pass[64], rep[64];
} L;

static void finish(const zos_user_t *u)
{
    zos_users_login(u);
    lv_obj_t *scr = L.scr;
    lv_group_t *g = L.g;
    void (*done)(void) = L.done;
    memset(&L, 0, sizeof L);
    done();                       /* builds and loads the launcher */
    lv_obj_delete_async(scr);
    lv_group_delete(g);
}

/* ---------------- picker ---------------- */

static void show_msg(const char *m)
{
    lv_label_set_text(L.msg, m);
    if (m[0]) lv_obj_add_flag(L.hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(L.hint, LV_OBJ_FLAG_HIDDEN);
}

static void set_hint(void)
{
    const zos_user_t *u = zos_users_get(L.sel);
    if (L.typing)
        lv_label_set_text(L.hint, LV_SYMBOL_NEW_LINE " log in      Esc back");
    else
        lv_label_set_text_fmt(L.hint, "%s" LV_SYMBOL_NEW_LINE " %s",
                              L.n > 1 ? LV_SYMBOL_LEFT " " LV_SYMBOL_RIGHT "  choose      " : "",
                              u && u->lock != ZOS_LOCK_NONE ? "unlock" : "log in");
}

static void select_user(int i)
{
    if (i < 0) i = 0;
    if (i >= L.n) i = L.n - 1;
    L.sel = i;
    for (int k = 0; k < L.n; k++) {
        if (k == i) lv_obj_add_state(L.av[k], LV_STATE_CHECKED);
        else lv_obj_remove_state(L.av[k], LV_STATE_CHECKED);
    }
    /* Keep the chosen one in the middle once there are more than fit. */
    int total = L.n * SLOT;
    int x = total <= ZOS_HOR_RES ? (ZOS_HOR_RES - total) / 2 : ZOS_HOR_RES / 2 - (i * SLOT + SLOT / 2);
    if (total > ZOS_HOR_RES) {
        if (x > 0) x = 0;
        if (x < ZOS_HOR_RES - total) x = ZOS_HOR_RES - total;
    }
    lv_obj_set_x(L.strip, x);
    show_msg("");
    set_hint();
}

static void stop_typing(void)
{
    L.typing = false;
    lv_textarea_set_text(L.secret, "");
    lv_obj_add_flag(L.secret, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(L.keys);
    set_hint();
}

static void start_typing(const zos_user_t *u)
{
    L.typing = true;
    lv_textarea_set_text(L.secret, "");
    lv_textarea_set_placeholder_text(L.secret, u->lock == ZOS_LOCK_PIN ? "PIN" : "Password");
    lv_textarea_set_accepted_chars(L.secret, u->lock == ZOS_LOCK_PIN ? "0123456789" : NULL);
    lv_obj_remove_flag(L.secret, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(L.secret);
    show_msg("");
    set_hint();
}

static void try_login(void)
{
    const zos_user_t *u = zos_users_get(L.sel);
    if (!u) return;
    if (u->lock == ZOS_LOCK_NONE) { finish(u); return; }
    if (!L.typing) { start_typing(u); return; }
    char err[96];
    if (zos_users_check(u, lv_textarea_get_text(L.secret), err, sizeof err)) {
        finish(u);
        return;
    }
    lv_textarea_set_text(L.secret, "");
    show_msg(err);
}

static void keys_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (k) {
    case LV_KEY_RIGHT: case LV_KEY_DOWN: select_user(L.sel + 1); break;
    case LV_KEY_LEFT: case LV_KEY_UP: select_user(L.sel - 1); break;
    case LV_KEY_ENTER: case ' ': try_login(); break;
    default:
        /* Typing a name's first letter jumps to it. */
        if (k < 128 && isalnum((int)k))
            for (int j = 1; j <= L.n; j++) {
                int i = (L.sel + j) % L.n;
                if (toupper((int)zos_users_get(i)->name[0]) == toupper((int)k)) { select_user(i); break; }
            }
    }
}

static void secret_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) try_login();
    else if (c == LV_EVENT_KEY) {
        uint32_t k = lv_event_get_key(e);
        if (k != LV_KEY_ENTER) show_msg("");
        if (k == LV_KEY_ESC || k == '`') stop_typing();
    }
}

static lv_obj_t *avatar(lv_obj_t *parent, const zos_user_t *u, int i)
{
    static lv_style_t st, st_sel;
    static bool inited;
    if (!inited) {
        inited = true;
        static const lv_style_prop_t props[] = {LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y,
                                                LV_STYLE_OUTLINE_OPA, 0};
        static lv_style_transition_dsc_t tr;
        lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 120, 0, NULL);
        lv_style_init(&st);
        lv_style_set_size(&st, AV, AV);
        lv_style_set_radius(&st, LV_RADIUS_CIRCLE);
        lv_style_set_bg_opa(&st, LV_OPA_COVER);
        lv_style_set_outline_color(&st, lv_color_white());
        lv_style_set_outline_width(&st, 2);
        lv_style_set_outline_pad(&st, 3);
        lv_style_set_outline_opa(&st, LV_OPA_TRANSP);
        lv_style_set_transform_pivot_x(&st, AV / 2);
        lv_style_set_transform_pivot_y(&st, AV / 2);
        lv_style_set_transition(&st, &tr);
        lv_style_init(&st_sel);
        lv_style_set_transform_scale(&st_sel, 284);
        lv_style_set_outline_opa(&st_sel, LV_OPA_COVER);
        lv_style_set_transition(&st_sel, &tr);
    }
    lv_obj_t *a = lv_obj_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_add_style(a, &st, 0);
    lv_obj_add_style(a, &st_sel, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(a, lv_color_hex(u->color), 0);
    lv_obj_set_pos(a, i * SLOT + (SLOT - AV) / 2, 6);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *ini = lv_label_create(a);
    char t[8] = "?";
    /* The first character, UTF-8 included. */
    const unsigned char *s = (const unsigned char *)u->name;
    int len = s[0] < 0x80 ? 1 : s[0] < 0xe0 ? 2 : s[0] < 0xf0 ? 3 : 4;
    snprintf(t, sizeof t, "%.*s", len, u->name);
    if (len == 1) t[0] = (char)toupper(s[0]);
    lv_label_set_text(ini, t);
    lv_obj_set_style_text_font(ini, zos_font.large, 0);
    lv_obj_set_style_text_color(ini, lv_color_white(), 0);
    lv_obj_center(ini);

    lv_obj_t *nm = lv_label_create(parent);
    lv_obj_set_style_text_font(nm, zos_font.small, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(ZOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(nm, SLOT - 4);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(nm, u->name);
    lv_obj_set_pos(nm, i * SLOT + 2, AV + 16);
    return a;
}

static void build_picker(void)
{
    L.n = zos_users_count();
    L.title = lv_label_create(L.scr);
    lv_obj_set_style_text_font(L.title, zos_font.body, 0);
    lv_obj_set_style_text_color(L.title, lv_color_hex(ZOS_COLOR_MUTED), 0);
    lv_label_set_text(L.title, L.n > 1 ? "Who's using ZeroOS?" : "Welcome back");
    lv_obj_align(L.title, LV_ALIGN_TOP_MID, 0, ZOS_STATUSBAR_H + 4);

    lv_obj_t *row = lv_obj_create(L.scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ZOS_HOR_RES, AV + 34);
    lv_obj_set_pos(row, 0, ZOS_STATUSBAR_H + 22);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    L.strip = lv_obj_create(row);
    lv_obj_remove_style_all(L.strip);
    lv_obj_set_size(L.strip, L.n * SLOT, AV + 34);
    lv_obj_remove_flag(L.strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < L.n && i < 16; i++) L.av[i] = avatar(L.strip, zos_users_get(i), i);

    L.keys = lv_obj_create(L.scr);   /* invisible focus target for the picker */
    lv_obj_remove_style_all(L.keys);
    lv_obj_set_size(L.keys, 1, 1);
    lv_obj_add_event_cb(L.keys, keys_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(L.g, L.keys);

    L.secret = lv_textarea_create(L.scr);
    lv_textarea_set_one_line(L.secret, true);
    lv_textarea_set_password_mode(L.secret, true);
    lv_textarea_set_max_length(L.secret, 63);
    lv_obj_set_size(L.secret, 170, 26);
    lv_obj_align(L.secret, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_set_style_text_font(L.secret, zos_font.body, 0);
    lv_obj_set_style_text_align(L.secret, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(L.secret, 3, 0);
    lv_obj_set_style_radius(L.secret, 8, 0);
    lv_obj_set_style_bg_color(L.secret, lv_color_hex(ZOS_COLOR_SURFACE), 0);
    lv_obj_set_style_border_width(L.secret, 1, 0);
    lv_obj_set_style_border_color(L.secret, lv_color_hex(ZOS_COLOR_ACCENT), 0);
    lv_obj_set_style_outline_width(L.secret, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_text_color(L.secret, lv_color_hex(0x6b7280), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(L.secret, secret_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(L.secret, LV_OBJ_FLAG_HIDDEN);
    lv_group_add_obj(L.g, L.secret);

    L.msg = lv_label_create(L.scr);
    lv_obj_set_style_text_font(L.msg, zos_font.small, 0);
    lv_obj_set_style_text_color(L.msg, lv_color_hex(0xf87171), 0);
    lv_label_set_text(L.msg, "");
    lv_obj_align(L.msg, LV_ALIGN_BOTTOM_MID, 0, -3);

    L.hint = lv_label_create(L.scr);
    lv_obj_set_style_text_font(L.hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(L.hint, lv_color_hex(0x4b5563), 0);
    lv_obj_align(L.hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* Start on whoever used it last. */
    int start = 0;
    const char *last = zos_setting_str("last_user", "");
    for (int i = 0; i < L.n; i++)
        if (!strcmp(zos_users_get(i)->id, last)) start = i;
    lv_group_focus_obj(L.keys);
    select_user(start);
}

/* ---------------- first run ---------------- */

static void welcome_submit(zos_form_t *f, void *ud)
{
    LV_UNUSED(ud);
    char err[128];
    char *nm = L.name;
    while (*nm == ' ') nm++;
    for (size_t l = strlen(nm); l && nm[l - 1] == ' '; l--) nm[l - 1] = 0;
    const zos_user_t *u = zos_users_add(nm, L.pass, L.rep, true, err, sizeof err);
    if (!u) {
        zos_form_message(f, err, true);
        return;
    }
    finish(u);
}

static void welcome_cancel(zos_form_t *f, void *ud)
{
    LV_UNUSED(ud);
    zos_form_message(f, "Make an account to start: a name is enough.", true);
}

static void build_welcome(void)
{
    L.name[0] = 0;
    L.fields[0] = (zos_field_t){.label = "Your name", .buf = L.name, .cap = 21, .placeholder = "what should we call you?"};
    L.fields[1] = (zos_field_t){.label = "PIN / pass", .buf = L.pass, .cap = sizeof L.pass,
                                .placeholder = "optional", .password = true};
    L.fields[2] = (zos_field_t){.label = "Repeat", .buf = L.rep, .cap = sizeof L.rep, .password = true};
    L.form = zos_form_create(L.scr, L.g, "Welcome to ZeroOS", L.fields, 3, welcome_submit, welcome_cancel, NULL);
    zos_form_message(L.form,
                     "Make your account: you'll be the admin, and everything already on this "
                     "device becomes yours. 4-8 digits make a PIN; leave it empty for no lock.",
                     false);
    zos_form_focus(L.form);
}

void zos_login_start(void (*done)(void))
{
    memset(&L, 0, sizeof L);
    L.done = done;
    L.scr = lv_obj_create(NULL);
    zos_theme_screen(L.scr);
    L.g = lv_group_create();
    lv_group_set_wrap(L.g, false);
    zos_input_set_group(L.g);
    zos_statusbar_create(L.scr, "ZeroOS");
    if (zos_users_count() == 0) build_welcome();
    else build_picker();
    lv_screen_load(L.scr);
}
