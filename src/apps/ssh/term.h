/* A VT/xterm terminal on an LVGL canvas: libvterm parses, we draw cells with
 * the misc-fixed bitmap fonts (termfont.h). Bytes from the program go in
 * with term_feed(); keystrokes come out through the `out` callback. */
#ifndef ZOS_TERM_H
#define ZOS_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "termfont.h"

typedef struct term term_t;
typedef void (*term_out_cb)(const char *bytes, size_t len, void *ud);

term_t *term_create(lv_obj_t *parent, const term_font_t *font, term_out_cb out, void *ud);
void term_destroy(term_t *t);
lv_obj_t *term_obj(term_t *t);
void term_size(const term_t *t, int *rows, int *cols);
/* Change font; the grid size changes, so resize the pty afterwards. */
void term_set_font(term_t *t, const term_font_t *font);
void term_feed(term_t *t, const char *data, size_t len);
/* Translate a ZeroOS key into terminal input. */
void term_key(term_t *t, uint32_t key);
/* Scrollback: lines > 0 looks back, 0 returns to the live screen. */
void term_scroll(term_t *t, int delta_lines);
int term_scrollback_offset(const term_t *t);
/* Write a line of local text (status messages) as if the program printed it. */
void term_local(term_t *t, const char *text);
const char *term_title(const term_t *t);

#endif
