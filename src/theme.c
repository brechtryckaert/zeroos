/* Fonts and shared styling. UI text uses DejaVu Sans through Tiny TTF so
 * accented titles (é, ë, ø ...) render; LVGL's Montserrat stays as the
 * fallback for the built-in LV_SYMBOL_* glyphs. */
#include <stdio.h>
#include <unistd.h>
#include "zos.h"

zos_fonts_t zos_font;

static const lv_font_t *load_ttf(const char *file, int size, const lv_font_t *fallback)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", zos_cfg.font_dir, file);
    if (access(path, R_OK) != 0) {
        LV_LOG_WARN("font %s not found, using built-in font", path);
        return fallback;
    }
    char lvpath[520];
    snprintf(lvpath, sizeof lvpath, "A:%s", path);
    lv_font_t *f = lv_tiny_ttf_create_file_ex(lvpath, size, LV_FONT_KERNING_NORMAL, 256);
    if (!f) return fallback;
    f->fallback = fallback;   /* LV_SYMBOL_* glyphs come from Montserrat */
    return f;
}

void zos_theme_init(void)
{
    zos_font.small = load_ttf("DejaVuSans.ttf", 11, &lv_font_montserrat_12);
    zos_font.body  = load_ttf("DejaVuSans.ttf", 13, &lv_font_montserrat_14);
    zos_font.title = load_ttf("DejaVuSans-Bold.ttf", 16, &lv_font_montserrat_16);
    zos_font.large = load_ttf("DejaVuSans-Bold.ttf", 19, &lv_font_montserrat_20);

    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(ZOS_COLOR_ACCENT),
                                           lv_color_hex(0x3b82f6), true, zos_font.body);
    lv_display_set_theme(disp, th);
}

void zos_theme_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(ZOS_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(ZOS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(scr, zos_font.body, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

void zos_label_one_line(lv_obj_t *label, int32_t width)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, width);
    /* DOTS only truncates when the height is fixed; otherwise it wraps. */
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
}
