/*
 * ZeroOS -- a small icon-driven shell for the M5Stack CardputerZero.
 *
 * Shared definitions: screen geometry, the app interface, theme helpers and
 * runtime configuration. Everything runs in one LVGL process; each app owns a
 * screen and an input group while it is open.
 */
#ifndef ZOS_H
#define ZOS_H

#include <stdbool.h>
#include "lvgl.h"

#define ZOS_HOR_RES 320
#define ZOS_VER_RES 170
#define ZOS_STATUSBAR_H 18

/* ---------- runtime configuration (filled in by main.c) ---------- */

typedef struct {
    const char *books_dir;   /* where the reader looks for books */
    const char *state_dir;   /* where apps keep their state */
    const char *font_dir;    /* directory containing DejaVuSans*.ttf */
    const char *music_dir;   /* ~/Music */
    const char *video_dir;   /* ~/Videos */
    const char *pictures_dir;/* ~/Pictures (camera photos) */
} zos_config_t;

extern zos_config_t zos_cfg;

/* ---------- apps ---------- */

typedef struct zos_app {
    const char *id;
    const char *name;
    const char *tagline;           /* one line shown under the name in the launcher */
    const lv_image_dsc_t *icon;    /* A8 mask, recoloured white */
    uint32_t color;                /* tile colour, 0xRRGGBB */
    /* Build the UI on `scr`. Add focusable objects to `group`. */
    void (*open)(lv_obj_t *scr, lv_group_t *group);
    /* Free everything that is not an LVGL child of the screen. Optional. */
    void (*close)(void);
    /* Unread count shown on the launcher tile. Optional. */
    int (*badge)(void);
} zos_app_t;

/* Registered apps, in launcher order. */
extern const zos_app_t *const zos_apps[];
extern const int zos_app_count;

void zos_app_launch(const zos_app_t *app);
/* Close the current app (if any) and open `app`. Safe from event handlers. */
void zos_app_switch(const zos_app_t *app);
/* Return to the launcher. Safe to call from inside an event handler. */
void zos_app_exit(void);
const zos_app_t *zos_app_current(void);

/* ---------- launcher / shell ---------- */

void zos_launcher_create(void);
void zos_launcher_show(void);

/* Stop drawing to the panel while another program (mpv) owns it, then
 * redraw everything when it hands the panel back. */
void zos_display_suspend(bool suspend);
bool zos_on_device(void);   /* fbdev backend: real hardware */

/* Restore saved settings (brightness) at startup; in the Settings app. */
void zos_settings_apply_saved(void);

/* A short banner over whatever is on screen (new message etc.). */
void zos_notify(uint32_t color, const char *title, const char *text);

/* A status bar (clock, Wi-Fi, battery) attached to `parent`; returns it. */
lv_obj_t *zos_statusbar_create(lv_obj_t *parent, const char *title);

/* ---------- input ---------- */

/* The single keypad input device (src/input.c). Backends push keys into
 * its queue: LVGL key codes, Unicode code points, or these extensions. */
extern lv_indev_t *zos_keypad;
lv_indev_t *zos_input_create(void);
void zos_key_push(uint32_t key);
bool zos_key_queue_empty(void);
void zos_input_set_group(lv_group_t *g);
#if ZOS_WITH_SDL
void zos_input_sdl_attach(void);
#endif
/* Device keyboard: a timer reads evdev and pushes keys. */
void zos_evdev_keyboard_start(const char *path);

#define ZOS_KEY_EXT_BASE 0x110000u              /* above Unicode */
#define ZOS_KEY_CTRL(c) (ZOS_KEY_EXT_BASE | (uint32_t)(c))   /* c: 'a'..'z', '[', ']', '\\', ' ' */
#define ZOS_KEY_TAB (ZOS_KEY_EXT_BASE + 0x100)  /* LV_KEY_NEXT (9) is eaten by focus handling */
#define ZOS_KEY_BACKTAB (ZOS_KEY_EXT_BASE + 0x101)
#define ZOS_KEY_PGUP (ZOS_KEY_EXT_BASE + 0x102)
#define ZOS_KEY_PGDN (ZOS_KEY_EXT_BASE + 0x103)
#define ZOS_KEY_IS_CTRL(k) ((k) > ZOS_KEY_EXT_BASE && (k) < ZOS_KEY_EXT_BASE + 0x80)

/* ---------- theme ---------- */

typedef struct {
    const lv_font_t *small;   /* ~11px: hints, status bar */
    const lv_font_t *body;    /* ~13px: list rows */
    const lv_font_t *title;   /* ~17px: headings */
    const lv_font_t *large;   /* ~20px: launcher app name */
} zos_fonts_t;

extern zos_fonts_t zos_font;

#define ZOS_COLOR_BG      0x0f1216
#define ZOS_COLOR_SURFACE 0x1a1f26
#define ZOS_COLOR_TEXT    0xeef1f5
#define ZOS_COLOR_MUTED   0x8a94a3
#define ZOS_COLOR_ACCENT  0xf5a524

void zos_theme_init(void);
/* Style a freshly created app screen with the shell background. */
void zos_theme_screen(lv_obj_t *scr);

/* Simple "coming soon" app body, used by apps that are not built yet. */
void zos_placeholder_body(lv_obj_t *scr, const zos_app_t *app, const char *text);

/* ---------- small utils ---------- */

/* Make a label a single line of `width` px, cut off with "…". */
void zos_label_one_line(lv_obj_t *label, int32_t width);

/* Join a and b into a newly allocated path ("a/b"). Free with lv_free. */
char *zos_path_join(const char *a, const char *b);
/* mkdir -p */
void zos_mkdirs(const char *path);

#endif /* ZOS_H */
