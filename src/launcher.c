/* Home screen: a scrolling row of app tiles with the selected one centred.
 * Left/Right moves, Enter (or 1-9) opens. Also: the shared status bar and
 * notification toasts. */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "zos.h"
#include "apps/music/music.h"
#include "mesh/mesh.h"
#include "core/tailscale.h"

static lv_obj_t *home_scr;
static lv_group_t *home_group;
static lv_obj_t *tiles[16], *badges[16], *strip;
static lv_obj_t *name_lbl, *tag_lbl;
static int sel;

bool zos_fake_status;   /* set by --demo-status, for screenshots in the simulator */

/* ---------- status bar ---------- */

typedef struct {
    lv_obj_t *clock, *wifi, *batt, *batt_icon, *mail, *lora, *vpn, *music;
} statusbar_t;

static int read_battery_percent(void)
{
    if (zos_fake_status) return 78;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    struct dirent *e;
    int pct = -1;
    while ((e = readdir(d)) && pct < 0) {
        if (e->d_name[0] == '.') continue;
        char p[300], type[32] = "";
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(type, sizeof type, f) && strncmp(type, "Battery", 7) == 0) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
            FILE *c = fopen(p, "r");
            if (c) {
                if (fscanf(c, "%d", &pct) != 1) pct = -1;
                fclose(c);
            }
        }
        fclose(f);
    }
    closedir(d);
    return pct;
}

static bool wifi_up(void)
{
    if (zos_fake_status) return true;
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f) return false;
    char s[16] = "";
    bool up = fgets(s, sizeof s, f) && strncmp(s, "up", 2) == 0;
    fclose(f);
    return up;
}

static void statusbar_tick(lv_timer_t *t)
{
    statusbar_t *sb = lv_timer_get_user_data(t);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (zos_fake_status) { tm.tm_hour = 17; tm.tm_min = 5; }
    lv_label_set_text_fmt(sb->clock, "%02d:%02d", tm.tm_hour, tm.tm_min);

    int unread = mesh_total_unread();
    if (unread) {
        lv_label_set_text_fmt(sb->mail, LV_SYMBOL_ENVELOPE " %d", unread);
        lv_obj_remove_flag(sb->mail, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sb->mail, LV_OBJ_FLAG_HIDDEN);
    }
    if (music_active()) lv_obj_remove_flag(sb->music, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sb->music, LV_OBJ_FLAG_HIDDEN);

    mesh_t *m = mesh_active();
    if (m) {
        lv_obj_set_style_text_color(sb->lora, lv_color_hex(m->state == MESH_READY ? 0x22c55e : 0x6b7280), 0);
        lv_obj_remove_flag(sb->lora, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sb->lora, LV_OBJ_FLAG_HIDDEN);
    }

    if (ts_connected()) lv_obj_remove_flag(sb->vpn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sb->vpn, LV_OBJ_FLAG_HIDDEN);

    if (wifi_up()) lv_obj_remove_flag(sb->wifi, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sb->wifi, LV_OBJ_FLAG_HIDDEN);

    int pct = read_battery_percent();
    if (pct < 0) {
        lv_obj_add_flag(sb->batt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sb->batt_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *sym = pct > 85 ? LV_SYMBOL_BATTERY_FULL : pct > 60 ? LV_SYMBOL_BATTERY_3
                        : pct > 35 ? LV_SYMBOL_BATTERY_2 : pct > 12 ? LV_SYMBOL_BATTERY_1
                        : LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(sb->batt, "%d%%", pct);
        lv_label_set_text(sb->batt_icon, sym);
        lv_obj_remove_flag(sb->batt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sb->batt_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void statusbar_deleted(lv_event_t *e)
{
    lv_timer_t *t = lv_event_get_user_data(e);
    lv_free(lv_timer_get_user_data(t));
    lv_timer_delete(t);
}

lv_obj_t *zos_statusbar_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, ZOS_HOR_RES, ZOS_STATUSBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_style_text_font(bar, zos_font.small, 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(ZOS_COLOR_MUTED), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_color(t, lv_color_hex(ZOS_COLOR_TEXT), 0);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);

    statusbar_t *sb = lv_malloc(sizeof *sb);
    sb->mail = lv_label_create(bar);
    lv_obj_set_style_text_font(sb->mail, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sb->mail, lv_color_hex(ZOS_COLOR_ACCENT), 0);
    sb->music = lv_label_create(bar);
    lv_obj_set_style_text_font(sb->music, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sb->music, lv_color_hex(0xe5484d), 0);
    lv_label_set_text(sb->music, LV_SYMBOL_AUDIO);
    sb->lora = lv_label_create(bar);
    lv_obj_set_style_text_font(sb->lora, &lv_font_montserrat_10, 0);
    lv_label_set_text(sb->lora, "LoRa");
    sb->vpn = lv_label_create(bar);
    lv_obj_set_style_text_font(sb->vpn, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(sb->vpn, lv_color_hex(0x818cf8), 0);
    lv_label_set_text(sb->vpn, "VPN");
    sb->wifi = lv_label_create(bar);
    /* symbol glyphs live in Montserrat; using it directly avoids a TTF miss per glyph */
    lv_obj_set_style_text_font(sb->wifi, &lv_font_montserrat_12, 0);
    lv_label_set_text(sb->wifi, LV_SYMBOL_WIFI);
    sb->batt = lv_label_create(bar);
    sb->batt_icon = lv_label_create(bar);
    lv_obj_set_style_text_font(sb->batt_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_left(sb->batt_icon, -3, 0);
    sb->clock = lv_label_create(bar);
    lv_obj_set_style_text_color(sb->clock, lv_color_hex(ZOS_COLOR_TEXT), 0);

    lv_timer_t *tm = lv_timer_create(statusbar_tick, 1000, sb);
    statusbar_tick(tm);
    lv_obj_add_event_cb(bar, statusbar_deleted, LV_EVENT_DELETE, tm);
    return bar;
}

/* ---------- notifications ---------- */

static lv_obj_t *toast;
static lv_timer_t *toast_timer;

static void toast_done(lv_timer_t *t)
{
    LV_UNUSED(t);
    toast_timer = NULL;
    if (toast) lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

void zos_notify(uint32_t color, const char *title, const char *text)
{
    if (!toast) {
        toast = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(toast);
        lv_obj_set_size(toast, ZOS_HOR_RES - 12, 38);
        lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 3);
        lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(toast, lv_color_hex(0x1f242c), 0);
        lv_obj_set_style_radius(toast, 10, 0);
        lv_obj_set_style_border_side(toast, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(toast, 4, 0);
        lv_obj_set_style_shadow_width(toast, 12, 0);
        lv_obj_set_style_shadow_color(toast, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(toast, LV_OPA_50, 0);
        lv_obj_set_style_pad_hor(toast, 9, 0);
        lv_obj_set_style_pad_ver(toast, 3, 0);
        lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *t = lv_label_create(toast);
        lv_obj_set_style_text_font(t, zos_font.small, 0);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *b = lv_label_create(toast);
        lv_obj_set_style_text_font(b, zos_font.body, 0);
        lv_obj_set_style_text_color(b, lv_color_hex(ZOS_COLOR_TEXT), 0);
        lv_obj_set_width(b, lv_pct(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    lv_obj_set_style_border_color(toast, lv_color_hex(color), 0);
    lv_obj_t *t = lv_obj_get_child(toast, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(color), 0);
    lv_label_set_text(t, title);
    lv_label_set_text(lv_obj_get_child(toast, 1), text);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
    if (toast_timer) lv_timer_delete(toast_timer);
    toast_timer = lv_timer_create(toast_done, 3500, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

static void mesh_notify(mesh_t *m, mesh_conv_t *c, const mesh_msg_t *msg)
{
    char title[96];
    if (c->is_channel)
        snprintf(title, sizeof title, "%s  \xc2\xb7  # %s  \xc2\xb7  %s", m->name, c->title, msg->from ? msg->from : "?");
    else
        snprintf(title, sizeof title, "%s  \xc2\xb7  %s", m->name, msg->from ? msg->from : c->title);
    uint32_t color = 0x22b573;
    for (int i = 0; i < zos_app_count; i++)
        if (!strcmp(zos_apps[i]->id, m->kind == MESH_MESHTASTIC ? "meshtastic" : "meshcore")) color = zos_apps[i]->color;
    zos_notify(color, title, msg->text);
}

/* ---------- tiles ---------- */

static void badge_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    for (int i = 0; i < zos_app_count; i++) {
        int n = zos_apps[i]->badge ? zos_apps[i]->badge() : 0;
        if (n) {
            lv_label_set_text_fmt(badges[i], "%d", n > 99 ? 99 : n);
            lv_obj_remove_flag(badges[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(badges[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

#define TILE 54
#define GAP 12

static int strip_x_for(int i) { return ZOS_HOR_RES / 2 - TILE / 2 - i * (TILE + GAP); }

static void anim_x(void *obj, int32_t v) { lv_obj_set_x(obj, v); }

static void select_tile(int i)
{
    if (i < 0) i = 0;
    if (i >= zos_app_count) i = zos_app_count - 1;
    lv_obj_remove_state(tiles[sel], LV_STATE_CHECKED);
    sel = i;
    lv_obj_add_state(tiles[sel], LV_STATE_CHECKED);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, strip);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_values(&a, lv_obj_get_x(strip), strip_x_for(sel));
    lv_anim_set_duration(&a, 140);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_label_set_text(name_lbl, zos_apps[sel]->name);
    lv_label_set_text(tag_lbl, zos_apps[sel]->tagline);
}

static void home_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (k) {
    case LV_KEY_RIGHT: case LV_KEY_DOWN: select_tile(sel + 1); break;
    case LV_KEY_LEFT:  case LV_KEY_UP:   select_tile(sel - 1); break;
    case LV_KEY_ENTER: case ' ':         zos_app_launch(zos_apps[sel]); break;
    default:
        if (k >= '1' && k <= '9' && (int)(k - '1') < zos_app_count) {
            select_tile(k - '1');
            zos_app_launch(zos_apps[sel]);
        }
    }
}

void zos_launcher_create(void)
{
    home_scr = lv_screen_active();
    zos_theme_screen(home_scr);
    zos_statusbar_create(home_scr, "ZeroOS");
    mesh_notify_hook = mesh_notify;

    static lv_style_t st_tile, st_sel;
    static const lv_style_prop_t props[] = {
        LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, LV_STYLE_TRANSLATE_Y,
        LV_STYLE_OUTLINE_OPA, 0
    };
    static lv_style_transition_dsc_t tr;
    lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 120, 0, NULL);

    lv_style_init(&st_tile);
    lv_style_set_size(&st_tile, TILE, TILE);
    lv_style_set_radius(&st_tile, 15);
    lv_style_set_bg_opa(&st_tile, LV_OPA_COVER);
    lv_style_set_outline_color(&st_tile, lv_color_white());
    lv_style_set_outline_width(&st_tile, 2);
    lv_style_set_outline_pad(&st_tile, 3);
    lv_style_set_outline_opa(&st_tile, LV_OPA_TRANSP);
    lv_style_set_transform_pivot_x(&st_tile, TILE / 2);
    lv_style_set_transform_pivot_y(&st_tile, TILE / 2);
    lv_style_set_transition(&st_tile, &tr);

    lv_style_init(&st_sel);
    lv_style_set_transform_scale(&st_sel, 282);   /* 256 = 100% */
    lv_style_set_translate_y(&st_sel, -3);
    lv_style_set_outline_opa(&st_sel, LV_OPA_COVER);
    lv_style_set_transition(&st_sel, &tr);

    /* Invisible focus target that receives the key events. */
    lv_obj_t *row = lv_obj_create(home_scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ZOS_HOR_RES, TILE + 16);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, ZOS_STATUSBAR_H + 14);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, home_key, LV_EVENT_KEY, NULL);

    strip = lv_obj_create(row);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, zos_app_count * (TILE + GAP) + TILE, TILE + 16);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < zos_app_count; i++) {
        lv_obj_t *t = lv_obj_create(strip);
        lv_obj_remove_style_all(t);
        lv_obj_add_style(t, &st_tile, 0);
        lv_obj_add_style(t, &st_sel, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(t, lv_color_hex(zos_apps[i]->color), 0);
        lv_obj_set_pos(t, i * (TILE + GAP), 8);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        lv_obj_t *ic = lv_image_create(t);
        lv_image_set_src(ic, zos_apps[i]->icon);
        lv_obj_set_style_image_recolor(ic, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
        lv_obj_center(ic);
        tiles[i] = t;

        lv_obj_t *b = lv_label_create(t);   /* unread badge, for apps that have one */
        lv_obj_set_style_text_font(b, zos_font.small, 0);
        lv_obj_set_style_text_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xe5484d), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 9, 0);
        lv_obj_set_style_pad_hor(b, 5, 0);
        lv_obj_set_style_pad_ver(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(ZOS_COLOR_BG), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 7, -7);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        badges[i] = b;
    }
    int start = 0;
    for (int i = 0; i < zos_app_count; i++)
        if (!strcmp(zos_apps[i]->id, "reader")) start = i;
    lv_obj_set_x(strip, strip_x_for(start));
    lv_timer_create(badge_tick, 500, NULL);

    name_lbl = lv_label_create(home_scr);
    lv_obj_set_style_text_font(name_lbl, zos_font.large, 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 104);

    tag_lbl = lv_label_create(home_scr);
    lv_obj_set_style_text_font(tag_lbl, zos_font.small, 0);
    lv_obj_set_style_text_color(tag_lbl, lv_color_hex(ZOS_COLOR_MUTED), 0);
    lv_obj_align(tag_lbl, LV_ALIGN_TOP_MID, 0, 128);

    lv_obj_t *hint = lv_label_create(home_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_label_set_text(hint, LV_SYMBOL_LEFT " " LV_SYMBOL_RIGHT "  choose      "
                            LV_SYMBOL_NEW_LINE "  open");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x4b5563), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    home_group = lv_group_create();
    lv_group_add_obj(home_group, row);
    sel = start;
    for (int i = 0; i < zos_app_count; i++) lv_obj_remove_state(tiles[i], LV_STATE_CHECKED);
    select_tile(sel);
    zos_input_set_group(home_group);
}

void zos_launcher_show(void)
{
    zos_input_set_group(home_group);
    lv_screen_load(home_scr);
}
