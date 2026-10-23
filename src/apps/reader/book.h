/* Thin wrapper around MuPDF for the reader: open, reflow, page through,
 * render into an RGB565 buffer, bookmarks that survive re-layout, TOC. */
#ifndef ZOS_BOOK_H
#define ZOS_BOOK_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct book book_t;

typedef struct {
    int chapter, page;
} book_loc_t;

typedef struct {
    char *title;
    int depth;
    book_loc_t loc;
} book_toc_t;

/* One MuPDF context for the app; call once on app open / close. */
bool book_lib_init(void);
void book_lib_done(void);

/* Read title/author without keeping the document open. Returns false if the
 * file cannot be opened. Strings are written into the given buffers. */
bool book_peek(const char *path, char *title, size_t tlen, char *author, size_t alen);

book_t *book_open(const char *path, char *err, size_t errlen);
void book_close(book_t *b);

/* Reflow to w x h pixels at font size em (px). Keeps the reading position. */
void book_layout(book_t *b, int w, int h, float em);
bool book_reflowable(const book_t *b);

book_loc_t book_cur(const book_t *b);
bool book_next(book_t *b);
bool book_prev(book_t *b);
bool book_loc_eq(book_loc_t a, book_loc_t c);
/* Location after `l`; returns `l` itself at the end of the book. */
book_loc_t book_after(book_t *b, book_loc_t l);
void book_goto(book_t *b, book_loc_t l);

/* Render the page at `l` into an RGB565 draw buffer of the layout size,
 * mapping grey levels through lut[0..255] (0 = ink, 255 = paper). */
bool book_render(book_t *b, book_loc_t l, lv_draw_buf_t *dst, const uint16_t lut[256]);

/* Position (chapter + fraction through it) that is stable across
 * re-layout and across sessions; 0 = start of the book. */
uint64_t book_bookmark(book_t *b);
void book_goto_bookmark(book_t *b, uint64_t mark);

/* Table of contents (flattened, with depth). Owned by the book. */
int book_toc(book_t *b, const book_toc_t **out);
/* Title of the TOC entry the current page falls in, or NULL. */
const char *book_section_title(book_t *b);

/* Background page counting: call repeatedly while idle; each call lays out
 * one chapter. Returns true once every chapter has been counted. */
bool book_count_step(book_t *b);
/* Progress 0..1; *page and *total are 1-based page numbers when counting is
 * complete, otherwise 0. */
float book_progress(book_t *b, int *page, int *total);

#endif
