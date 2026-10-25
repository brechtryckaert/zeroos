/* Bitmap fonts for the terminal (generated: src/assets/termfonts.c). */
#ifndef ZOS_TERMFONT_H
#define ZOS_TERMFONT_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint8_t w, h;            /* cell size; w <= 8 */
    uint16_t count;
    const uint16_t *cp;      /* sorted code points */
    const uint8_t *rows;     /* count * h bytes; bit (w-1) = leftmost pixel */
} term_font_t;

extern const term_font_t term_font_6x12, term_font_6x10, term_font_5x8;

/* Rows for code point `c`, or NULL if the font lacks it. */
static inline const uint8_t *term_font_glyph(const term_font_t *f, uint32_t c)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->cp[mid] == c) return f->rows + (uint32_t)mid * f->h;
        if (f->cp[mid] < c) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

#endif
