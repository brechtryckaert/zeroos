/* Music: browse ~/Music by artist, album or song, play with a queue,
 * shuffle and repeat. Playback carries on in the background when you leave
 * the app; the status bar shows a note while it does. */
#include <stdio.h>
#include <string.h>
#include "../../core/player.h"
#include "../../core/settings.h"
#include "../../core/ui.h"
#include "../../zos.h"
#include "music.h"

extern const lv_image_dsc_t zos_icon_music;
extern const zos_app_t zos_app_music;

typedef enum { V_HOME, V_ARTISTS, V_ARTIST, V_ALBUMS, V_ALBUM, V_SONGS, V_QUEUE, V_NOW } view_t;

#define MAX_DEPTH 8
#define ROW_H 28
#define LIST_Y ZOS_STATUSBAR_H
#define LIST_H (ZOS_VER_RES - ZOS_STATUSBAR_H - 12)

typedef struct {
    view_t v;
    int param;      /* artist or album index */
    int sel;
} level_t;

enum { H_NOW, H_SHUFFLE, H_ARTISTS, H_ALBUMS, H_SONGS, H_RESCAN, H_COUNT };

static struct {
    lv_obj_t *scr, *sink;
    lv_obj_t *list_page, *list_title, *list_hint;
    zos_vlist_t *list;
    lv_obj_t *now_page, *cover_box, *cover, *cover_sym, *n_title, *n_artist, *n_album, *n_bar,
             *n_pos, *n_left, *n_state;
    level_t stack[MAX_DEPTH];
    int depth;
    int home_map[H_COUNT], home_n;
    lv_timer_t *tick;
} M;

static level_t *top(void) { return &M.stack[M.depth - 1]; }
static void render(void);

/* ---------------- list contents ---------------- */

static void fill_home(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    const mlib_t *L = music_lib();
    switch (M.home_map[i]) {
    case H_NOW: {
        int c = music_current();
        r->symbol = LV_SYMBOL_PLAY;
        r->symbol_color = ZOS_COLOR_ACCENT;
        snprintf(r->title, sizeof r->title, "Now playing");
        if (c >= 0) snprintf(r->sub, sizeof r->sub, "%s \xc2\xb7 %s", L->t[c].title, L->t[c].artist);
        break;
    }
    case H_SHUFFLE:
        r->symbol = LV_SYMBOL_SHUFFLE;
        snprintf(r->title, sizeof r->title, "Shuffle all");
        snprintf(r->sub, sizeof r->sub, "%d songs", L->nt);
        break;
    case H_ARTISTS:
        r->symbol = LV_SYMBOL_AUDIO;
        snprintf(r->title, sizeof r->title, "Artists");
        snprintf(r->right, sizeof r->right, "%d", L->nar);
        break;
    case H_ALBUMS:
        r->symbol = LV_SYMBOL_IMAGE;
        snprintf(r->title, sizeof r->title, "Albums");
        snprintf(r->right, sizeof r->right, "%d", L->nal);
        break;
    case H_SONGS:
        r->symbol = LV_SYMBOL_LIST;
        snprintf(r->title, sizeof r->title, "Songs");
        snprintf(r->right, sizeof r->right, "%d", L->nt);
        break;
    case H_RESCAN:
        r->symbol = LV_SYMBOL_REFRESH;
        snprintf(r->title, sizeof r->title, music_scanning() ? "Scanning\xe2\x80\xa6" : "Rescan library");
        if (!L->nt && !music_scanning())
            snprintf(r->sub, sizeof r->sub, "No songs yet: copy music to %s", zos_cfg.music_dir);
        else
            snprintf(r->sub, sizeof r->sub, "%s", zos_cfg.music_dir);
        break;
    }
}

static void fill_artists(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    const martist_t *a = &music_lib()->ar[i];
    r->symbol = LV_SYMBOL_AUDIO;
    snprintf(r->title, sizeof r->title, "%s", a->name);
    snprintf(r->sub, sizeof r->sub, "%d album%s \xc2\xb7 %d song%s", a->na, a->na == 1 ? "" : "s",
             a->ntracks, a->ntracks == 1 ? "" : "s");
}

static void album_row(int a, zos_vrow_t *r)
{
    const malbum_t *al = &music_lib()->al[a];
    r->img = music_cover(a, 26);
    r->symbol = LV_SYMBOL_AUDIO;
    snprintf(r->title, sizeof r->title, "%s", al->name);
    if (al->year) snprintf(r->sub, sizeof r->sub, "%s \xc2\xb7 %d", al->artist, al->year);
    else snprintf(r->sub, sizeof r->sub, "%s", al->artist);
    snprintf(r->right, sizeof r->right, "%d", al->n);
}

static void fill_albums(int i, zos_vrow_t *r, void *ud) { LV_UNUSED(ud); album_row(i, r); }

/* artist page: "All songs", then the artist's albums */
static void fill_artist(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    const martist_t *a = &music_lib()->ar[top()->param];
    if (i == 0) {
        r->symbol = LV_SYMBOL_PLAY;
        snprintf(r->title, sizeof r->title, "All songs");
        snprintf(r->right, sizeof r->right, "%d", a->ntracks);
        return;
    }
    album_row(a->albums[i - 1], r);
}

static void track_row(int t, zos_vrow_t *r, bool with_album, bool numbered)
{
    const mtrack_t *tr = &music_lib()->t[t];
    r->symbol = LV_SYMBOL_AUDIO;
    if (numbered && tr->track) snprintf(r->title, sizeof r->title, "%d. %s", tr->track, tr->title);
    else snprintf(r->title, sizeof r->title, "%s", tr->title);
    if (with_album) snprintf(r->sub, sizeof r->sub, "%s \xc2\xb7 %s", tr->artist, tr->album);
    else snprintf(r->sub, sizeof r->sub, "%s", tr->artist);
    if (t == music_current()) {
        r->title_color = ZOS_COLOR_ACCENT;
        r->symbol = LV_SYMBOL_VOLUME_MAX;
        r->symbol_color = ZOS_COLOR_ACCENT;
        snprintf(r->right, sizeof r->right, zos_player_state()->paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

static void fill_album(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    const malbum_t *al = &music_lib()->al[top()->param];
    track_row(al->tracks[i], r, false, true);
}

static void fill_songs(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    const level_t *lv = top();
    const mlib_t *L = music_lib();
    if (lv->param >= 0) {   /* one artist's songs */
        const martist_t *a = &L->ar[lv->param];
        int k = i;
        for (int x = 0; x < a->na; x++) {
            const malbum_t *al = &L->al[a->albums[x]];
            if (k < al->n) { track_row(al->tracks[k], r, true, false); return; }
            k -= al->n;
        }
        return;
    }
    track_row(L->songs[i], r, true, false);
}

static void fill_queue(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    int n, pos;
    const int *q = music_queue(&n, &pos);
    track_row(q[i], r, true, false);
    if (i < pos) r->title_color = ZOS_COLOR_MUTED;
}

/* collect the tracks of the songs list in the current level */
static int collect_songs(int *out)
{
    const mlib_t *L = music_lib();
    const level_t *lv = top();
    if (lv->param < 0) {
        memcpy(out, L->songs, (size_t)L->nt * sizeof(int));
        return L->nt;
    }
    const martist_t *a = &L->ar[lv->param];
    int k = 0;
    for (int x = 0; x < a->na; x++) {
        const malbum_t *al = &L->al[a->albums[x]];
        for (int j = 0; j < al->n; j++) out[k++] = al->tracks[j];
    }
    return k;
}

/* ---------------- views ---------------- */

static void push(view_t v, int param)
{
    if (M.depth >= MAX_DEPTH) return;
    if (M.depth) top()->sel = zos_vlist_selected(M.list);
    M.stack[M.depth++] = (level_t){.v = v, .param = param, .sel = 0};
    render();
}

static void pop(void)
{
    if (M.depth <= 1) { zos_app_exit(); return; }
    M.depth--;
    render();
}

static void render_now(void)
{
    const mlib_t *L = music_lib();
    int c = music_current();
    const zos_player_state_t *s = zos_player_state();
    if (c < 0) {
        lv_label_set_text(M.n_title, "Nothing playing");
        lv_label_set_text(M.n_artist, "");
        lv_label_set_text(M.n_album, "Pick something in the library.");
        lv_bar_set_value(M.n_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(M.n_pos, "");
        lv_label_set_text(M.n_left, "");
        lv_label_set_text(M.n_state, "");
        zos_ui_show(M.cover, false);
        zos_ui_show(M.cover_sym, true);
        return;
    }
    const mtrack_t *t = &L->t[c];
    lv_label_set_text(M.n_title, t->title);
    lv_label_set_text(M.n_artist, t->artist);
    char al[160];
    if (L->al[t->album_idx].year) snprintf(al, sizeof al, "%s \xc2\xb7 %d", t->album, L->al[t->album_idx].year);
    else snprintf(al, sizeof al, "%s", t->album);
    lv_label_set_text(M.n_album, al);
    const lv_image_dsc_t *img = music_cover(t->album_idx, 84);
    if (img) lv_image_set_src(M.cover, img);
    zos_ui_show(M.cover, img != NULL);
    zos_ui_show(M.cover_sym, img == NULL);

    char a[24], b[24];
    zos_ui_fmt_time(s->pos, a, sizeof a);
    lv_label_set_text(M.n_pos, a);
    if (s->dur > 0) {
        zos_ui_fmt_time(s->dur - s->pos, b + 1, sizeof b - 1);
        b[0] = '-';
        lv_label_set_text(M.n_left, b);
        lv_bar_set_value(M.n_bar, (int32_t)(1000 * s->pos / s->dur), LV_ANIM_OFF);
    } else {
        lv_label_set_text(M.n_left, "");
        lv_bar_set_value(M.n_bar, 0, LV_ANIM_OFF);
    }
    int n, pos;
    music_queue(&n, &pos);
    static const char *rep[] = {"", "  \xc2\xb7  " LV_SYMBOL_LOOP " all", "  \xc2\xb7  " LV_SYMBOL_LOOP " one"};
    char st[160];
    snprintf(st, sizeof st, "%s  %d of %d%s%s  \xc2\xb7  vol %d%%",
             s->buffering && !s->pos ? "Loading" : s->paused ? LV_SYMBOL_PAUSE " Paused" : LV_SYMBOL_PLAY " Playing",
             pos + 1, n, music_shuffle() ? "  \xc2\xb7  " LV_SYMBOL_SHUFFLE : "", rep[music_repeat()],
             zos_setting_int("volume", 80));
    lv_label_set_text(M.n_state, st);
}

static void render(void)
{
    level_t *lv = top();
    bool now = lv->v == V_NOW;
    zos_ui_show(M.now_page, now);
    zos_ui_show(M.list_page, !now);
    if (now) { render_now(); return; }

    const mlib_t *L = music_lib();
    const char *hint = LV_SYMBOL_NEW_LINE " play   S shuffle   Tab now playing   Space pause   Esc back";
    char title[160];
    int count = 0;
    zos_vlist_fill_cb fill = NULL;
    switch (lv->v) {
    case V_HOME:
        M.home_n = 0;
        if (music_active()) M.home_map[M.home_n++] = H_NOW;
        if (L->nt) {
            M.home_map[M.home_n++] = H_SHUFFLE;
            M.home_map[M.home_n++] = H_ARTISTS;
            M.home_map[M.home_n++] = H_ALBUMS;
            M.home_map[M.home_n++] = H_SONGS;
        }
        M.home_map[M.home_n++] = H_RESCAN;
        count = M.home_n;
        fill = fill_home;
        snprintf(title, sizeof title, "Music");
        hint = LV_SYMBOL_NEW_LINE " open   Tab now playing   Space pause   Esc home";
        zos_vlist_set_empty_text(M.list, "");
        break;
    case V_ARTISTS:
        count = L->nar; fill = fill_artists; snprintf(title, sizeof title, "Artists");
        hint = LV_SYMBOL_NEW_LINE " open   S shuffle artist   Tab now playing   Esc back";
        break;
    case V_ARTIST:
        count = L->ar[lv->param].na + 1; fill = fill_artist;
        snprintf(title, sizeof title, "%s", L->ar[lv->param].name);
        break;
    case V_ALBUMS:
        count = L->nal; fill = fill_albums; snprintf(title, sizeof title, "Albums");
        hint = LV_SYMBOL_NEW_LINE " open   S shuffle album   Tab now playing   Esc back";
        break;
    case V_ALBUM:
        count = L->al[lv->param].n; fill = fill_album;
        snprintf(title, sizeof title, "%s", L->al[lv->param].name);
        break;
    case V_SONGS:
        count = lv->param >= 0 ? L->ar[lv->param].ntracks : L->nt; fill = fill_songs;
        snprintf(title, sizeof title, lv->param >= 0 ? "%s" : "Songs", lv->param >= 0 ? L->ar[lv->param].name : "");
        break;
    case V_QUEUE: {
        int n, pos;
        music_queue(&n, &pos);
        count = n; fill = fill_queue; snprintf(title, sizeof title, "Queue");
        hint = LV_SYMBOL_NEW_LINE " jump to   Tab now playing   Esc back";
        break;
    }
    default: break;
    }
    lv_label_set_text(M.list_title, title);
    lv_label_set_text(M.list_hint, hint);
    if (lv->v != V_HOME)
        zos_vlist_set_empty_text(M.list, music_scanning() ? "Scanning your music\xe2\x80\xa6" : "Nothing here.");
    zos_vlist_set(M.list, 0, NULL, NULL);
    zos_vlist_select(M.list, lv->sel);
    zos_vlist_set(M.list, count, fill, NULL);
    zos_vlist_select(M.list, lv->sel);
}

/* ---------------- actions ---------------- */

static void open_now(void)
{
    if (top()->v == V_NOW) return;
    push(V_NOW, 0);
}

static void play_list(bool shuffle)
{
    const mlib_t *L = music_lib();
    level_t *lv = top();
    int i = zos_vlist_selected(M.list);
    static int buf[1];
    (void)buf;
    switch (lv->v) {
    case V_ALBUM:
        music_play(L->al[lv->param].tracks, L->al[lv->param].n, shuffle ? -1 : i, shuffle);
        break;
    case V_ALBUMS:
        if (i >= 0) music_play(L->al[i].tracks, L->al[i].n, shuffle ? -1 : 0, shuffle);
        break;
    case V_ARTIST:
    case V_ARTISTS: {
        int a = lv->v == V_ARTIST ? lv->param : i;
        if (a < 0) return;
        if (lv->v == V_ARTIST && i > 0 && !shuffle) {
            const malbum_t *al = &L->al[L->ar[a].albums[i - 1]];
            music_play(al->tracks, al->n, 0, false);
            break;
        }
        int *t = lv_malloc((size_t)L->ar[a].ntracks * sizeof(int));
        int k = 0;
        for (int x = 0; x < L->ar[a].na; x++) {
            const malbum_t *al = &L->al[L->ar[a].albums[x]];
            for (int j = 0; j < al->n; j++) t[k++] = al->tracks[j];
        }
        music_play(t, k, shuffle ? -1 : 0, shuffle);
        lv_free(t);
        break;
    }
    case V_SONGS: {
        int *t = lv_malloc((size_t)(L->nt ? L->nt : 1) * sizeof(int));
        int k = collect_songs(t);
        music_play(t, k, shuffle ? -1 : i, shuffle);
        lv_free(t);
        break;
    }
    default: return;
    }
    open_now();
}

static void activate(void)
{
    level_t *lv = top();
    int i = zos_vlist_selected(M.list);
    if (i < 0) return;
    const mlib_t *L = music_lib();
    switch (lv->v) {
    case V_HOME:
        switch (M.home_map[i]) {
        case H_NOW: open_now(); break;
        case H_SHUFFLE: music_play(L->songs, L->nt, -1, true); open_now(); break;
        case H_ARTISTS: push(V_ARTISTS, 0); break;
        case H_ALBUMS: push(V_ALBUMS, 0); break;
        case H_SONGS: push(V_SONGS, -1); break;
        case H_RESCAN: music_scan(render); render(); break;
        }
        break;
    case V_ARTISTS: push(V_ARTIST, i); break;
    case V_ARTIST:
        if (i == 0) push(V_SONGS, lv->param);
        else push(V_ALBUM, L->ar[lv->param].albums[i - 1]);
        break;
    case V_ALBUMS: push(V_ALBUM, i); break;
    case V_ALBUM:
    case V_SONGS: play_list(false); break;
    case V_QUEUE: music_jump(i); open_now(); break;
    default: break;
    }
}

static void now_key(uint32_t k)
{
    switch (k) {
    case ' ': case LV_KEY_ENTER: if (music_active()) zos_player_toggle_pause(); break;
    case LV_KEY_LEFT: zos_player_seek(-10); break;
    case LV_KEY_RIGHT: zos_player_seek(10); break;
    case 'n': case 'N': case LV_KEY_DOWN: music_next(); break;
    case 'p': case 'P': case LV_KEY_UP: music_prev(); break;
    case '+': case '=': music_volume(5); break;
    case '-': case '_': music_volume(-5); break;
    case 's': case 'S': music_toggle_shuffle(); break;
    case 'r': case 'R': music_cycle_repeat(); break;
    case 'q': case 'Q': if (music_active()) push(V_QUEUE, 0); return;
    case 'x': case 'X': music_stop(); break;
    case LV_KEY_ESC: case LV_KEY_BACKSPACE: case '`': pop(); return;
    default: return;
    }
    render_now();
}

static void sink_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (top()->v == V_NOW) { now_key(k); return; }
    if (zos_vlist_key(M.list, k)) return;
    switch (k) {
    case LV_KEY_ENTER: case LV_KEY_RIGHT: activate(); break;
    case 's': case 'S': play_list(true); break;
    case ZOS_KEY_TAB: case '\t': open_now(); break;
    case ' ': if (music_active()) zos_player_toggle_pause(); zos_vlist_refresh(M.list); break;
    case LV_KEY_ESC: case LV_KEY_BACKSPACE: case LV_KEY_LEFT: case '`': pop(); break;
    default: break;
    }
}

/* ---------------- updates ---------------- */

static void on_music_change(void)
{
    if (!M.scr) return;
    if (top()->v == V_NOW) render_now();
    else if (top()->v == V_HOME) render();
    else zos_vlist_refresh(M.list);
}

static void on_cover(void)
{
    if (!M.scr) return;
    if (top()->v == V_NOW) render_now();
    else zos_vlist_refresh(M.list);
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (top()->v == V_NOW) render_now();
}

/* ---------------- build ---------------- */

static void build(lv_obj_t *scr)
{
    M.list_page = zos_ui_page(scr);
    lv_obj_t *sb = zos_statusbar_create(M.list_page, "Music");
    M.list_title = lv_obj_get_child(sb, 0);
    M.list = zos_vlist_create(M.list_page, 2, LIST_Y, ZOS_HOR_RES - 4, LIST_H, ROW_H, 26);
    M.list_hint = zos_ui_hint(M.list_page, "");

    M.now_page = zos_ui_page(scr);
    zos_statusbar_create(M.now_page, "Now playing");
    M.cover_box = zos_ui_box(M.now_page);
    lv_obj_set_size(M.cover_box, 84, 84);
    lv_obj_set_pos(M.cover_box, 8, ZOS_STATUSBAR_H + 6);
    lv_obj_set_style_radius(M.cover_box, 6, 0);
    lv_obj_set_style_clip_corner(M.cover_box, true, 0);
    lv_obj_set_style_bg_opa(M.cover_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(M.cover_box, lv_color_hex(0x2a1d22), 0);
    M.cover = lv_image_create(M.cover_box);
    lv_obj_center(M.cover);
    M.cover_sym = zos_ui_label(M.cover_box, &lv_font_montserrat_20, 0xe5484d);
    lv_label_set_text(M.cover_sym, LV_SYMBOL_AUDIO);
    lv_obj_center(M.cover_sym);

    int32_t x = 102, w = ZOS_HOR_RES - x - 8;
    M.n_title = zos_ui_label(M.now_page, zos_font.title, ZOS_COLOR_TEXT);
    zos_label_one_line(M.n_title, w);
    lv_obj_set_pos(M.n_title, x, ZOS_STATUSBAR_H + 6);
    M.n_artist = zos_ui_label(M.now_page, zos_font.body, ZOS_COLOR_TEXT);
    zos_label_one_line(M.n_artist, w);
    lv_obj_set_pos(M.n_artist, x, ZOS_STATUSBAR_H + 28);
    M.n_album = zos_ui_label(M.now_page, zos_font.small, ZOS_COLOR_MUTED);
    zos_label_one_line(M.n_album, w);
    lv_obj_set_pos(M.n_album, x, ZOS_STATUSBAR_H + 47);
    M.n_bar = lv_bar_create(M.now_page);
    lv_obj_set_size(M.n_bar, w, 5);
    lv_bar_set_range(M.n_bar, 0, 1000);
    lv_obj_set_pos(M.n_bar, x, ZOS_STATUSBAR_H + 68);
    lv_obj_set_style_bg_color(M.n_bar, lv_color_hex(0x2a313b), 0);
    lv_obj_set_style_bg_color(M.n_bar, lv_color_hex(0xe5484d), LV_PART_INDICATOR);
    M.n_pos = zos_ui_label(M.now_page, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_set_pos(M.n_pos, x, ZOS_STATUSBAR_H + 76);
    M.n_left = zos_ui_label(M.now_page, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_align(M.n_left, LV_ALIGN_TOP_RIGHT, -8, ZOS_STATUSBAR_H + 76);
    M.n_state = zos_ui_label(M.now_page, zos_font.small, ZOS_COLOR_TEXT);
    zos_label_one_line(M.n_state, ZOS_HOR_RES - 16);
    lv_obj_set_pos(M.n_state, 8, ZOS_STATUSBAR_H + 98);
    zos_ui_hint(M.now_page, "Space pause  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " seek  " LV_SYMBOL_UP LV_SYMBOL_DOWN
                            " track  +/- vol  S shuffle  R repeat  Q list");

    M.sink = zos_ui_box(scr);
    lv_obj_set_size(M.sink, 1, 1);
    lv_obj_add_event_cb(M.sink, sink_key, LV_EVENT_KEY, NULL);
}

static void music_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&M, 0, sizeof M);
    M.scr = scr;
    build(scr);
    lv_group_add_obj(g, M.sink);
    lv_group_focus_obj(M.sink);
    music_set_change_cb(on_music_change);
    music_set_cover_ready_cb(on_cover);
    M.stack[0] = (level_t){.v = V_HOME};
    M.depth = 1;
    if (music_active()) M.stack[M.depth++] = (level_t){.v = V_NOW};
    if (!music_lib()->nt && !music_scanning()) music_scan(render);
    else if (!music_scanning()) music_scan(NULL);   /* pick up new files quietly */
    M.tick = lv_timer_create(tick_cb, 500, NULL);
    render();
}

static void music_close(void)
{
    music_set_change_cb(NULL);
    music_set_cover_ready_cb(NULL);
    lv_timer_delete(M.tick);
    memset(&M, 0, sizeof M);
}

const zos_app_t zos_app_music = {
    .id = "music", .name = "Music", .tagline = "Your songs, albums and artists",
    .icon = &zos_icon_music, .color = 0xe5484d, .open = music_open, .close = music_close,
};
