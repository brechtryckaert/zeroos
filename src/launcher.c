/* Home screen: a row of app tiles. Left/Right moves, Enter (or 1-9) opens. */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "zos.h"

static lv_obj_t *home_scr;
static lv_group_t *home_group;
static lv_obj_t *tiles[16];
static lv_obj_t *name_lbl, *tag_lbl;
static int sel;

bool zos_fake_status;   /* set by --demo-status, for screenshots in the simulator */

/* ---------- status bar ---------- */

typedef struct {
    lv_obj_t *clock, *wifi, *batt, *batt_icon;
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

    lv_timer_t *tm = lv_timer_create(statusbar_tick, 5000, sb);
    statusbar_tick(tm);
    lv_obj_add_event_cb(bar, statusbar_deleted, LV_EVENT_DELETE, tm);
    return bar;
}

/* ---------- tiles ---------- */

#define TILE 54
#define GAP 9

static void select_tile(int i)
{
    if (i < 0) i = 0;
    if (i >= zos_app_count) i = zos_app_count - 1;
    lv_obj_remove_state(tiles[sel], LV_STATE_CHECKED);
    sel = i;
    lv_obj_add_state(tiles[sel], LV_STATE_CHECKED);
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

    int total = zos_app_count * TILE + (zos_app_count - 1) * GAP;
    int x0 = (ZOS_HOR_RES - total) / 2;
    for (int i = 0; i < zos_app_count; i++) {
        lv_obj_t *t = lv_obj_create(row);
        lv_obj_remove_style_all(t);
        lv_obj_add_style(t, &st_tile, 0);
        lv_obj_add_style(t, &st_sel, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(t, lv_color_hex(zos_apps[i]->color), 0);
        lv_obj_set_pos(t, x0 + i * (TILE + GAP), 8);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *ic = lv_image_create(t);
        lv_image_set_src(ic, zos_apps[i]->icon);
        lv_obj_set_style_image_recolor(ic, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
        lv_obj_center(ic);
        tiles[i] = t;
    }

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
    sel = 2;   /* start on the reader for now: it's the app that works */
    for (int i = 0; i < zos_app_count; i++) lv_obj_remove_state(tiles[i], LV_STATE_CHECKED);
    select_tile(sel);
    zos_input_set_group(home_group);
}

void zos_launcher_show(void)
{
    zos_input_set_group(home_group);
    lv_screen_load(home_scr);
}
