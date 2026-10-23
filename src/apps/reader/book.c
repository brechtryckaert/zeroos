#include <mupdf/fitz.h>
#include <stdio.h>
#include <string.h>
#include "book.h"

/* Margins and spacing tuned for a 320x156 page. Book CSS still applies;
 * this only sets the page box and keeps huge author margins in check. */
static const char *USER_CSS =
    "@page { margin: 3px 9px 1px 9px; }"
    "body { margin: 0; padding: 0; }"
    "img, svg { max-width: 100%; }";

static fz_context *ctx;

struct book {
    fz_document *doc;
    bool reflow;
    int w, h;
    fz_location cur;
    int nch;
    int *ch_pages;       /* page count per chapter, -1 = not counted yet */
    int counted;         /* chapters counted so far (in order) */
    uint64_t anchor;     /* position to restore on re-layout ...      */
    bool anchored;       /* ... valid until the reader turns a page   */
    book_toc_t *toc;
    int ntoc;
};

static void quiet_warning(void *user, const char *msg)
{
    LV_UNUSED(user);
    LV_LOG_INFO("mupdf: %s", msg);
}

bool book_lib_init(void)
{
    if (ctx) return true;
    ctx = fz_new_context(NULL, NULL, 32 << 20);   /* 32 MB resource store: fine in 512 MB */
    if (!ctx) return false;
    fz_set_warning_callback(ctx, quiet_warning, NULL);
    fz_try(ctx) fz_register_document_handlers(ctx);
    fz_catch(ctx) {
        fz_drop_context(ctx);
        ctx = NULL;
        return false;
    }
    fz_set_user_css(ctx, USER_CSS);
    fz_set_aa_level(ctx, 8);
    return true;
}

void book_lib_done(void)
{
    if (ctx) fz_drop_context(ctx);
    ctx = NULL;
}

static void meta(fz_document *doc, const char *key, char *buf, size_t len)
{
    buf[0] = 0;
    if (fz_lookup_metadata(ctx, doc, key, buf, (int)len) < 0) buf[0] = 0;
}

bool book_peek(const char *path, char *title, size_t tlen, char *author, size_t alen)
{
    fz_document *volatile doc = NULL;
    volatile bool ok = true;
    fz_try(ctx) {
        doc = fz_open_document(ctx, path);
        meta(doc, FZ_META_INFO_TITLE, title, tlen);
        meta(doc, FZ_META_INFO_AUTHOR, author, alen);
    }
    fz_catch(ctx) ok = false;
    fz_drop_document(ctx, doc);
    return ok;
}

static void reset_counts(book_t *b)
{
    fz_free(ctx, b->ch_pages);
    b->ch_pages = NULL;
    b->nch = 0;
    b->counted = 0;
    fz_try(ctx) b->nch = fz_count_chapters(ctx, b->doc);
    fz_catch(ctx) b->nch = 0;
    if (b->nch > 0) {
        b->ch_pages = fz_malloc(ctx, sizeof(int) * b->nch);
        for (int i = 0; i < b->nch; i++) b->ch_pages[i] = -1;
    }
}

static void load_toc(book_t *b);

book_t *book_open(const char *path, char *err, size_t errlen)
{
    book_t *b = fz_calloc(ctx, 1, sizeof *b);
    fz_try(ctx) {
        b->doc = fz_open_document(ctx, path);
        b->reflow = fz_is_document_reflowable(ctx, b->doc);
    }
    fz_catch(ctx) {
        snprintf(err, errlen, "%s", fz_caught_message(ctx));
        fz_free(ctx, b);
        return NULL;
    }
    b->cur = fz_make_location(0, 0);
    return b;
}

static void free_toc(book_t *b)
{
    for (int i = 0; i < b->ntoc; i++) fz_free(ctx, b->toc[i].title);
    fz_free(ctx, b->toc);
    b->toc = NULL;
    b->ntoc = 0;
}

void book_close(book_t *b)
{
    if (!b) return;
    free_toc(b);
    fz_free(ctx, b->ch_pages);
    fz_drop_document(ctx, b->doc);
    fz_free(ctx, b);
}

bool book_reflowable(const book_t *b) { return b->reflow; }

static int chapter_pages(book_t *b, int ch);

/* Positions are "chapter + fraction through the chapter", packed into 64
 * bits. MuPDF's own fz_bookmark is a heap pointer for EPUB in current
 * builds, so it can't be saved to disk; this survives restarts and
 * re-layout, landing on the page that contains the same point. */
#define FRAC_ONE 1000000u

static uint64_t pos_of(book_t *b, fz_location l)
{
    if (l.chapter < 0) return 0;
    int n = chapter_pages(b, l.chapter);
    uint64_t frac = n > 0 ? (uint64_t)l.page * FRAC_ONE / (uint64_t)n : 0;
    return ((uint64_t)l.chapter << 32) | frac;
}

static fz_location loc_of(book_t *b, uint64_t pos)
{
    int ch = (int)(pos >> 32);
    uint32_t frac = (uint32_t)(pos & 0xffffffffu);
    if (b->nch <= 0) return fz_make_location(0, 0);
    if (ch >= b->nch) ch = b->nch - 1;
    int n = chapter_pages(b, ch);
    int page = (int)((uint64_t)frac * (uint64_t)n / FRAC_ONE);
    if (page >= n) page = n - 1;
    if (page < 0) page = 0;
    return fz_make_location(ch, page);
}

void book_layout(book_t *b, int w, int h, float em)
{
    bool had_layout = b->w > 0;
    /* Re-use the anchor so repeated size changes don't drift a page each time. */
    uint64_t pos = !had_layout ? 0 : b->anchored ? b->anchor : pos_of(b, b->cur);
    b->anchor = pos;
    b->anchored = had_layout;
    b->w = w;
    b->h = h;
    if (b->reflow) {
        fz_try(ctx) fz_layout_document(ctx, b->doc, (float)w, (float)h, em);
        fz_catch(ctx) LV_LOG_WARN("layout failed: %s", fz_caught_message(ctx));
    }
    reset_counts(b);
    if (had_layout) b->cur = loc_of(b, pos);
    load_toc(b);   /* TOC targets move when the layout changes */
}

book_loc_t book_cur(const book_t *b) { return (book_loc_t){b->cur.chapter, b->cur.page}; }

bool book_loc_eq(book_loc_t a, book_loc_t c) { return a.chapter == c.chapter && a.page == c.page; }

book_loc_t book_after(book_t *b, book_loc_t l)
{
    fz_location in = fz_make_location(l.chapter, l.page);
    volatile fz_location n = in;
    fz_try(ctx) n = fz_next_page(ctx, b->doc, in);
    fz_catch(ctx) n = in;
    return (book_loc_t){n.chapter, n.page};
}

bool book_next(book_t *b)
{
    volatile fz_location n = b->cur;
    fz_try(ctx) n = fz_next_page(ctx, b->doc, b->cur);
    fz_catch(ctx) return false;
    if (n.chapter == b->cur.chapter && n.page == b->cur.page) return false;
    b->cur = n;
    b->anchored = false;
    return true;
}

bool book_prev(book_t *b)
{
    volatile fz_location p = b->cur;
    fz_try(ctx) p = fz_previous_page(ctx, b->doc, b->cur);
    fz_catch(ctx) return false;
    if (p.chapter == b->cur.chapter && p.page == b->cur.page) return false;
    b->cur = p;
    b->anchored = false;
    return true;
}

void book_goto(book_t *b, book_loc_t l)
{
    if (l.chapter >= 0) b->cur = fz_make_location(l.chapter, l.page < 0 ? 0 : l.page);
    b->anchored = false;
}

bool book_render(book_t *b, book_loc_t l, lv_draw_buf_t *dst, const uint16_t lut[256])
{
    const int W = dst->header.w, H = dst->header.h;
    const uint32_t stride = dst->header.stride;
    fz_page *volatile page = NULL;
    fz_pixmap *volatile pix = NULL;
    volatile bool ok = true;

    /* paper colour first, so margins of fixed-layout pages match the theme */
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(dst->data + y * stride);
        for (int x = 0; x < W; x++) row[x] = lut[255];
    }

    fz_try(ctx) {
        page = fz_load_chapter_page(ctx, b->doc, l.chapter, l.page);
        fz_rect r = fz_bound_page(ctx, page);
        float pw = r.x1 - r.x0, ph = r.y1 - r.y0;
        float s = 1.0f;
        if (!b->reflow || pw > W + 1 || ph > H + 1) {   /* fit fixed pages (PDF, CBZ) */
            float sx = W / pw, sy = H / ph;
            s = sx < sy ? sx : sy;
        }
        pix = fz_new_pixmap_from_page(ctx, page, fz_scale(s, s), fz_device_gray(ctx), 0);
        int ox = (W - pix->w) / 2, oy = (H - pix->h) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        for (int y = 0; y < pix->h && y + oy < H; y++) {
            const unsigned char *src = pix->samples + (size_t)y * pix->stride;
            uint16_t *row = (uint16_t *)(dst->data + (y + oy) * stride) + ox;
            for (int x = 0; x < pix->w && x + ox < W; x++) row[x] = lut[src[x]];
        }
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        LV_LOG_WARN("render %d/%d failed: %s", l.chapter, l.page, fz_caught_message(ctx));
        ok = false;
    }
    return ok;
}

uint64_t book_bookmark(book_t *b) { return b->anchored ? b->anchor : pos_of(b, b->cur); }

void book_goto_bookmark(book_t *b, uint64_t mark)
{
    b->cur = loc_of(b, mark);
    b->anchor = mark;
    b->anchored = true;
}

/* ---------- table of contents ---------- */

static void walk(book_t *b, fz_outline *first, int depth, int *cap)
{
    for (fz_outline *volatile o = first; o; o = o->next) {
        if (o->title) {
            if (b->ntoc == *cap) {
                *cap = *cap ? *cap * 2 : 32;
                b->toc = fz_realloc(ctx, b->toc, sizeof(book_toc_t) * *cap);
            }
            volatile fz_location loc = o->page;
            if (loc.chapter < 0 && o->uri) {
                float x, y;
                fz_try(ctx) loc = fz_resolve_link(ctx, b->doc, o->uri, &x, &y);
                fz_catch(ctx) loc = fz_make_location(-1, -1);
            }
            book_toc_t *t = &b->toc[b->ntoc++];
            t->title = fz_strdup(ctx, o->title);
            t->depth = depth;
            t->loc = (book_loc_t){loc.chapter, loc.page};
        }
        if (o->down) walk(b, o->down, depth + 1, cap);
    }
}

static void load_toc(book_t *b)
{
    free_toc(b);
    fz_outline *volatile root = NULL;
    int cap = 0;
    fz_try(ctx) {
        root = fz_load_outline(ctx, b->doc);
        walk(b, root, 0, &cap);
    }
    fz_always(ctx) fz_drop_outline(ctx, root);
    fz_catch(ctx) LV_LOG_INFO("no outline: %s", fz_caught_message(ctx));
}

int book_toc(book_t *b, const book_toc_t **out)
{
    *out = b->toc;
    return b->ntoc;
}

static bool loc_le(book_loc_t a, book_loc_t c)
{
    return a.chapter < c.chapter || (a.chapter == c.chapter && a.page <= c.page);
}

const char *book_section_title(book_t *b)
{
    const char *t = NULL;
    book_loc_t cur = book_cur(b);
    for (int i = 0; i < b->ntoc; i++)
        if (b->toc[i].loc.chapter >= 0 && loc_le(b->toc[i].loc, cur)) t = b->toc[i].title;
    return t;
}

/* ---------- progress ---------- */

static int chapter_pages(book_t *b, int ch)
{
    if (ch < 0 || ch >= b->nch) return 1;
    if (b->ch_pages[ch] < 0) {
        fz_try(ctx) b->ch_pages[ch] = fz_count_chapter_pages(ctx, b->doc, ch);
        fz_catch(ctx) b->ch_pages[ch] = 1;
    }
    return b->ch_pages[ch] > 0 ? b->ch_pages[ch] : 1;
}

bool book_count_step(book_t *b)
{
    if (b->counted >= b->nch) return true;
    chapter_pages(b, b->counted++);
    return b->counted >= b->nch;
}

float book_progress(book_t *b, int *page, int *total)
{
    *page = *total = 0;
    if (b->nch <= 0) return 0;
    int ch = b->cur.chapter;
    if (b->counted >= b->nch) {
        int before = 0, all = 0;
        for (int i = 0; i < b->nch; i++) {
            if (i < ch) before += b->ch_pages[i];
            all += b->ch_pages[i];
        }
        *page = before + b->cur.page + 1;
        *total = all;
        return all > 1 ? (float)(*page - 1) / (float)(all - 1) : 1.0f;
    }
    /* estimate while counting: chapters weighted equally */
    float in_ch = (float)b->cur.page / (float)chapter_pages(b, ch);
    return ((float)ch + in_ch) / (float)b->nch;
}
