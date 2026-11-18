/* ZeroOS entry point: picks a display/input backend, sets up the shell and
 * runs the LVGL loop.
 *
 *   zeroos [--backend sdl|fbdev|headless] [--fb /dev/fb0] [--kbd /dev/input/eventN]
 *          [--rotate 0|90|180|270] [--books DIR] [--state DIR] [--fonts DIR]
 *          [--zoom N] [--demo-status] [--script "key:right;wait:200;shot:a.ppm"]
 *          [--radio-control | --no-radio-control]
 *
 * Mesh apps reach meshtasticd / the MeshCore companion on localhost; set
 * ZOS_MESHTASTIC_ADDR / ZOS_MESHCORE_ADDR (host:port) to point elsewhere.
 * ZeroOS only starts/stops those daemons through systemd when running on
 * the device (fbdev backend) or with --radio-control.
 *
 * The headless backend renders into memory and runs a key script, which is
 * how screenshots and smoke tests are produced without a window. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "zos.h"
#include "mesh/mesh.h"
#include "core/task.h"
#include "core/http.h"
#include "core/tailscale.h"
#include "core/hdmi.h"
#include "core/player.h"
#include "core/settings.h"

zos_config_t zos_cfg;
extern bool zos_fake_status;
static bool on_device;
bool zos_on_device(void) { return on_device; }

void zos_display_suspend(bool suspend)
{
    lv_display_t *d = lv_display_get_default();
    lv_display_enable_invalidation(d, !suspend);
    if (!suspend) lv_obj_invalidate(lv_screen_active());
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static const char *env_or(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

/* ---------------- headless backend + key script ---------------- */

static uint16_t headless_fb[ZOS_HOR_RES * ZOS_VER_RES];

static void headless_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    LV_UNUSED(a); LV_UNUSED(px);
    lv_display_flush_ready(d);
}

static void run_frames(int n)
{
    for (int i = 0; i < n; i++) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

/* Like run_frames, but in real time, so sockets and other processes keep up. */
static void run_realtime(int ms)
{
    for (int t = 0; t < ms; t += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
        usleep(10000);
    }
}

static void save_ppm(const char *path)
{
    lv_refr_now(NULL);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", ZOS_HOR_RES, ZOS_VER_RES);
    for (int i = 0; i < ZOS_HOR_RES * ZOS_VER_RES; i++) {
        uint16_t c = headless_fb[i];
        uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 0x1f) * 255 / 31),
                          (uint8_t)(((c >> 5) & 0x3f) * 255 / 63),
                          (uint8_t)((c & 0x1f) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("zeroos: saved %s\n", path);
}

static uint32_t key_from_name(const char *s)
{
    static const struct { const char *n; uint32_t k; } names[] = {
        {"right", LV_KEY_RIGHT}, {"left", LV_KEY_LEFT}, {"up", LV_KEY_UP}, {"down", LV_KEY_DOWN},
        {"enter", LV_KEY_ENTER}, {"esc", LV_KEY_ESC}, {"space", ' '}, {"backspace", LV_KEY_BACKSPACE},
        {"tab", ZOS_KEY_TAB}, {"semicolon", ';'}, {"pgup", ZOS_KEY_PGUP}, {"pgdn", ZOS_KEY_PGDN},
        {"del", LV_KEY_DEL}, {"home", LV_KEY_HOME}, {"end", LV_KEY_END},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(s, names[i].n) == 0) return names[i].k;
    return (uint8_t)s[0];
}

static void run_script(const char *script)
{
    char *copy = strdup(script), *save = NULL;
    run_frames(30);
    for (char *cmd = strtok_r(copy, ";", &save); cmd; cmd = strtok_r(NULL, ";", &save)) {
        while (*cmd == ' ') cmd++;
        char *arg = strchr(cmd, ':');
        if (!arg) continue;
        *arg++ = 0;
        int repeat = 1;
        char *star = strchr(arg, '*');      /* key:right*5 */
        if (star) { *star = 0; repeat = atoi(star + 1); }
        if (strcmp(cmd, "key") == 0) {
            for (int r = 0; r < repeat; r++) {
                zos_key_push(key_from_name(arg));
                while (!zos_key_queue_empty()) run_frames(1);
                run_frames(25);   /* let transitions and prefetch settle */
            }
        } else if (strcmp(cmd, "ctrl") == 0) {      /* ctrl:c */
            zos_key_push(ZOS_KEY_CTRL((uint8_t)arg[0]));
            while (!zos_key_queue_empty()) run_frames(1);
            run_frames(5);
        } else if (strcmp(cmd, "type") == 0) {      /* type:text (\n = Enter) */
            for (const char *c = arg; *c; c++) {
                if (c[0] == '\\' && c[1] == 'n') {
                    zos_key_push(LV_KEY_ENTER);
                    c++;
                } else {
                    zos_key_push((uint8_t)*c);
                }
                while (!zos_key_queue_empty()) run_frames(1);
            }
            run_frames(5);
        } else if (strcmp(cmd, "app") == 0) {      /* app:emby  (open by id) */
            for (int i = 0; i < zos_app_count; i++)
                if (!strcmp(zos_apps[i]->id, arg)) zos_app_switch(zos_apps[i]);
            run_frames(30);
        } else if (strcmp(cmd, "wait") == 0) {
            run_realtime(atoi(arg));
        } else if (strcmp(cmd, "shot") == 0) {
            save_ppm(arg);
        } else if (strcmp(cmd, "hdmishot") == 0) {   /* with ZOS_FAKE_HDMI=WxH */
            lv_refr_now(NULL);
            if (!zos_hdmi_save_ppm(arg)) fprintf(stderr, "zeroos: no HDMI mirror to save\n");
        }
    }
    free(copy);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *backend = ZOS_WITH_SDL ? "sdl" : "fbdev";
    const char *fbdev = "/dev/fb0", *kbd = NULL, *script = NULL;
    int rotate = 0, zoom = 3, radio_ctl = -1;

    const char *home = env_or("HOME", "/root");
    static char books_default[512], state_default[512];
    snprintf(books_default, sizeof books_default, "%s/Books", home);
    snprintf(state_default, sizeof state_default, "%s/.local/state/zeroos", home);
    zos_cfg.books_dir = env_or("ZOS_BOOKS", books_default);
    zos_cfg.state_dir = env_or("ZOS_STATE", state_default);
    zos_cfg.font_dir = env_or("ZOS_FONTS", "/usr/share/fonts/truetype/dejavu");
    static char music_default[512], video_default[512], pics_default[512];
    snprintf(music_default, sizeof music_default, "%s/Music", home);
    snprintf(video_default, sizeof video_default, "%s/Videos", home);
    snprintf(pics_default, sizeof pics_default, "%s/Pictures", home);
    zos_cfg.music_dir = env_or("ZOS_MUSIC", music_default);
    zos_cfg.video_dir = env_or("ZOS_VIDEOS", video_default);
    zos_cfg.pictures_dir = env_or("ZOS_PICTURES", pics_default);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--backend") && v) { backend = v; i++; }
        else if (!strcmp(a, "--fb") && v) { fbdev = v; i++; }
        else if (!strcmp(a, "--kbd") && v) { kbd = v; i++; }
        else if (!strcmp(a, "--rotate") && v) { rotate = atoi(v); i++; }
        else if (!strcmp(a, "--zoom") && v) { zoom = atoi(v); i++; }
        else if (!strcmp(a, "--books") && v) { zos_cfg.books_dir = v; i++; }
        else if (!strcmp(a, "--state") && v) { zos_cfg.state_dir = v; i++; }
        else if (!strcmp(a, "--fonts") && v) { zos_cfg.font_dir = v; i++; }
        else if (!strcmp(a, "--script") && v) { script = v; i++; backend = "headless"; }
        else if (!strcmp(a, "--demo-status")) { zos_fake_status = true; }
        else if (!strcmp(a, "--radio-control")) { radio_ctl = 1; }
        else if (!strcmp(a, "--no-radio-control")) { radio_ctl = 0; }
        else { fprintf(stderr, "unknown option %s (see top of src/main.c)\n", a); return 2; }
    }
    zos_mkdirs(zos_cfg.state_dir);

    lv_init();
    lv_display_t *disp = NULL;

    if (!strcmp(backend, "headless")) {
        disp = lv_display_create(ZOS_HOR_RES, ZOS_VER_RES);
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(disp, headless_fb, NULL, sizeof headless_fb,
                               LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_flush_cb(disp, headless_flush);
        zos_input_create();
    }
#if ZOS_WITH_SDL
    else if (!strcmp(backend, "sdl")) {
        lv_tick_set_cb(now_ms);
        disp = lv_sdl_window_create(ZOS_HOR_RES, ZOS_VER_RES);
        lv_sdl_window_set_zoom(disp, zoom);
        lv_sdl_window_set_title(disp, "ZeroOS simulator (320x170)");
        zos_input_create();
        zos_input_sdl_attach();
    }
#endif
#if ZOS_WITH_FBDEV
    else if (!strcmp(backend, "fbdev")) {
        lv_tick_set_cb(now_ms);
        if (access(fbdev, R_OK | W_OK) != 0) {
            perror(fbdev);
            fprintf(stderr, "zeroos: is the display driver loaded? (see README, 'Display')\n");
            return 1;
        }
        disp = lv_linux_fbdev_create();
        lv_linux_fbdev_set_file(disp, fbdev);
        if (rotate) {
            lv_display_rotation_t r = rotate == 90 ? LV_DISPLAY_ROTATION_90
                                    : rotate == 180 ? LV_DISPLAY_ROTATION_180
                                    : LV_DISPLAY_ROTATION_270;
            lv_display_set_rotation(disp, r);
        }
        zos_input_create();
        zos_evdev_keyboard_start(kbd);
        on_device = true;
    }
#endif
    else {
        fprintf(stderr, "backend '%s' is not available in this build\n", backend);
        return 2;
    }
    LV_UNUSED(zoom); LV_UNUSED(rotate); LV_UNUSED(fbdev); LV_UNUSED(kbd);

    zos_hdmi_init(disp);
    if (on_device) zos_settings_apply_saved();
    zos_http_global_init();
    zos_task_init();
    zos_theme_init();
    zos_launcher_create();
    if (radio_ctl < 0) radio_ctl = !strcmp(backend, "fbdev");
    mesh_service_init(radio_ctl);
    ts_service_init();

    /* An app asked to be opened after a restart (USB drive mode does). */
    const char *start_app = zos_setting_str("start_app", "");
    if (start_app[0] && !script) {
        for (int i = 0; i < zos_app_count; i++)
            if (!strcmp(zos_apps[i]->id, start_app)) zos_app_switch(zos_apps[i]);
        zos_setting_set_str("start_app", "");
    }
    if (script) {
        run_script(script);
        zos_player_stop();
        return 0;
    }
    for (;;) {
        uint32_t wait = lv_timer_handler();
        if (wait > 33) wait = 33;
        if (wait < 2) wait = 2;
        usleep(wait * 1000);
    }
}
