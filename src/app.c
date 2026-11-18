/* App registry and lifecycle. Opening an app creates a fresh screen and input
 * group; exiting tears both down and returns to the launcher. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "zos.h"

extern const zos_app_t zos_app_music;
extern const zos_app_t zos_app_video;
extern const zos_app_t zos_app_reader;
extern const zos_app_t zos_app_camera;
extern const zos_app_t zos_app_emby;
extern const zos_app_t zos_app_meshtastic;
extern const zos_app_t zos_app_meshcore;
extern const zos_app_t zos_app_tailscale;
extern const zos_app_t zos_app_ssh;
extern const zos_app_t zos_app_usb;
extern const zos_app_t zos_app_settings;

const zos_app_t *const zos_apps[] = {
    &zos_app_music,
    &zos_app_video,
    &zos_app_reader,
    &zos_app_camera,
    &zos_app_emby,
    &zos_app_meshtastic,
    &zos_app_meshcore,
    &zos_app_tailscale,
    &zos_app_ssh,
    &zos_app_usb,
    &zos_app_settings,
};
const int zos_app_count = sizeof(zos_apps) / sizeof(zos_apps[0]);

static const zos_app_t *cur_app;
static lv_obj_t *cur_scr;
static lv_group_t *cur_group;

void zos_input_set_group(lv_group_t *g)
{
    if (zos_keypad) lv_indev_set_group(zos_keypad, g);
}

void zos_app_launch(const zos_app_t *app)
{
    if (cur_app) return;
    cur_app = app;
    cur_scr = lv_obj_create(NULL);
    zos_theme_screen(cur_scr);
    cur_group = lv_group_create();
    lv_group_set_wrap(cur_group, false);
    zos_input_set_group(cur_group);
    app->open(cur_scr, cur_group);
    lv_screen_load(cur_scr);
}

static void finish_exit(void *unused)
{
    LV_UNUSED(unused);
    if (!cur_app) return;
    const zos_app_t *app = cur_app;
    cur_app = NULL;
    if (app->close) app->close();
    zos_launcher_show();
    lv_obj_delete(cur_scr);
    lv_group_delete(cur_group);
    cur_scr = NULL;
    cur_group = NULL;
}

void zos_app_exit(void)
{
    /* Deferred so an app can call this from its own key handler. */
    lv_async_call(finish_exit, NULL);
}

const zos_app_t *zos_app_current(void) { return cur_app; }

static const zos_app_t *switch_to;

static void do_switch(void *unused)
{
    LV_UNUSED(unused);
    if (cur_app) finish_exit(NULL);
    if (switch_to) zos_app_launch(switch_to);
    switch_to = NULL;
}

void zos_app_switch(const zos_app_t *app)
{
    switch_to = app;
    lv_async_call(do_switch, NULL);
}

/* ---------- placeholder body for apps that are still to be written ---------- */

void zos_placeholder_body(lv_obj_t *scr, const zos_app_t *app, const char *text)
{
    zos_statusbar_create(scr, app->name);

    lv_obj_t *tile = lv_obj_create(scr);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 52, 52);
    lv_obj_set_style_radius(tile, 14, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(app->color), 0);
    lv_obj_align(tile, LV_ALIGN_LEFT_MID, 22, 6);
    lv_obj_t *ic = lv_image_create(tile);
    lv_image_set_src(ic, app->icon);
    lv_obj_set_style_image_recolor(ic, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
    lv_obj_center(ic);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lbl, 210);
    lv_obj_set_style_text_color(lbl, lv_color_hex(ZOS_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(lbl, zos_font.small, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 92, 6);
}

/* ---------- utils ---------- */

char *zos_path_join(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = lv_malloc(n);
    snprintf(p, n, "%s/%s", a, b);
    return p;
}

void zos_mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        LV_LOG_WARN("mkdir %s failed: %s", tmp, strerror(errno));
}
