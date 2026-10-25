/* Emby: browse your Emby server and play through mpv.
 *
 *   sign in  -> home (Continue watching + libraries) -> folders -> details -> player
 *
 * Video is transcoded by the server to small H.264 HLS (quality: Q), which
 * the CM0 decodes in hardware; mpv draws it straight to the panel. Playback
 * position is reported back, so resume works across all Emby clients. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/form.h"
#include "../../core/player.h"
#include "../../core/task.h"
#include "../../zos.h"
#include "emby_api.h"

extern const lv_image_dsc_t zos_icon_media;
extern const zos_app_t zos_app_emby;

#define MAX_DEPTH 8
#define PAGE_SIZE 100
#define THUMB 34
#define ACCENT 0x52b54b

typedef enum { V_LOGIN, V_LIST, V_DETAIL, V_PLAYER } view_t;

typedef struct {
    char title[96];
    char path[400];        /* items query; "" for home */
    bool home;
    emby_item_t *items;
    char **thumbs;         /* local image path per item, or NULL */
    bool *thumb_asked;
    int n, total, sel;
    int resume_count;      /* home: first N items are "Continue watching" */
    bool loading;
    char error[160];
    uint32_t gen;
} page_t;

static struct {
    lv_obj_t *scr, *sink;
    lv_group_t *g;
    view_t view;
    emby_cfg_t cfg;

    /* login */
    zos_form_t *form;
    char f_server[200], f_user[64], f_pass[96];
    zos_field_t fields[3];

    /* list */
    lv_obj_t *list_view, *list_title, *list, *list_msg, *list_hint;
    lv_obj_t *row_img[PAGE_SIZE * 4 + 16];
    page_t pages[MAX_DEPTH];
    int depth;
    bool confirm_logout;

    /* detail */
    lv_obj_t *detail_view, *d_poster, *d_title, *d_meta, *d_over, *d_resume, *d_hint;
    emby_item_t cur;
    char cur_thumb[600];

    /* player */
    lv_obj_t *player_view, *p_poster, *p_title, *p_sub, *p_bar, *p_time, *p_state;
    char play_session[40];
    const char *play_method;
    uint32_t last_report;
    bool last_paused;
    emby_item_t playing;
    int play_page, play_index;   /* for audio auto-advance */

    uint32_t gen;
} E;

static const char owner_tag;

static void set_view(view_t v);
static void list_render(void);
static void load_page(page_t *p);

/* ======================= small helpers ======================= */

static lv_obj_t *box(lv_obj_t *p)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *page_obj(lv_obj_t *scr)
{
    lv_obj_t *o = box(scr);
    lv_obj_set_size(o, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(ZOS_COLOR_BG), 0);
    return o;
}

static void fmt_time(double s, char *out, size_t n)
{
    int t = s > 0 ? (int)s : 0;
    if (t >= 3600) snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void subtitle_of(const emby_item_t *it, char *out, size_t n)
{
    char rt[24] = "";
    int mins = (int)(it->runtime_ticks / EMBY_TICKS / 60);
    if (mins) snprintf(rt, sizeof rt, "%d min", mins);
    const char *dot = " \xc2\xb7 ";
    if (!strcmp(it->type, "Episode"))
        snprintf(out, n, "%s%sS%dE%d%s%s", it->series, dot, it->parent_index, it->index, rt[0] ? dot : "", rt);
    else if (!strcmp(it->type, "Audio"))
        snprintf(out, n, "%s", it->artist);
    else if (!strcmp(it->type, "MusicAlbum"))
        snprintf(out, n, "%s%s%d tracks", it->artist, it->artist[0] ? dot : "", it->child_count);
    else if (!strcmp(it->type, "Series"))
        snprintf(out, n, it->year ? "TV series%s%d" : "TV series", dot, it->year);
    else if (!strcmp(it->type, "Season"))
        snprintf(out, n, "%d episode%s", it->child_count, it->child_count == 1 ? "" : "s");
    else if (it->is_folder) {
        static const char *names[][2] = {{"movies", "Movies"}, {"tvshows", "TV shows"}, {"music", "Music"},
                                         {"musicvideos", "Music videos"}, {"homevideos", "Home videos"},
                                         {"boxsets", "Collections"}, {"playlists", "Playlists"},
                                         {"books", "Books"}, {"livetv", "Live TV"}};
        const char *nm = it->child_count ? NULL : "Folder";
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!strcmp(it->collection, names[i][0])) nm = names[i][1];
        if (nm && it->collection[0]) snprintf(out, n, "%s library", nm);
        else if (it->child_count) snprintf(out, n, "%d item%s", it->child_count, it->child_count == 1 ? "" : "s");
        else snprintf(out, n, "Folder");
    }
    else if (it->year)
        snprintf(out, n, "%d%s%s", it->year, rt[0] ? dot : "", rt);
    else
        snprintf(out, n, "%s", rt);
}

static void page_free(page_t *p)
{
    for (int i = 0; i < p->n; i++) free(p->thumbs ? p->thumbs[i] : NULL);
    free(p->items);
    free(p->thumbs);
    free(p->thumb_asked);
    memset(p, 0, sizeof *p);
}

static page_t *top(void) { return &E.pages[E.depth - 1]; }

/* ======================= jobs ======================= */

typedef struct {
    int kind;                /* 0 page, 1 thumb, 2 login, 3 report, 4 detail image */
    uint32_t gen;
    int depth;
    emby_cfg_t cfg;
    char path[400];
    bool home;
    emby_item_t *items;
    int n, total, resume_count, rc;
    emby_item_t item;
    char *img;
    int iw, ih;
    char err[200];
    /* login */
    char server[200], user[64], pass[96];
    bool ok;
    /* report */
    char kind_s[16], ps[40], method[16], event[16];
    int64_t pos;
    bool paused, stop_enc;
} job_t;

static void job_work(void *a)
{
    job_t *j = a;
    switch (j->kind) {
    case 0:
        if (j->home) {
            emby_item_t *res = NULL, *views = NULL;
            int nr = emby_get_items(&j->cfg, "/Users/{uid}/Items/Resume?Limit=12&Fields=Overview&MediaTypes=Video,Audio",
                                    &res, NULL, j->err, sizeof j->err);
            int nv = emby_get_items(&j->cfg, "/Users/{uid}/Views", &views, NULL, j->err, sizeof j->err);
            if (nv < 0) {
                j->rc = nv;
                free(res);
                break;
            }
            if (nr < 0) nr = 0;
            j->n = nr + nv;
            j->items = calloc((size_t)(j->n ? j->n : 1), sizeof *j->items);
            if (nr) memcpy(j->items, res, sizeof *res * (size_t)nr);
            if (nv) memcpy(j->items + nr, views, sizeof *views * (size_t)nv);
            j->resume_count = nr;
            j->total = j->n;
            free(res);
            free(views);
            j->rc = j->n;
        } else {
            j->rc = emby_get_items(&j->cfg, j->path, &j->items, &j->total, j->err, sizeof j->err);
            j->n = j->rc > 0 ? j->rc : 0;
        }
        break;
    case 1:
    case 4: j->img = emby_fetch_image(&j->cfg, &j->item, j->iw, j->ih); break;
    case 2: j->ok = emby_login(&j->cfg, j->server, j->user, j->pass, j->err, sizeof j->err); break;
    case 3:
        emby_report(&j->cfg, j->kind_s, j->item.id, j->ps, j->pos, j->paused, j->method, j->event[0] ? j->event : NULL);
        if (j->stop_enc) emby_stop_encodings(&j->cfg, j->ps);
        break;
    }
}

static void job_release(void *a)
{
    job_t *j = a;
    free(j->items);
    free(j->img);
    free(j);
}

static void open_login(const char *msg);

static void job_done(void *a)
{
    job_t *j = a;
    if (j->kind == 0) {
        if (j->depth > E.depth || j->gen != E.pages[j->depth - 1].gen) return;   /* user moved on */
        page_t *p = &E.pages[j->depth - 1];
        p->loading = false;
        if (j->rc == -2) {
            emby_cfg_forget();
            open_login("Your session expired. Please sign in again.");
            return;
        }
        if (j->rc < 0) {
            snprintf(p->error, sizeof p->error, "%s", j->err);
        } else {
            int base = p->n;   /* appending a further page? */
            p->items = realloc(p->items, sizeof *p->items * (size_t)(base + j->n + 1));
            p->thumbs = realloc(p->thumbs, sizeof *p->thumbs * (size_t)(base + j->n + 1));
            p->thumb_asked = realloc(p->thumb_asked, sizeof *p->thumb_asked * (size_t)(base + j->n + 1));
            for (int i = 0; i < j->n; i++) {
                p->items[base + i] = j->items[i];
                p->thumbs[base + i] = NULL;
                p->thumb_asked[base + i] = false;
            }
            p->n = base + j->n;
            p->total = j->total;
            p->resume_count = base ? p->resume_count : j->resume_count;
            p->error[0] = 0;
        }
        if (E.view == V_LIST && j->depth == E.depth) list_render();
    } else if (j->kind == 1) {
        if (j->depth > E.depth || j->gen != E.pages[j->depth - 1].gen || !j->img) return;
        page_t *p = &E.pages[j->depth - 1];
        for (int i = 0; i < p->n; i++)
            if (!strcmp(p->items[i].id, j->item.id) && !p->thumbs[i]) {
                p->thumbs[i] = strdup(j->img);
                if (j->depth == E.depth && i < (int)(sizeof E.row_img / sizeof E.row_img[0]) && E.row_img[i]) {
                    char src[640];
                    snprintf(src, sizeof src, "A:%s", j->img);
                    lv_image_set_src(E.row_img[i], src);
                }
            }
    } else if (j->kind == 4) {
        if (!j->img || strcmp(j->item.id, E.cur.id)) return;
        snprintf(E.cur_thumb, sizeof E.cur_thumb, "A:%s", j->img);
        lv_image_set_src(E.d_poster, E.cur_thumb);
        lv_image_set_src(E.p_poster, E.cur_thumb);
        lv_obj_remove_flag(E.d_poster, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(E.p_poster, LV_OBJ_FLAG_HIDDEN);
    } else if (j->kind == 2) {
        if (!j->ok) {
            zos_form_message(E.form, j->err, true);
            return;
        }
        int q = E.cfg.quality;
        E.cfg = j->cfg;
        E.cfg.quality = q;
        emby_cfg_save(&E.cfg);
        lv_obj_delete(zos_form_obj(E.form));
        E.form = NULL;
        for (int i = 0; i < MAX_DEPTH; i++) page_free(&E.pages[i]);
        E.depth = 1;
        E.pages[0].home = true;
        load_page(&E.pages[0]);
        set_view(V_LIST);
    }
}

static job_t *new_job(int kind)
{
    job_t *j = calloc(1, sizeof *j);
    j->kind = kind;
    j->cfg = E.cfg;
    return j;
}

static void load_page(page_t *p)
{
    job_t *j = new_job(0);
    p->loading = true;
    p->gen = ++E.gen;
    j->gen = p->gen;
    j->depth = (int)(p - E.pages) + 1;
    j->home = p->home;
    if (!p->home) snprintf(j->path, sizeof j->path, "%s&StartIndex=%d&Limit=%d", p->path, p->n, PAGE_SIZE);
    zos_task_run(&owner_tag, job_work, job_done, job_release, j);
}

static void ask_thumbs(void)
{
    page_t *p = top();
    for (int i = p->sel - 2; i <= p->sel + 5; i++) {
        if (i < 0 || i >= p->n || p->thumb_asked[i] || !p->items[i].image_tag[0]) continue;
        p->thumb_asked[i] = true;
        job_t *j = new_job(1);
        j->gen = p->gen;
        j->depth = E.depth;
        j->item = p->items[i];
        j->iw = THUMB;
        j->ih = THUMB;
        zos_task_run(&owner_tag, job_work, job_done, job_release, j);
    }
}

static void report(const char *kind, int64_t pos, bool paused, const char *event, bool stop_enc)
{
    job_t *j = new_job(3);
    j->item = E.playing;
    snprintf(j->kind_s, sizeof j->kind_s, "%s", kind);
    snprintf(j->ps, sizeof j->ps, "%s", E.play_session);
    snprintf(j->method, sizeof j->method, "%s", E.play_method ? E.play_method : "Transcode");
    snprintf(j->event, sizeof j->event, "%s", event ? event : "");
    j->pos = pos;
    j->paused = paused;
    j->stop_enc = stop_enc;
    /* reports must reach the server even if the app closes: no owner */
    zos_task_run(NULL, job_work, NULL, job_release, j);
}

/* ======================= login ======================= */

static void login_submit(zos_form_t *f, void *ud)
{
    LV_UNUSED(ud);
    if (!E.f_server[0] || !E.f_user[0]) {
        zos_form_message(f, "Server and username are required.", true);
        return;
    }
    zos_form_message(f, "Signing in\xe2\x80\xa6", false);
    job_t *j = new_job(2);
    snprintf(j->server, sizeof j->server, "%s", E.f_server);
    snprintf(j->user, sizeof j->user, "%s", E.f_user);
    snprintf(j->pass, sizeof j->pass, "%s", E.f_pass);
    memset(E.f_pass, 0, sizeof E.f_pass);   /* only the token is kept */
    zos_task_run(&owner_tag, job_work, job_done, job_release, j);
}

static void login_cancel(zos_form_t *f, void *ud)
{
    LV_UNUSED(f);
    LV_UNUSED(ud);
    zos_app_exit();
}

static void open_login(const char *msg)
{
    E.view = V_LOGIN;
    lv_obj_add_flag(E.list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(E.detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(E.player_view, LV_OBJ_FLAG_HIDDEN);
    snprintf(E.f_server, sizeof E.f_server, "%s", E.cfg.server);
    snprintf(E.f_user, sizeof E.f_user, "%s", E.cfg.user_name);
    E.f_pass[0] = 0;
    E.fields[0] = (zos_field_t){"Server", E.f_server, sizeof E.f_server, "192.168.1.10:8096", false, false};
    E.fields[1] = (zos_field_t){"User", E.f_user, sizeof E.f_user, "Emby username", false, false};
    E.fields[2] = (zos_field_t){"Password", E.f_pass, sizeof E.f_pass, "", true, false};
    if (E.form) lv_obj_delete(zos_form_obj(E.form));
    E.form = zos_form_create(E.scr, E.g, "Emby \xc2\xb7 Sign in", E.fields, 3, login_submit, login_cancel, NULL);
    zos_form_message(E.form, msg ? msg : "Your password is only used to get a token; it isn't stored.", msg != NULL);
    zos_form_focus(E.form);
}

/* ======================= list ======================= */

static void list_select(int i)
{
    page_t *p = top();
    if (!p->n) return;
    if (i < 0) i = 0;
    if (i >= p->n) i = p->n - 1;
    for (uint32_t c = 0; c < lv_obj_get_child_count(E.list); c++) lv_obj_remove_state(lv_obj_get_child(E.list, (int32_t)c), LV_STATE_CHECKED);
    p->sel = i;
    /* rows carry their item index as user data (headers carry -1) */
    for (uint32_t c = 0; c < lv_obj_get_child_count(E.list); c++) {
        lv_obj_t *row = lv_obj_get_child(E.list, (int32_t)c);
        if ((intptr_t)lv_obj_get_user_data(row) == i) {
            lv_obj_add_state(row, LV_STATE_CHECKED);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        }
    }
    ask_thumbs();
    /* near the end of a long folder: fetch the next page */
    if (!p->home && !p->loading && p->n < p->total && i >= p->n - 4) load_page(p);
}

static lv_obj_t *section(const char *t)
{
    lv_obj_t *h = box(E.list);
    lv_obj_set_size(h, lv_pct(100), 15);
    lv_obj_set_user_data(h, (void *)(intptr_t)-1);
    lv_obj_t *l = label(h, zos_font.small, ACCENT);
    lv_label_set_text(l, t);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 6, 0);
    return h;
}

static void list_render(void)
{
    page_t *p = top();
    char title[140];
    if (p->home) snprintf(title, sizeof title, "Emby \xc2\xb7 %s", E.cfg.server_name[0] ? E.cfg.server_name : E.cfg.server);
    else snprintf(title, sizeof title, "%s", p->title);
    lv_label_set_text(E.list_title, title);
    lv_obj_clean(E.list);
    memset(E.row_img, 0, sizeof E.row_img);

    if (p->loading && !p->n) lv_label_set_text(E.list_msg, "Loading\xe2\x80\xa6");
    else if (p->error[0]) lv_label_set_text(E.list_msg, p->error);
    else if (!p->n) lv_label_set_text(E.list_msg, "Nothing here.");
    else lv_label_set_text(E.list_msg, "");

    for (int i = 0; i < p->n; i++) {
        if (p->home && i == 0 && p->resume_count) section("CONTINUE WATCHING");
        if (p->home && i == p->resume_count) section("LIBRARIES");
        emby_item_t *it = &p->items[i];
        lv_obj_t *row = box(E.list);
        lv_obj_set_size(row, lv_pct(100), THUMB + 4);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 4, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(ZOS_COLOR_SURFACE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(row, 3, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(row, lv_color_hex(ACCENT), LV_STATE_CHECKED);

        lv_obj_t *slot = box(row);
        lv_obj_set_size(slot, THUMB, THUMB);
        lv_obj_set_style_radius(slot, 4, 0);
        lv_obj_set_style_clip_corner(slot, true, 0);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(slot, lv_color_hex(0x232a33), 0);
        lv_obj_align(slot, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *img = lv_image_create(slot);
        lv_obj_center(img);
        if (p->thumbs[i]) {
            char src[640];
            snprintf(src, sizeof src, "A:%s", p->thumbs[i]);
            lv_image_set_src(img, src);
        } else if (it->is_folder && !it->image_tag[0]) {
            lv_obj_t *ic = label(slot, &lv_font_montserrat_16, 0x6b7280);
            lv_label_set_text(ic, !strcmp(it->collection, "music") || !strcmp(it->type, "MusicAlbum") ? LV_SYMBOL_AUDIO
                                  : LV_SYMBOL_DIRECTORY);
            lv_obj_center(ic);
        }
        if (i < (int)(sizeof E.row_img / sizeof E.row_img[0])) E.row_img[i] = img;

        lv_obj_t *t = label(row, zos_font.body, it->played ? ZOS_COLOR_MUTED : ZOS_COLOR_TEXT);
        char name[160];
        if ((!strcmp(it->type, "Audio") || !strcmp(it->type, "Episode")) && it->index)
            snprintf(name, sizeof name, "%d. %s", it->index, it->name);
        else
            snprintf(name, sizeof name, "%s", it->name);
        lv_label_set_text(t, name);
        zos_label_one_line(t, 236);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, THUMB + 7, 2);

        char sub[200];
        subtitle_of(it, sub, sizeof sub);
        if (it->pos_ticks > 0 && it->runtime_ticks > 0) {
            char left[24];
            fmt_time((double)(it->runtime_ticks - it->pos_ticks) / EMBY_TICKS, left, sizeof left);
            size_t l = strlen(sub);
            snprintf(sub + l, sizeof sub - l, "%s%s left", l ? " \xc2\xb7 " : "", left);
        }
        if (it->played) {
            size_t l = strlen(sub);
            snprintf(sub + l, sizeof sub - l, "%s\xe2\x9c\x93 watched", l ? " \xc2\xb7 " : "");
        }
        lv_obj_t *s = label(row, zos_font.small, ZOS_COLOR_MUTED);
        lv_label_set_text(s, sub);
        zos_label_one_line(s, 236);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, THUMB + 7, -3);

        if (it->pos_ticks > 0 && it->runtime_ticks > 0) {   /* resume progress */
            lv_obj_t *bar = box(slot);
            lv_obj_set_size(bar, THUMB, 3);
            lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x111111), 0);
            lv_obj_t *fill = box(bar);
            lv_obj_set_size(fill, (int32_t)(THUMB * it->pos_ticks / it->runtime_ticks), 3);
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(fill, lv_color_hex(ACCENT), 0);
        }
    }
    if (p->loading && p->n) {
        lv_obj_t *more = label(E.list, zos_font.small, ZOS_COLOR_MUTED);
        lv_label_set_text(more, "  Loading more\xe2\x80\xa6");
        lv_obj_set_user_data(more, (void *)(intptr_t)-1);
    }
    char hint[160];
    snprintf(hint, sizeof hint, "%s    Q quality: %s    %s",
             LV_SYMBOL_NEW_LINE " open", emby_qualities[E.cfg.quality].name, p->home ? "L sign out   Esc" : "Esc back");
    lv_label_set_text(E.list_hint, E.confirm_logout ? "Press L again to sign out" : hint);
    int keep = p->sel;
    list_select(keep);
}

static void open_folder(const emby_item_t *it)
{
    if (E.depth >= MAX_DEPTH) return;
    page_t *p = &E.pages[E.depth++];
    page_free(p);
    snprintf(p->title, sizeof p->title, "%s", it->name);
    /* Index order keeps seasons, episodes and album tracks in sequence; other folders fall back to name. */
    snprintf(p->path, sizeof p->path,
             "/Users/{uid}/Items?ParentId=%s&SortBy=ParentIndexNumber,IndexNumber,SortName&SortOrder=Ascending"
             "&Fields=Overview,ChildCount&EnableImageTypes=Primary",
             it->id);
    load_page(p);
    list_render();
}

/* ======================= detail ======================= */

static void ask_poster(const emby_item_t *it)
{
    E.cur_thumb[0] = 0;
    lv_obj_add_flag(E.d_poster, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(E.p_poster, LV_OBJ_FLAG_HIDDEN);
    if (!it->image_tag[0]) return;
    job_t *j = new_job(4);
    j->item = *it;
    j->iw = 68;
    j->ih = 100;
    zos_task_run(&owner_tag, job_work, job_done, job_release, j);
}

static void detail_render(void)
{
    emby_item_t *it = &E.cur;
    lv_label_set_text(E.d_title, it->name);
    char meta[200];
    subtitle_of(it, meta, sizeof meta);
    if (it->rating[0]) {
        size_t l = strlen(meta);
        snprintf(meta + l, sizeof meta - l, " \xc2\xb7 %s", it->rating);
    }
    lv_label_set_text(E.d_meta, meta);
    lv_label_set_text(E.d_over, it->overview[0] ? it->overview : "No description.");
    lv_obj_scroll_to_y(E.d_over, 0, LV_ANIM_OFF);
    if (it->pos_ticks > 0) {
        char t[32], s[96];
        fmt_time((double)it->pos_ticks / EMBY_TICKS, t, sizeof t);
        snprintf(s, sizeof s, "Resume from %s", t);
        lv_label_set_text(E.d_resume, s);
    } else {
        lv_label_set_text(E.d_resume, it->played ? "\xe2\x9c\x93 Watched" : "");
    }
    char hint[160];
    snprintf(hint, sizeof hint, "%s    %s    Q %s    Esc back",
             it->pos_ticks > 0 ? LV_SYMBOL_NEW_LINE " resume" : LV_SYMBOL_NEW_LINE " play",
             it->pos_ticks > 0 ? "R from start" : "", emby_qualities[E.cfg.quality].name);
    lv_label_set_text(E.d_hint, hint);
}

static void open_detail(const emby_item_t *it, const char *thumb)
{
    E.cur = *it;
    detail_render();
    ask_poster(it);
    LV_UNUSED(thumb);
    set_view(V_DETAIL);
}

/* ======================= player ======================= */

static void player_render(void)
{
    const zos_player_state_t *s = zos_player_state();
    char a[24], b[24], t[64];
    fmt_time(s->pos, a, sizeof a);
    fmt_time(s->dur > 0 ? s->dur : (double)E.playing.runtime_ticks / EMBY_TICKS, b, sizeof b);
    snprintf(t, sizeof t, "%s / %s", a, b);
    lv_label_set_text(E.p_time, t);
    double dur = s->dur > 0 ? s->dur : (double)E.playing.runtime_ticks / EMBY_TICKS;
    lv_bar_set_value(E.p_bar, dur > 0 ? (int32_t)(1000 * s->pos / dur) : 0, LV_ANIM_OFF);
    const char *st = !s->running ? "Starting\xe2\x80\xa6" : s->buffering ? "Buffering\xe2\x80\xa6" : s->paused ? "Paused" : "Playing";
    char line[96];
    snprintf(line, sizeof line, "%s  \xc2\xb7  vol %d%%  \xc2\xb7  %s", st, s->volume, E.play_method ? E.play_method : "");
    lv_label_set_text(E.p_state, line);
}

static void on_player_change(void *ud)
{
    LV_UNUSED(ud);
    const zos_player_state_t *s = zos_player_state();
    if (E.view == V_PLAYER) player_render();
    int64_t pos = (int64_t)(s->pos * EMBY_TICKS);
    if (s->paused != E.last_paused) {
        E.last_paused = s->paused;
        report("/Progress", pos, s->paused, s->paused ? "Pause" : "Unpause", false);
        E.last_report = lv_tick_get();
    } else if (lv_tick_get() - E.last_report > 10000) {   /* Emby asks for ~10 s check-ins */
        report("/Progress", pos, s->paused, "TimeUpdate", false);
        E.last_report = lv_tick_get();
    }
}

static void start_playback(const emby_item_t *it, bool from_start);

static void update_local_position(emby_item_t *it, int64_t pos, bool finished)
{
    if (finished) {
        it->pos_ticks = 0;
        it->played = true;
    } else {
        it->pos_ticks = pos;
    }
}

static void on_player_end(bool eof, double last_pos, void *ud)
{
    LV_UNUSED(ud);
    int64_t pos = (int64_t)(last_pos * EMBY_TICKS);
    bool finished = eof || (E.playing.runtime_ticks && pos > E.playing.runtime_ticks * 9 / 10);
    report("/Stopped", finished ? E.playing.runtime_ticks : pos, false, NULL, !strcmp(E.play_method, "Transcode"));
    /* reflect it locally right away: detail, list, Continue watching */
    if (!strcmp(E.cur.id, E.playing.id)) update_local_position(&E.cur, pos, finished);
    for (int d = 0; d < E.depth; d++)
        for (int i = 0; i < E.pages[d].n; i++)
            if (!strcmp(E.pages[d].items[i].id, E.playing.id)) update_local_position(&E.pages[d].items[i], pos, finished);

    /* music: carry on with the next track of the album */
    if (eof && !emby_is_video(&E.playing) && E.play_page >= 0 && E.play_page < E.depth) {
        page_t *p = &E.pages[E.play_page];
        int next = E.play_index + 1;
        if (next < p->n && emby_is_playable(&p->items[next]) && !emby_is_video(&p->items[next])) {
            E.play_index = next;
            E.cur = p->items[next];
            ask_poster(&E.cur);
            start_playback(&p->items[next], true);
            return;
        }
    }
    if (E.view == V_PLAYER) {
        if (!emby_is_video(&E.playing)) set_view(V_LIST);
        else {
            detail_render();
            set_view(V_DETAIL);
        }
    }
}

static void start_playback(const emby_item_t *it, bool from_start)
{
    for (int i = 0; i < 32; i++) E.play_session[i] = "0123456789abcdef"[lv_rand(0, 15)];
    E.play_session[32] = 0;
    E.playing = *it;
    char *url = emby_stream_url(&E.cfg, it, E.play_session, &E.play_method);
    double start = from_start ? 0 : (double)it->pos_ticks / EMBY_TICKS;
    char title[200];
    if (!strcmp(it->type, "Episode")) snprintf(title, sizeof title, "%s \xc2\xb7 %s", it->series, it->name);
    else snprintf(title, sizeof title, "%s", it->name);
    lv_label_set_text(E.p_title, it->name);
    char sub[200];
    subtitle_of(it, sub, sizeof sub);
    lv_label_set_text(E.p_sub, sub);
    if (E.cur_thumb[0]) {
        lv_image_set_src(E.p_poster, E.cur_thumb);
        lv_obj_remove_flag(E.p_poster, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(E.p_poster, LV_OBJ_FLAG_HIDDEN);
    }
    bool ok = zos_player_start(url, start, emby_is_video(it), title,
                               (zos_player_cbs_t){on_player_change, on_player_end}, NULL);
    free(url);
    if (!ok) {
        zos_notify(ACCENT, "Emby", "Couldn't start mpv. Is it installed?");
        return;
    }
    E.last_paused = false;
    E.last_report = lv_tick_get();
    report("", (int64_t)(start * EMBY_TICKS), false, NULL, false);
    set_view(V_PLAYER);
    player_render();
}

/* ======================= keys ======================= */

static void list_key(uint32_t k)
{
    page_t *p = top();
    if (k != 'l') E.confirm_logout = false;
    if (k == LV_KEY_DOWN) list_select(p->sel + 1);
    else if (k == LV_KEY_UP) list_select(p->sel - 1);
    else if (k == ZOS_KEY_PGDN || k == LV_KEY_RIGHT) list_select(p->sel + 4);
    else if (k == ZOS_KEY_PGUP || k == LV_KEY_LEFT) list_select(p->sel - 4);
    else if (k == 'q') {
        E.cfg.quality = (E.cfg.quality + 1) % emby_quality_count;
        emby_cfg_save(&E.cfg);
        list_render();
    } else if (k == 'l' && p->home) {
        if (E.confirm_logout) {
            emby_cfg_forget();
            E.cfg.token[0] = 0;
            E.confirm_logout = false;
            open_login(NULL);
        } else {
            E.confirm_logout = true;
            list_render();
        }
    } else if (k == LV_KEY_ESC || k == '`' || k == LV_KEY_BACKSPACE) {
        if (E.depth > 1) {
            page_free(&E.pages[--E.depth]);
            list_render();
        } else {
            zos_app_exit();
        }
    } else if ((k == LV_KEY_ENTER || k == ' ') && p->n) {
        emby_item_t *it = &p->items[p->sel];
        if (it->is_folder) open_folder(it);
        else if (!strcmp(it->type, "Audio")) {   /* tracks play straight away */
            E.cur = *it;
            E.play_page = E.depth - 1;
            E.play_index = p->sel;
            ask_poster(it);
            start_playback(it, true);
        } else if (emby_is_playable(it)) {
            E.play_page = -1;
            open_detail(it, p->thumbs[p->sel]);
        }
    }
}

static void detail_key(uint32_t k)
{
    if (k == LV_KEY_ESC || k == '`' || k == LV_KEY_BACKSPACE) {
        set_view(V_LIST);
        list_render();
    } else if (k == LV_KEY_ENTER || k == ' ' || k == 'p') start_playback(&E.cur, false);
    else if (k == 'r') start_playback(&E.cur, true);
    else if (k == 'q') {
        E.cfg.quality = (E.cfg.quality + 1) % emby_quality_count;
        emby_cfg_save(&E.cfg);
        detail_render();
    } else if (k == LV_KEY_DOWN) lv_obj_scroll_by_bounded(E.d_over, 0, -24, LV_ANIM_ON);
    else if (k == LV_KEY_UP) lv_obj_scroll_by_bounded(E.d_over, 0, 24, LV_ANIM_ON);
}

static void player_key(uint32_t k)
{
    if (k == ' ' || k == LV_KEY_ENTER || k == 'p') zos_player_toggle_pause();
    else if (k == LV_KEY_RIGHT) zos_player_seek(10);
    else if (k == LV_KEY_LEFT) zos_player_seek(-10);
    else if (k == LV_KEY_UP) zos_player_seek(60);
    else if (k == LV_KEY_DOWN) zos_player_seek(-60);
    else if (k == '+' || k == '=') zos_player_volume(5);
    else if (k == '-') zos_player_volume(-5);
    else if (k == LV_KEY_ESC || k == 'q' || k == '`' || k == LV_KEY_BACKSPACE) {
        E.play_page = -1;   /* no auto-advance after a deliberate stop */
        zos_player_stop();
    }
}

static void sink_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (E.view) {
    case V_LIST: list_key(k); break;
    case V_DETAIL: detail_key(k); break;
    case V_PLAYER: player_key(k); break;
    default: break;
    }
}

static void set_view(view_t v)
{
    E.view = v;
    if (v == V_LIST) lv_obj_remove_flag(E.list_view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(E.list_view, LV_OBJ_FLAG_HIDDEN);
    if (v == V_DETAIL) lv_obj_remove_flag(E.detail_view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(E.detail_view, LV_OBJ_FLAG_HIDDEN);
    if (v == V_PLAYER) lv_obj_remove_flag(E.player_view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(E.player_view, LV_OBJ_FLAG_HIDDEN);
    if (v != V_LOGIN) {
        lv_group_remove_all_objs(E.g);
        lv_group_add_obj(E.g, E.sink);
        lv_group_focus_obj(E.sink);
    }
}

/* ======================= build ======================= */

static lv_obj_t *hint(lv_obj_t *p)
{
    lv_obj_t *h = label(p, &lv_font_montserrat_10, 0x5b6472);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -2);
    return h;
}

static void build(lv_obj_t *scr)
{
    /* list */
    E.list_view = page_obj(scr);
    lv_obj_t *sb = zos_statusbar_create(E.list_view, "");
    E.list_title = lv_obj_get_child(sb, 0);
    E.list = lv_obj_create(E.list_view);
    lv_obj_remove_style_all(E.list);
    lv_obj_set_pos(E.list, 0, ZOS_STATUSBAR_H);
    lv_obj_set_size(E.list, ZOS_HOR_RES, ZOS_VER_RES - ZOS_STATUSBAR_H - 13);
    lv_obj_set_style_pad_hor(E.list, 4, 0);
    lv_obj_set_style_pad_row(E.list, 1, 0);
    lv_obj_set_flex_flow(E.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(E.list, LV_SCROLLBAR_MODE_OFF);
    E.list_msg = label(E.list_view, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_set_width(E.list_msg, 300);
    lv_label_set_long_mode(E.list_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(E.list_msg, LV_ALIGN_CENTER, 0, 0);
    E.list_hint = hint(E.list_view);

    /* detail */
    E.detail_view = page_obj(scr);
    zos_statusbar_create(E.detail_view, "Emby");
    lv_obj_t *frame = box(E.detail_view);
    lv_obj_set_size(frame, 68, 100);
    lv_obj_set_pos(frame, 6, ZOS_STATUSBAR_H + 4);
    lv_obj_set_style_radius(frame, 5, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x232a33), 0);
    E.d_poster = lv_image_create(frame);
    lv_obj_center(E.d_poster);
    E.d_title = label(E.detail_view, zos_font.title, ZOS_COLOR_TEXT);
    lv_obj_set_width(E.d_title, 234);
    lv_label_set_long_mode(E.d_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(E.d_title, 20);
    lv_obj_set_pos(E.d_title, 82, ZOS_STATUSBAR_H + 2);
    E.d_meta = label(E.detail_view, zos_font.small, ACCENT);
    zos_label_one_line(E.d_meta, 234);
    lv_obj_set_pos(E.d_meta, 82, ZOS_STATUSBAR_H + 23);
    E.d_over = label(E.detail_view, zos_font.small, ZOS_COLOR_TEXT);
    lv_obj_set_size(E.d_over, 232, 74);
    lv_label_set_long_mode(E.d_over, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(E.d_over, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(E.d_over, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(E.d_over, 82, ZOS_STATUSBAR_H + 40);
    E.d_resume = label(E.detail_view, zos_font.small, ZOS_COLOR_ACCENT);
    lv_obj_set_pos(E.d_resume, 8, ZOS_STATUSBAR_H + 108);
    E.d_hint = hint(E.detail_view);

    /* player */
    E.player_view = page_obj(scr);
    zos_statusbar_create(E.player_view, "Now playing");
    lv_obj_t *pf = box(E.player_view);
    lv_obj_set_size(pf, 68, 100);
    lv_obj_set_pos(pf, 6, ZOS_STATUSBAR_H + 4);
    lv_obj_set_style_radius(pf, 5, 0);
    lv_obj_set_style_clip_corner(pf, true, 0);
    lv_obj_set_style_bg_opa(pf, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pf, lv_color_hex(0x232a33), 0);
    E.p_poster = lv_image_create(pf);
    lv_obj_center(E.p_poster);
    E.p_title = label(E.player_view, zos_font.title, ZOS_COLOR_TEXT);
    zos_label_one_line(E.p_title, 234);
    lv_obj_set_pos(E.p_title, 82, ZOS_STATUSBAR_H + 6);
    E.p_sub = label(E.player_view, zos_font.small, ZOS_COLOR_MUTED);
    zos_label_one_line(E.p_sub, 234);
    lv_obj_set_pos(E.p_sub, 82, ZOS_STATUSBAR_H + 28);
    E.p_bar = lv_bar_create(E.player_view);
    lv_obj_set_size(E.p_bar, 228, 6);
    lv_bar_set_range(E.p_bar, 0, 1000);
    lv_obj_set_pos(E.p_bar, 82, ZOS_STATUSBAR_H + 56);
    lv_obj_set_style_bg_color(E.p_bar, lv_color_hex(0x2a313b), 0);
    lv_obj_set_style_bg_color(E.p_bar, lv_color_hex(ACCENT), LV_PART_INDICATOR);
    E.p_time = label(E.player_view, zos_font.body, ZOS_COLOR_TEXT);
    lv_obj_set_pos(E.p_time, 82, ZOS_STATUSBAR_H + 66);
    E.p_state = label(E.player_view, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_set_pos(E.p_state, 82, ZOS_STATUSBAR_H + 88);
    lv_obj_t *ph = hint(E.player_view);
    lv_label_set_text(ph, "Space pause   " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " 10 s   " LV_SYMBOL_UP LV_SYMBOL_DOWN
                          " 1 min   +/- volume   Esc stop");

    E.sink = box(scr);
    lv_obj_set_size(E.sink, 1, 1);
    lv_obj_add_event_cb(E.sink, sink_key, LV_EVENT_KEY, NULL);
}

static void emby_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&E, 0, sizeof E);
    E.scr = scr;
    E.g = g;
    E.play_page = -1;
    build(scr);
    if (emby_cfg_load(&E.cfg)) {
        E.depth = 1;
        E.pages[0].home = true;
        load_page(&E.pages[0]);
        set_view(V_LIST);
        list_render();
    } else {
        open_login(NULL);
    }
}

static void emby_close(void)
{
    if (zos_player_state()->running) zos_player_stop();   /* reports "Stopped" */
    zos_task_cancel(&owner_tag);
    for (int i = 0; i < MAX_DEPTH; i++) page_free(&E.pages[i]);
    memset(&E, 0, sizeof E);
}

const zos_app_t zos_app_emby = {
    .id = "emby", .name = "Emby", .tagline = "Your media server, in your pocket",
    .icon = &zos_icon_media, .color = 0x3a8f3a, .open = emby_open, .close = emby_close,
};
