#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>
#include "../../zos.h"
#include "term.h"

#define SB_LINES 600
#define BLINK_MS 530

typedef struct {
    uint32_t ch;
    uint16_t fg, bg;
    uint8_t bold : 1, underline : 1, reverse : 1;
} cell_t;

struct term {
    lv_obj_t *canvas;
    lv_draw_buf_t *buf;
    const term_font_t *font;
    int rows, cols, ox, oy;
    VTerm *vt;
    VTermScreen *screen;
    term_out_cb out;
    void *ud;
    uint8_t *dirty;              /* one flag per row */
    bool all_dirty;
    VTermPos cursor;
    bool cursor_visible, blink_on;
    lv_timer_t *timer;
    uint32_t blink_at;
    uint16_t def_fg, def_bg;
    /* scrollback ring */
    cell_t *sb;
    int sb_cols, sb_count, sb_head;   /* head = index of the oldest line */
    int sb_view;                      /* lines scrolled back */
    char title[96];
};

/* A calm palette that reads well on a small LCD (xterm order). */
static const uint32_t ANSI[16] = {
    0x1b1f26, 0xe5534b, 0x57ab5a, 0xc69026, 0x539bf5, 0xb083f0, 0x39c5cf, 0xadbac7,
    0x636e7b, 0xff7b72, 0x7ee787, 0xf2cc60, 0x79c0ff, 0xd2a8ff, 0x56d4dd, 0xf0f6fc,
};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t hex565(uint32_t c) { return rgb565((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c); }

static uint16_t brighten(uint16_t c)
{
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r = r + (31 - r) / 3;
    g = g + (63 - g) / 3;
    b = b + (31 - b) / 3;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* ---------------- cells ---------------- */

static cell_t cell_from_vterm(term_t *t, const VTermScreenCell *vc)
{
    cell_t c = {0};
    c.ch = vc->chars[0];
    VTermColor fg = vc->fg, bg = vc->bg;
    c.fg = VTERM_COLOR_IS_DEFAULT_FG(&fg) ? t->def_fg : 0;
    c.bg = VTERM_COLOR_IS_DEFAULT_BG(&bg) ? t->def_bg : 0;
    if (!VTERM_COLOR_IS_DEFAULT_FG(&fg)) {
        vterm_screen_convert_color_to_rgb(t->screen, &fg);
        c.fg = rgb565(fg.rgb.red, fg.rgb.green, fg.rgb.blue);
    }
    if (!VTERM_COLOR_IS_DEFAULT_BG(&bg)) {
        vterm_screen_convert_color_to_rgb(t->screen, &bg);
        c.bg = rgb565(bg.rgb.red, bg.rgb.green, bg.rgb.blue);
    }
    c.bold = vc->attrs.bold;
    c.underline = vc->attrs.underline != 0;
    c.reverse = vc->attrs.reverse;
    return c;
}

static cell_t screen_cell(term_t *t, int row, int col)
{
    VTermScreenCell vc;
    VTermPos p = {row, col};
    if (!vterm_screen_get_cell(t->screen, p, &vc)) return (cell_t){.fg = t->def_fg, .bg = t->def_bg};
    return cell_from_vterm(t, &vc);
}

static void draw_cell(term_t *t, int row, int col, cell_t c, bool cursor)
{
    const term_font_t *f = t->font;
    uint16_t fg = c.bold ? brighten(c.fg) : c.fg, bg = c.bg;
    if (c.reverse != cursor) {
        uint16_t x = fg;
        fg = bg;
        bg = x;
    }
    const uint8_t *g = (c.ch > ' ') ? term_font_glyph(f, c.ch) : NULL;
    if (!g && c.ch > ' ') g = term_font_glyph(f, '?');
    uint32_t stride = t->buf->header.stride / 2;
    uint16_t *px = (uint16_t *)t->buf->data;
    int x0 = t->ox + col * f->w, y0 = t->oy + row * f->h;
    for (int y = 0; y < f->h; y++) {
        uint16_t *line = px + (uint32_t)(y0 + y) * stride + x0;
        uint8_t bits = g ? g[y] : 0;
        if (c.underline && y == f->h - 1) bits = 0xFF;
        for (int x = 0; x < f->w; x++) line[x] = (bits & (0x80 >> (x + 8 - f->w))) ? fg : bg;
    }
}

static cell_t sb_cell(term_t *t, int line, int col)
{
    /* line 0 = oldest kept line */
    int idx = (t->sb_head + line) % SB_LINES;
    if (col >= t->sb_cols) return (cell_t){.fg = t->def_fg, .bg = t->def_bg};
    return t->sb[(size_t)idx * t->sb_cols + col];
}

static void redraw_row(term_t *t, int row)
{
    int virt = t->sb_count - t->sb_view + row;   /* index into scrollback ++ screen */
    for (int col = 0; col < t->cols; col++) {
        cell_t c;
        if (t->sb_view && virt < t->sb_count) c = sb_cell(t, virt, col);
        else c = screen_cell(t, t->sb_view ? virt - t->sb_count : row, col);
        bool cur = !t->sb_view && t->cursor_visible && t->blink_on && t->cursor.row == row && t->cursor.col == col;
        draw_cell(t, row, col, c, cur);
    }
}

static void flush(term_t *t)
{
    int top = -1, bot = -1;
    for (int r = 0; r < t->rows; r++)
        if (t->all_dirty || t->dirty[r]) {
            redraw_row(t, r);
            t->dirty[r] = 0;
            if (top < 0) top = r;
            bot = r;
        }
    t->all_dirty = false;
    if (top >= 0) {
        lv_area_t a = {0, t->oy + top * t->font->h, ZOS_HOR_RES - 1, t->oy + (bot + 1) * t->font->h - 1};
        lv_obj_invalidate_area(t->canvas, &a);
    }
}

/* ---------------- libvterm callbacks ---------------- */

static int cb_damage(VTermRect r, void *ud)
{
    term_t *t = ud;
    for (int row = r.start_row; row < r.end_row && row < t->rows; row++) t->dirty[row] = 1;
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *ud)
{
    term_t *t = ud;
    if (old.row >= 0 && old.row < t->rows) t->dirty[old.row] = 1;
    if (pos.row >= 0 && pos.row < t->rows) t->dirty[pos.row] = 1;
    t->cursor = pos;
    t->cursor_visible = visible;
    t->blink_on = true;
    t->blink_at = lv_tick_get() + BLINK_MS;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *ud)
{
    term_t *t = ud;
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        t->cursor_visible = val->boolean;
        t->dirty[t->cursor.row < t->rows ? t->cursor.row : 0] = 1;
    } else if (prop == VTERM_PROP_TITLE && val->string.str) {
        size_t n = val->string.len < sizeof t->title - 1 ? val->string.len : sizeof t->title - 1;
        if (val->string.initial) t->title[0] = 0;
        size_t have = strlen(t->title);
        if (have + n > sizeof t->title - 1) n = sizeof t->title - 1 - have;
        memcpy(t->title + have, val->string.str, n);
        t->title[have + n] = 0;
    }
    return 1;
}

static int cb_pushline(int cols, const VTermScreenCell *cells, void *ud)
{
    term_t *t = ud;
    if (!t->sb) return 0;
    int slot;
    if (t->sb_count < SB_LINES) slot = (t->sb_head + t->sb_count++) % SB_LINES;
    else {
        slot = t->sb_head;
        t->sb_head = (t->sb_head + 1) % SB_LINES;
    }
    cell_t *dst = t->sb + (size_t)slot * t->sb_cols;
    for (int c = 0; c < t->sb_cols; c++)
        dst[c] = c < cols ? cell_from_vterm(t, &cells[c]) : (cell_t){.fg = t->def_fg, .bg = t->def_bg};
    if (t->sb_view) t->sb_view = t->sb_view < t->sb_count ? t->sb_view + 1 : t->sb_count;   /* keep the view still */
    return 1;
}

static int cb_bell(void *ud)
{
    LV_UNUSED(ud);
    return 1;
}

static const VTermScreenCallbacks CBS = {
    .damage = cb_damage,
    .movecursor = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell = cb_bell,
    .sb_pushline = cb_pushline,
};

static void out_cb(const char *s, size_t len, void *ud)
{
    term_t *t = ud;
    if (t->out) t->out(s, len, t->ud);
}

static void tick(lv_timer_t *timer)
{
    term_t *t = lv_timer_get_user_data(timer);
    if ((int32_t)(lv_tick_get() - t->blink_at) >= 0) {
        t->blink_on = !t->blink_on;
        t->blink_at = lv_tick_get() + BLINK_MS;
        if (t->cursor.row >= 0 && t->cursor.row < t->rows) t->dirty[t->cursor.row] = 1;
    }
    flush(t);
}

/* ---------------- setup ---------------- */

static void layout(term_t *t)
{
    t->cols = ZOS_HOR_RES / t->font->w;
    t->rows = ZOS_VER_RES / t->font->h;
    t->ox = (ZOS_HOR_RES - t->cols * t->font->w) / 2;
    t->oy = (ZOS_VER_RES - t->rows * t->font->h) / 2;
    lv_free(t->dirty);
    t->dirty = lv_zalloc((size_t)t->rows);
    /* clear the margins */
    uint16_t *px = (uint16_t *)t->buf->data;
    uint32_t stride = t->buf->header.stride / 2;
    for (int y = 0; y < ZOS_VER_RES; y++)
        for (int x = 0; x < ZOS_HOR_RES; x++) px[(uint32_t)y * stride + x] = t->def_bg;
    t->all_dirty = true;
}

static void alloc_sb(term_t *t)
{
    lv_free(t->sb);
    t->sb_cols = t->cols;
    t->sb = lv_zalloc(sizeof(cell_t) * (size_t)SB_LINES * (size_t)t->sb_cols);
    t->sb_count = t->sb_head = t->sb_view = 0;
}

term_t *term_create(lv_obj_t *parent, const term_font_t *font, term_out_cb out, void *ud)
{
    term_t *t = lv_zalloc(sizeof *t);
    t->font = font;
    t->out = out;
    t->ud = ud;
    t->def_fg = hex565(0xd6dbe1);
    t->def_bg = hex565(0x0d1014);
    t->buf = lv_draw_buf_create(ZOS_HOR_RES, ZOS_VER_RES, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    t->canvas = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(t->canvas, t->buf);
    lv_obj_set_pos(t->canvas, 0, 0);
    layout(t);
    alloc_sb(t);

    t->vt = vterm_new(t->rows, t->cols);
    vterm_set_utf8(t->vt, 1);
    t->screen = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->screen, &CBS, t);
    vterm_screen_enable_altscreen(t->screen, 1);
    vterm_output_set_callback(t->vt, out_cb, t);
    VTermState *st = vterm_obtain_state(t->vt);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, 0xd6, 0xdb, 0xe1);
    vterm_color_rgb(&bg, 0x0d, 0x10, 0x14);
    vterm_state_set_default_colors(st, &fg, &bg);
    for (int i = 0; i < 16; i++) {
        VTermColor c;
        vterm_color_rgb(&c, (uint8_t)(ANSI[i] >> 16), (uint8_t)(ANSI[i] >> 8), (uint8_t)ANSI[i]);
        vterm_state_set_palette_color(st, i, &c);
    }
    vterm_screen_reset(t->screen, 1);
    t->cursor_visible = true;
    t->timer = lv_timer_create(tick, 16, t);
    return t;
}

void term_destroy(term_t *t)
{
    if (!t) return;
    lv_timer_delete(t->timer);
    vterm_free(t->vt);
    lv_obj_delete(t->canvas);
    lv_draw_buf_destroy(t->buf);
    lv_free(t->dirty);
    lv_free(t->sb);
    lv_free(t);
}

lv_obj_t *term_obj(term_t *t) { return t->canvas; }

void term_size(const term_t *t, int *rows, int *cols)
{
    *rows = t->rows;
    *cols = t->cols;
}

void term_set_font(term_t *t, const term_font_t *font)
{
    t->font = font;
    layout(t);
    alloc_sb(t);   /* scrollback cells are per column; start fresh at the new width */
    vterm_set_size(t->vt, t->rows, t->cols);
    vterm_screen_flush_damage(t->screen);
    t->all_dirty = true;
    flush(t);
}

void term_feed(term_t *t, const char *data, size_t len)
{
    vterm_input_write(t->vt, data, len);
    vterm_screen_flush_damage(t->screen);
}

void term_local(term_t *t, const char *text)
{
    /* \r\n framing + dim colour, so it reads as a local notice */
    term_feed(t, "\r\n\x1b[2;37m", 9);
    term_feed(t, text, strlen(text));
    term_feed(t, "\x1b[0m\r\n", 6);
}

void term_scroll(term_t *t, int delta)
{
    int v = t->sb_view + delta;
    if (v < 0) v = 0;
    if (v > t->sb_count) v = t->sb_count;
    if (v != t->sb_view) {
        t->sb_view = v;
        t->all_dirty = true;
    }
}

int term_scrollback_offset(const term_t *t) { return t->sb_view; }
const char *term_title(const term_t *t) { return t->title; }

void term_key(term_t *t, uint32_t k)
{
    if (t->sb_view) {   /* typing returns to the live screen */
        t->sb_view = 0;
        t->all_dirty = true;
    }
    VTermModifier none = VTERM_MOD_NONE;
    if (ZOS_KEY_IS_CTRL(k)) {
        uint32_t c = k & 0x7F;
        if (c == ' ') {
            char nul = 0;
            out_cb(&nul, 1, t);
        } else {
            vterm_keyboard_unichar(t->vt, c, VTERM_MOD_CTRL);
        }
        return;
    }
    switch (k) {
    case LV_KEY_ENTER: vterm_keyboard_key(t->vt, VTERM_KEY_ENTER, none); return;
    case LV_KEY_BACKSPACE: vterm_keyboard_key(t->vt, VTERM_KEY_BACKSPACE, none); return;
    case LV_KEY_ESC: vterm_keyboard_key(t->vt, VTERM_KEY_ESCAPE, none); return;
    case LV_KEY_UP: vterm_keyboard_key(t->vt, VTERM_KEY_UP, none); return;
    case LV_KEY_DOWN: vterm_keyboard_key(t->vt, VTERM_KEY_DOWN, none); return;
    case LV_KEY_LEFT: vterm_keyboard_key(t->vt, VTERM_KEY_LEFT, none); return;
    case LV_KEY_RIGHT: vterm_keyboard_key(t->vt, VTERM_KEY_RIGHT, none); return;
    case LV_KEY_DEL: vterm_keyboard_key(t->vt, VTERM_KEY_DEL, none); return;
    case LV_KEY_HOME: vterm_keyboard_key(t->vt, VTERM_KEY_HOME, none); return;
    case LV_KEY_END: vterm_keyboard_key(t->vt, VTERM_KEY_END, none); return;
    case ZOS_KEY_TAB: vterm_keyboard_key(t->vt, VTERM_KEY_TAB, none); return;
    case ZOS_KEY_BACKTAB: vterm_keyboard_key(t->vt, VTERM_KEY_TAB, VTERM_MOD_SHIFT); return;
    case ZOS_KEY_PGUP: vterm_keyboard_key(t->vt, VTERM_KEY_PAGEUP, none); return;
    case ZOS_KEY_PGDN: vterm_keyboard_key(t->vt, VTERM_KEY_PAGEDOWN, none); return;
    }
    if (k >= 0x20 && k < ZOS_KEY_EXT_BASE && k != 0x7F) vterm_keyboard_unichar(t->vt, k, none);
}
