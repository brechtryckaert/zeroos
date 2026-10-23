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
} zos_app_t;

/* Registered apps, in launcher order. */
extern const zos_app_t *const zos_apps[];
extern const int zos_app_count;

void zos_app_launch(const zos_app_t *app);
/* Return to the launcher. Safe to call from inside an event handler. */
void zos_app_exit(void);
const zos_app_t *zos_app_current(void);

/* ---------- launcher / shell ---------- */

void zos_launcher_create(void);
void zos_launcher_show(void);

/* A status bar (clock, Wi-Fi, battery) attached to `parent`; returns it. */
lv_obj_t *zos_statusbar_create(lv_obj_t *parent, const char *title);

/* ---------- input ---------- */

/* The single keypad input device; set by the backend. */
extern lv_indev_t *zos_keypad;
void zos_input_set_group(lv_group_t *g);

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

/* Join a and b into a newly allocated path ("a/b"). Free with lv_free. */
char *zos_path_join(const char *a, const char *b);
/* mkdir -p */
void zos_mkdirs(const char *path);

#endif /* ZOS_H */
