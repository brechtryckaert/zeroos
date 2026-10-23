/* Books: an ePub reader built on MuPDF.
 *
 * Three views on one screen:
 *   library  - list of books in the books dir, most recent first
 *   page     - full-screen page with a one-line footer
 *   contents - table of contents overlay
 *
 * Page turns are instant because the next page is rendered into a spare
 * buffer while you read; idle time is also used to lay out the remaining
 * chapters so the footer can show real page numbers. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../zos.h"
#include "book.h"
#include "shelf.h"

extern const lv_image_dsc_t zos_icon_book;

#define PAGE_W ZOS_HOR_RES
#define FOOT_H 14
#define PAGE_H (ZOS_VER_RES - FOOT_H)
#define NSLOTS 3

static const int EM_SIZES[] = {10, 11, 12, 13, 14, 16, 18, 20};
#define N_EM ((int)(sizeof EM_SIZES / sizeof EM_SIZES[0]))

static const struct { const char *name; uint32_t bg, fg; } THEMES[] = {
    {"Paper", 0xfbfaf7, 0x1c1c1c},
    {"Sepia", 0xf2e8d0, 0x3b2d1c},
    {"Night", 0x101113, 0xc4c4c4},
};
#define N_THEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

typedef enum { MODE_LIBRARY, MODE_PAGE, MODE_TOC } mode_t_;

static struct {
    lv_obj_t *scr;
    mode_t_ mode;

    /* library */
    lv_obj_t *lib_view, *lib_list, *lib_title;
    int lib_sel, lib_count;
    shelf_item_t **lib_items;

    /* page */
    lv_obj_t *page_view, *page_img, *foot_l, *foot_r, *toast;
    lv_timer_t *toast_timer, *idle_timer;
    book_t *book;
    shelf_item_t *item;
    uint16_t lut[256];
    uint32_t gen;            /* bumped when layout or theme changes */
    struct { book_loc_t loc; uint32_t gen; lv_draw_buf_t *buf; uint32_t used; } slot[NSLOTS];
    uint32_t use_clock;
    bool prefetch_pending, counting_done;
    int turns_since_save;

    /* contents */
    lv_obj_t *toc_view, *toc_list;
    int toc_sel, toc_count;
} R;

static void show_page(void);
static void library_refresh(void);

/* ======================= helpers ======================= */

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void hide(lv_obj_t *o, bool h)
{
    if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void toast_hide(lv_timer_t *t)
{
    LV_UNUSED(t);
    hide(R.toast, true);
    R.toast_timer = NULL;
}

static void toast(const char *fmt, const char *arg)
{
    char buf[96];
    snprintf(buf, sizeof buf, fmt, arg);
    lv_label_set_text(R.toast, buf);
    hide(R.toast, false);
    lv_obj_move_foreground(R.toast);
    if (R.toast_timer) lv_timer_delete(R.toast_timer);
    R.toast_timer = lv_timer_create(toast_hide, 1100, NULL);
    lv_timer_set_repeat_count(R.toast_timer, 1);
}

/* ======================= library ======================= */

static void lib_select(int i)
{
    if (R.lib_count == 0) return;
    if (i < 0) i = 0;
    if (i >= R.lib_count) i = R.lib_count - 1;
    lv_obj_t *old = lv_obj_get_child(R.lib_list, R.lib_sel);
    if (old) lv_obj_remove_state(old, LV_STATE_CHECKED);
    R.lib_sel = i;
    lv_obj_t *row = lv_obj_get_child(R.lib_list, i);
    lv_obj_add_state(row, LV_STATE_CHECKED);
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

static void library_refresh(void)
{
    shelf_scan(zos_cfg.books_dir);
    R.lib_count = shelf_list(&R.lib_items);
    lv_obj_clean(R.lib_list);
    lv_label_set_text_fmt(R.lib_title, "Library  \u00b7  %d", R.lib_count);

    static lv_style_t st_row, st_row_sel;
    static bool inited;
    if (!inited) {
        inited = true;
        lv_style_init(&st_row);
        lv_style_set_width(&st_row, lv_pct(100));
        lv_style_set_height(&st_row, 37);
        lv_style_set_pad_hor(&st_row, 10);
        lv_style_set_pad_ver(&st_row, 3);
        lv_style_set_radius(&st_row, 6);
        lv_style_init(&st_row_sel);
        lv_style_set_bg_opa(&st_row_sel, LV_OPA_COVER);
        lv_style_set_bg_color(&st_row_sel, lv_color_hex(ZOS_COLOR_SURFACE));
        lv_style_set_border_side(&st_row_sel, LV_BORDER_SIDE_LEFT);
        lv_style_set_border_width(&st_row_sel, 3);
        lv_style_set_border_color(&st_row_sel, lv_color_hex(ZOS_COLOR_ACCENT));
    }

    if (R.lib_count == 0) {
        lv_obj_t *msg = lv_label_create(R.lib_list);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(msg, lv_pct(100));
        lv_obj_set_style_pad_all(msg, 12, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(ZOS_COLOR_MUTED), 0);
        lv_label_set_text_fmt(msg, "No books yet.\nCopy .epub files to\n%s", zos_cfg.books_dir);
        return;
    }

    for (int i = 0; i < R.lib_count; i++) {
        shelf_item_t *it = R.lib_items[i];
        lv_obj_t *row = plain_obj(R.lib_list);
        lv_obj_add_style(row, &st_row, 0);
        lv_obj_add_style(row, &st_row_sel, LV_STATE_CHECKED);

        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, it->title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(t, 250);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *a = lv_label_create(row);
        lv_label_set_text(a, it->author[0] ? it->author : "Unknown author");
        lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(a, 250);
        lv_obj_set_style_text_font(a, zos_font.small, 0);
        lv_obj_set_style_text_color(a, lv_color_hex(ZOS_COLOR_MUTED), 0);
        lv_obj_align(a, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lv_obj_t *p = lv_label_create(row);
        if (it->last_opened) lv_label_set_text_fmt(p, "%d%%", it->percent);
        else lv_label_set_text(p, "new");
        lv_obj_set_style_text_font(p, zos_font.small, 0);
        lv_obj_set_style_text_color(p, lv_color_hex(it->last_opened ? ZOS_COLOR_MUTED
                                                                    : ZOS_COLOR_ACCENT), 0);
        lv_obj_align(p, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    R.lib_sel = 0;
    lv_obj_scroll_to_y(R.lib_list, 0, LV_ANIM_OFF);
    lib_select(0);
}

/* ======================= page view ======================= */

static void apply_theme(void)
{
    reader_prefs_t *p = reader_prefs();
    if (p->theme < 0 || p->theme >= N_THEMES) p->theme = 0;
    lv_color_t bg = lv_color_hex(THEMES[p->theme].bg), fg = lv_color_hex(THEMES[p->theme].fg);
    for (int i = 0; i < 256; i++) R.lut[i] = lv_color_to_u16(lv_color_mix(bg, fg, (uint8_t)i));
    lv_obj_set_style_bg_color(R.page_view, bg, 0);
    lv_color_t dim = lv_color_mix(fg, bg, 120);
    lv_obj_set_style_text_color(R.foot_l, dim, 0);
    lv_obj_set_style_text_color(R.foot_r, dim, 0);
    R.gen++;
}

static void relayout(void)
{
    reader_prefs_t *p = reader_prefs();
    if (p->em_idx < 0) p->em_idx = 0;
    if (p->em_idx >= N_EM) p->em_idx = N_EM - 1;
    book_layout(R.book, PAGE_W, PAGE_H, (float)EM_SIZES[p->em_idx]);
    R.counting_done = false;
    R.gen++;
}

/* Find a rendered buffer for `loc`, rendering it into the least recently
 * used slot if needed. */
static lv_draw_buf_t *page_buffer(book_loc_t loc)
{
    int lru = 0;
    for (int i = 0; i < NSLOTS; i++) {
        if (R.slot[i].buf && R.slot[i].gen == R.gen && book_loc_eq(R.slot[i].loc, loc)) {
            R.slot[i].used = ++R.use_clock;
            return R.slot[i].buf;
        }
        if (R.slot[i].used < R.slot[lru].used) lru = i;
    }
    if (!R.slot[lru].buf)
        R.slot[lru].buf = lv_draw_buf_create(PAGE_W, PAGE_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    book_render(R.book, loc, R.slot[lru].buf, R.lut);
    lv_image_cache_drop(R.slot[lru].buf);
    R.slot[lru].loc = loc;
    R.slot[lru].gen = R.gen;
    R.slot[lru].used = ++R.use_clock;
    return R.slot[lru].buf;
}

static void update_footer(void)
{
    const char *sec = book_section_title(R.book);
    lv_label_set_text(R.foot_l, sec ? sec : R.item->title);
    int page, total;
    float f = book_progress(R.book, &page, &total);
    R.item->percent = (int)(f * 100.0f + 0.5f);
    if (total) lv_label_set_text_fmt(R.foot_r, "%d / %d", page, total);
    else lv_label_set_text_fmt(R.foot_r, "%d%%", R.item->percent);
}

static void show_page(void)
{
    lv_draw_buf_t *buf = page_buffer(book_cur(R.book));
    lv_image_set_src(R.page_img, buf);
    lv_obj_invalidate(R.page_img);
    update_footer();
    R.prefetch_pending = true;
    lv_timer_resume(R.idle_timer);
}

/* Runs while reading: prefetch the next page first, then count chapters. */
static void idle_work(lv_timer_t *t)
{
    if (R.mode == MODE_LIBRARY || !R.book) {
        lv_timer_pause(t);
        return;
    }
    if (R.prefetch_pending) {
        R.prefetch_pending = false;
        book_loc_t cur = book_cur(R.book), nxt = book_after(R.book, cur);
        if (!book_loc_eq(cur, nxt)) page_buffer(nxt);
        return;
    }
    if (!R.counting_done) {
        R.counting_done = book_count_step(R.book);
        if (R.counting_done) update_footer();
        return;
    }
    lv_timer_pause(t);
}

static void save_position(void)
{
    if (!R.book || !R.item) return;
    R.item->mark = book_bookmark(R.book);
    R.item->last_opened = (long)time(NULL);
    shelf_save();
    R.turns_since_save = 0;
}

static void open_book(shelf_item_t *it)
{
    char err[256] = "";
    R.book = book_open(it->path, err, sizeof err);
    if (!R.book) {
        lv_obj_t *mb = lv_label_create(R.lib_view);
        lv_label_set_text_fmt(mb, "Can't open this book:\n%s", err);
        lv_obj_set_style_bg_opa(mb, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(mb, lv_color_hex(0x7f1d1d), 0);
        lv_obj_set_style_pad_all(mb, 6, 0);
        lv_obj_center(mb);
        lv_obj_delete_delayed(mb, 2500);
        return;
    }
    R.item = it;
    relayout();
    if (it->mark) book_goto_bookmark(R.book, it->mark);
    apply_theme();
    it->last_opened = (long)time(NULL);
    R.mode = MODE_PAGE;
    hide(R.lib_view, true);
    hide(R.page_view, false);
    show_page();
}

static void close_book(void)
{
    save_position();
    book_close(R.book);
    R.book = NULL;
    R.item = NULL;
    for (int i = 0; i < NSLOTS; i++) R.slot[i].gen = 0;   /* buffers are kept for reuse */
    R.mode = MODE_LIBRARY;
    hide(R.page_view, true);
    hide(R.toc_view, true);
    hide(R.lib_view, false);
    library_refresh();   /* re-sorts: this book moves to the top */
}

static void turn(bool forward)
{
    bool moved = forward ? book_next(R.book) : book_prev(R.book);
    if (!moved) {
        toast("%s", forward ? "End of book" : "Start of book");
        return;
    }
    show_page();
    if (++R.turns_since_save >= 20) save_position();
}

static void change_em(int delta)
{
    reader_prefs_t *p = reader_prefs();
    int n = p->em_idx + delta;
    if (n < 0 || n >= N_EM) return;
    p->em_idx = n;
    relayout();
    show_page();
    char s[16];
    snprintf(s, sizeof s, "%d", EM_SIZES[n]);
    toast("Text size %s", s);
}

static void cycle_theme(void)
{
    reader_prefs_t *p = reader_prefs();
    p->theme = (p->theme + 1) % N_THEMES;
    apply_theme();
    show_page();
    toast("%s", THEMES[p->theme].name);
}

/* ======================= contents ======================= */

static void toc_select(int i)
{
    if (R.toc_count == 0) return;
    if (i < 0) i = 0;
    if (i >= R.toc_count) i = R.toc_count - 1;
    lv_obj_t *old = lv_obj_get_child(R.toc_list, R.toc_sel);
    if (old) lv_obj_remove_state(old, LV_STATE_CHECKED);
    R.toc_sel = i;
    lv_obj_t *row = lv_obj_get_child(R.toc_list, i);
    lv_obj_add_state(row, LV_STATE_CHECKED);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

static void toc_open(void)
{
    const book_toc_t *toc;
    R.toc_count = book_toc(R.book, &toc);
    if (R.toc_count == 0) {
        toast("%s", "No table of contents");
        return;
    }
    lv_obj_clean(R.toc_list);
    book_loc_t cur = book_cur(R.book);
    int current = 0;
    for (int i = 0; i < R.toc_count; i++) {
        lv_obj_t *l = lv_label_create(R.toc_list);
        lv_label_set_text(l, toc[i].title);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_pad_ver(l, 3, 0);
        lv_obj_set_style_pad_left(l, 8 + 12 * toc[i].depth, 0);
        lv_obj_set_style_pad_right(l, 8, 0);
        lv_obj_set_style_radius(l, 5, 0);
        lv_obj_set_style_bg_color(l, lv_color_hex(ZOS_COLOR_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(l, lv_color_hex(0x111111), LV_STATE_CHECKED);
        if (toc[i].loc.chapter >= 0 &&
            (toc[i].loc.chapter < cur.chapter ||
             (toc[i].loc.chapter == cur.chapter && toc[i].loc.page <= cur.page)))
            current = i;
    }
    R.toc_sel = 0;
    R.mode = MODE_TOC;
    hide(R.toc_view, false);
    lv_obj_move_foreground(R.toc_view);
    toc_select(current);
}

static void toc_jump(void)
{
    const book_toc_t *toc;
    book_toc(R.book, &toc);
    if (toc[R.toc_sel].loc.chapter >= 0) book_goto(R.book, toc[R.toc_sel].loc);
    R.mode = MODE_PAGE;
    hide(R.toc_view, true);
    show_page();
}

/* ======================= input ======================= */

static bool is_back(uint32_t k) { return k == LV_KEY_ESC || k == 'q' || k == '`'; }

static void on_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (R.mode) {
    case MODE_LIBRARY:
        if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT) lib_select(R.lib_sel + 1);
        else if (k == LV_KEY_UP || k == LV_KEY_LEFT) lib_select(R.lib_sel - 1);
        else if ((k == LV_KEY_ENTER || k == ' ') && R.lib_count) open_book(R.lib_items[R.lib_sel]);
        else if (k == 'r') library_refresh();
        else if (is_back(k) || k == LV_KEY_BACKSPACE) zos_app_exit();
        break;
    case MODE_PAGE:
        if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN || k == ' ' || k == LV_KEY_ENTER || k == 'n')
            turn(true);
        else if (k == LV_KEY_LEFT || k == LV_KEY_UP || k == LV_KEY_BACKSPACE || k == 'p')
            turn(false);
        else if (k == '+' || k == '=') change_em(+1);
        else if (k == '-' || k == '_') change_em(-1);
        else if (k == 't') cycle_theme();
        else if (k == 'c') toc_open();
        else if (is_back(k)) close_book();
        break;
    case MODE_TOC:
        if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT) toc_select(R.toc_sel + 1);
        else if (k == LV_KEY_UP || k == LV_KEY_LEFT) toc_select(R.toc_sel - 1);
        else if (k == LV_KEY_ENTER || k == ' ') toc_jump();
        else if (is_back(k) || k == 'c' || k == LV_KEY_BACKSPACE) {
            R.mode = MODE_PAGE;
            hide(R.toc_view, true);
        }
        break;
    }
}

/* ======================= app lifecycle ======================= */

static void exit_on_key(lv_event_t *e)
{
    LV_UNUSED(e);
    zos_app_exit();
}

static void reader_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&R, 0, sizeof R);
    R.scr = scr;
    if (!book_lib_init()) {
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, "MuPDF failed to start.");
        lv_obj_center(l);
        lv_obj_t *s = plain_obj(scr);
        lv_obj_add_event_cb(s, exit_on_key, LV_EVENT_KEY, NULL);
        lv_group_add_obj(g, s);
        return;
    }
    shelf_load(zos_cfg.state_dir);

    /* key sink: one focused object, dispatches by mode */
    lv_obj_t *sink = plain_obj(scr);
    lv_obj_set_size(sink, 1, 1);
    lv_obj_add_event_cb(sink, on_key, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g, sink);

    /* library */
    R.lib_view = plain_obj(scr);
    lv_obj_set_size(R.lib_view, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_t *bar = zos_statusbar_create(R.lib_view, "");
    R.lib_title = lv_obj_get_child(bar, 0);
    R.lib_list = lv_obj_create(R.lib_view);
    lv_obj_remove_style_all(R.lib_list);
    lv_obj_set_size(R.lib_list, ZOS_HOR_RES, ZOS_VER_RES - ZOS_STATUSBAR_H);
    lv_obj_set_pos(R.lib_list, 0, ZOS_STATUSBAR_H);
    lv_obj_set_style_pad_hor(R.lib_list, 4, 0);
    lv_obj_set_style_pad_row(R.lib_list, 1, 0);
    lv_obj_set_flex_flow(R.lib_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(R.lib_list, LV_SCROLLBAR_MODE_OFF);

    /* page */
    R.page_view = plain_obj(scr);
    lv_obj_set_size(R.page_view, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_set_style_bg_opa(R.page_view, LV_OPA_COVER, 0);
    R.page_img = lv_image_create(R.page_view);
    lv_obj_set_pos(R.page_img, 0, 0);
    R.foot_l = lv_label_create(R.page_view);
    lv_obj_set_style_text_font(R.foot_l, zos_font.small, 0);
    lv_label_set_long_mode(R.foot_l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(R.foot_l, 220);
    lv_obj_align(R.foot_l, LV_ALIGN_BOTTOM_LEFT, 9, 0);
    R.foot_r = lv_label_create(R.page_view);
    lv_obj_set_style_text_font(R.foot_r, zos_font.small, 0);
    lv_obj_align(R.foot_r, LV_ALIGN_BOTTOM_RIGHT, -9, 0);
    hide(R.page_view, true);

    /* contents overlay */
    R.toc_view = plain_obj(scr);
    lv_obj_set_size(R.toc_view, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_set_style_bg_opa(R.toc_view, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(R.toc_view, lv_color_hex(ZOS_COLOR_BG), 0);
    lv_obj_t *tb = zos_statusbar_create(R.toc_view, "Contents");
    LV_UNUSED(tb);
    R.toc_list = lv_obj_create(R.toc_view);
    lv_obj_remove_style_all(R.toc_list);
    lv_obj_set_size(R.toc_list, ZOS_HOR_RES, ZOS_VER_RES - ZOS_STATUSBAR_H);
    lv_obj_set_pos(R.toc_list, 0, ZOS_STATUSBAR_H);
    lv_obj_set_style_pad_hor(R.toc_list, 4, 0);
    lv_obj_set_flex_flow(R.toc_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(R.toc_list, LV_SCROLLBAR_MODE_OFF);
    hide(R.toc_view, true);

    /* toast */
    R.toast = lv_label_create(scr);
    lv_obj_set_style_bg_opa(R.toast, 230, 0);
    lv_obj_set_style_bg_color(R.toast, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_color(R.toast, lv_color_hex(0xf5f5f5), 0);
    lv_obj_set_style_pad_hor(R.toast, 10, 0);
    lv_obj_set_style_pad_ver(R.toast, 4, 0);
    lv_obj_set_style_radius(R.toast, 8, 0);
    lv_obj_align(R.toast, LV_ALIGN_CENTER, 0, 0);
    hide(R.toast, true);

    R.idle_timer = lv_timer_create(idle_work, 15, NULL);
    lv_timer_pause(R.idle_timer);

    R.mode = MODE_LIBRARY;
    library_refresh();
}

static void reader_close(void)
{
    if (R.book) {
        save_position();
        book_close(R.book);
        R.book = NULL;
    }
    shelf_save();
    if (R.idle_timer) lv_timer_delete(R.idle_timer);
    if (R.toast_timer) lv_timer_delete(R.toast_timer);
    for (int i = 0; i < NSLOTS; i++)
        if (R.slot[i].buf) lv_draw_buf_destroy(R.slot[i].buf);
    shelf_free();
    book_lib_done();
    memset(&R, 0, sizeof R);
}

const zos_app_t zos_app_reader = {
    .id = "reader",
    .name = "Books",
    .tagline = "Read EPUB, PDF and comics",
    .icon = &zos_icon_book,
    .color = 0xf5a524,
    .open = reader_open,
    .close = reader_close,
};
