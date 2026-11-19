/* Video: browse ~/Videos and play MP4, MKV, WebM, AVI ... with mpv.
 *
 * Subtitles and audio tracks inside the file (MKV usually has several) and
 * .srt/.ass files next to it can be picked while playing. Where you stopped
 * is remembered per file. With a monitor on HDMI, video plays there (with
 * HDMI audio) and this screen becomes the remote control. */
#include <cjson/cJSON.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "../../core/hdmi.h"
#include "../../core/player.h"
#include "../../core/settings.h"
#include "../../core/ui.h"
#include "../../zos.h"

extern const lv_image_dsc_t zos_icon_video;
extern const zos_app_t zos_app_video;

typedef enum { V_BROWSE, V_RESUME, V_PLAYER, V_TRACKS } view_t;

#define MAX_DEPTH 10

typedef struct {
    char *name;
    bool dir;
    long long size;
    int items;          /* for folders */
} entry_t;

typedef struct {
    char *path;
    int sel;
} level_t;

static struct {
    lv_obj_t *scr, *sink;
    lv_obj_t *browse_page, *b_title, *b_hint;
    zos_vlist_t *list;
    lv_obj_t *resume_box, *resume_lbl;
    lv_obj_t *player_page, *p_title, *p_where, *p_bar, *p_time, *p_state, *p_tracks;
    lv_obj_t *tracks_page, *t_title;
    zos_vlist_t *tlist;
    view_t view;
    level_t stack[MAX_DEPTH];
    int depth;
    entry_t *e;
    int n;
    cJSON *pos;             /* path -> {p: seconds, d: duration, done: bool} */
    char playing[1024];
    char playing_name[256];
    uint32_t last_save;
    const char *tracks_type;   /* "sub" or "audio" */
    int track_ids[ZOS_PLAYER_MAX_TRACKS + 1];
    int ntrack_rows;
    bool switching;
} V;

static void render_browse(void);
static void set_view(view_t v);

/* ---------------- positions ---------------- */

static void pos_path(char *out, size_t n) { snprintf(out, n, "%s/video-positions.json", zos_cfg.state_dir); }

static void pos_load(void)
{
    char p[512];
    pos_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        char *b = NULL;
        size_t cap = 0, len = 0;
        char chunk[4096];
        size_t r;
        while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
            if (len + r + 1 > cap) { cap = (len + r + 1) * 2; b = realloc(b, cap); }
            memcpy(b + len, chunk, r);
            len += r;
        }
        fclose(f);
        if (b) { b[len] = 0; V.pos = cJSON_Parse(b); free(b); }
    }
    if (!V.pos) V.pos = cJSON_CreateObject();
}

static void pos_save(void)
{
    char p[512], tmp[520];
    pos_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    char *txt = cJSON_PrintUnformatted(V.pos);
    FILE *f = fopen(tmp, "w");
    if (f) { fputs(txt, f); fclose(f); rename(tmp, p); }
    free(txt);
}

static void pos_set(const char *path, double at, double dur, bool done)
{
    cJSON_DeleteItemFromObject(V.pos, path);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "p", done ? 0 : at);
    cJSON_AddNumberToObject(o, "d", dur);
    cJSON_AddBoolToObject(o, "done", done);
    cJSON_AddItemToObject(V.pos, path, o);
}

static void pos_get(const char *path, double *at, double *dur, bool *done)
{
    *at = *dur = 0;
    *done = false;
    const cJSON *o = cJSON_GetObjectItem(V.pos, path);
    if (!o) return;
    const cJSON *p = cJSON_GetObjectItem(o, "p"), *d = cJSON_GetObjectItem(o, "d");
    if (cJSON_IsNumber(p)) *at = p->valuedouble;
    if (cJSON_IsNumber(d)) *dur = d->valuedouble;
    *done = cJSON_IsTrue(cJSON_GetObjectItem(o, "done"));
}

/* ---------------- folder listing ---------------- */

static bool is_video(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *ext[] = {".mp4", ".mkv", ".m4v", ".webm", ".avi", ".mov", ".ts", ".m2ts", ".mpg", ".mpeg", ".wmv", ".ogv", ".flv", ".3gp"};
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (!strcasecmp(dot, ext[i])) return true;
    return false;
}

static int count_items(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)))
        if (de->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void free_entries(void)
{
    for (int i = 0; i < V.n; i++) free(V.e[i].name);
    free(V.e);
    V.e = NULL;
    V.n = 0;
}

static const char *cur_dir(void) { return V.stack[V.depth - 1].path; }

static void load_dir(void)
{
    free_entries();
    DIR *d = opendir(cur_dir());
    if (!d) return;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *p = zos_path_join(cur_dir(), de->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && is_video(de->d_name)))) {
            if (V.n == cap) { cap = cap ? cap * 2 : 32; V.e = realloc(V.e, (size_t)cap * sizeof *V.e); }
            entry_t *e = &V.e[V.n++];
            e->name = strdup(de->d_name);
            e->dir = S_ISDIR(st.st_mode);
            e->size = (long long)st.st_size;
            e->items = e->dir ? count_items(p) : 0;
        }
        lv_free(p);
    }
    closedir(d);
    qsort(V.e, (size_t)V.n, sizeof *V.e, cmp_entry);
}

static void fill_browse(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    entry_t *e = &V.e[i];
    if (e->dir) {
        r->symbol = LV_SYMBOL_DIRECTORY;
        r->symbol_color = 0xc4b5fd;
        snprintf(r->title, sizeof r->title, "%s", e->name);
        snprintf(r->sub, sizeof r->sub, "%d item%s", e->items, e->items == 1 ? "" : "s");
        return;
    }
    r->symbol = LV_SYMBOL_VIDEO;
    r->symbol_color = 0x8e4ec6;
    const char *dot = strrchr(e->name, '.');
    snprintf(r->title, sizeof r->title, "%.*s", dot ? (int)(dot - e->name) : (int)strlen(e->name), e->name);
    char size[24], ext[12] = "";
    zos_ui_fmt_size((double)e->size, size, sizeof size);
    if (dot) {
        snprintf(ext, sizeof ext, "%s", dot + 1);
        for (char *c = ext; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    }
    char *path = zos_path_join(cur_dir(), e->name);
    double at, dur;
    bool done;
    pos_get(path, &at, &dur, &done);
    lv_free(path);
    if (done) {
        snprintf(r->sub, sizeof r->sub, "%s \xc2\xb7 %s \xc2\xb7 \xe2\x9c\x93 watched", ext, size);
        r->title_color = ZOS_COLOR_MUTED;
    } else if (at > 10 && dur > 0) {
        char t[24];
        zos_ui_fmt_time(at, t, sizeof t);
        snprintf(r->sub, sizeof r->sub, "%s \xc2\xb7 %s \xc2\xb7 stopped at %s (%d%%)", ext, size, t, (int)(100 * at / dur));
    } else {
        snprintf(r->sub, sizeof r->sub, "%s \xc2\xb7 %s", ext, size);
    }
}

static void render_browse(void)
{
    const char *base = strrchr(cur_dir(), '/');
    lv_label_set_text(V.b_title, V.depth == 1 ? "Videos" : base ? base + 1 : cur_dir());
    char empty[600];
    snprintf(empty, sizeof empty, V.depth == 1 ? "No videos yet.\nCopy MP4 or MKV files to\n%s" : "Empty folder.", cur_dir());
    zos_vlist_set_empty_text(V.list, empty);
    zos_vlist_set(V.list, V.n, fill_browse, NULL);
    zos_vlist_select(V.list, V.stack[V.depth - 1].sel);
    bool tv = zos_player_video_target() == ZOS_OUT_HDMI;
    char hint[200];
    snprintf(hint, sizeof hint, LV_SYMBOL_NEW_LINE " play   O output: %s   Del forget position   Esc back",
             tv ? "TV (HDMI)" : zos_hdmi_connected() ? "this screen" : "screen");
    lv_label_set_text(V.b_hint, hint);
}

/* ---------------- playback ---------------- */

static void render_player(void)
{
    const zos_player_state_t *s = zos_player_state();
    lv_label_set_text(V.p_title, V.playing_name);
    lv_label_set_text(V.p_where, s->on_hdmi ? LV_SYMBOL_VIDEO "  Playing on the TV (HDMI)"
                                 : zos_on_device() ? "Playing on this screen" : "Playing in the mpv window");
    char a[24], b[24], t[64];
    zos_ui_fmt_time(s->pos, a, sizeof a);
    zos_ui_fmt_time(s->dur, b, sizeof b);
    snprintf(t, sizeof t, "%s / %s", a, b);
    lv_label_set_text(V.p_time, t);
    lv_bar_set_value(V.p_bar, s->dur > 0 ? (int32_t)(1000 * s->pos / s->dur) : 0, LV_ANIM_OFF);
    char st[96];
    snprintf(st, sizeof st, "%s  \xc2\xb7  vol %d%%", !s->running ? "Starting\xe2\x80\xa6" : s->buffering && !s->pos ? "Loading\xe2\x80\xa6"
             : s->paused ? LV_SYMBOL_PAUSE " Paused" : LV_SYMBOL_PLAY " Playing", s->volume);
    lv_label_set_text(V.p_state, st);
    /* current audio / subtitle track */
    char tr[200] = "";
    const char *aname = "", *sname = "Off";
    static char abuf[80], sbuf[80];
    int na = 0, ns = 0;
    for (int i = 0; i < s->ntracks; i++) {
        const zos_track_t *k = &s->tracks[i];
        const char *nm = k->title[0] ? k->title : k->lang[0] ? k->lang : k->codec;
        if (!strcmp(k->type, "audio")) { na++; if (k->selected) { snprintf(abuf, sizeof abuf, "%s", nm); aname = abuf; } }
        if (!strcmp(k->type, "sub")) { ns++; if (k->selected) { snprintf(sbuf, sizeof sbuf, "%s%s", nm, k->external ? " (file)" : ""); sname = sbuf; } }
    }
    snprintf(tr, sizeof tr, "Audio: %s%s   Subtitles: %s%s", aname[0] ? aname : "-", na > 1 ? " \xe2\x96\xb8" : "",
             ns ? sname : "none", ns ? " \xe2\x96\xb8" : "");
    lv_label_set_text(V.p_tracks, tr);
}

static void save_progress(bool final, bool eof)
{
    const zos_player_state_t *s = zos_player_state();
    if (!V.playing[0]) return;
    double dur = s->dur;
    bool done = eof || (dur > 0 && s->pos > dur * 0.95);
    pos_set(V.playing, s->pos, dur, done);
    if (final || lv_tick_get() - V.last_save > 15000) {
        pos_save();
        V.last_save = lv_tick_get();
    }
}

static void on_change(void *ud)
{
    LV_UNUSED(ud);
    if (!V.scr) return;
    save_progress(false, false);
    if (V.view == V_PLAYER) render_player();
}

#define HELP_TEXT "Space pause    \xe2\x86\x90 \xe2\x86\x92 10 s    [ ] 1 min\n" \
                  "\xe2\x86\x91 \xe2\x86\x93 volume    M mute    N / P next / previous\n" \
                  "S subtitles    A audio    H these keys    Esc stop"

static void play(double from);

/* Index of the next (dir 1) or previous (-1) video in the folder, or -1. */
static int neighbour(int dir)
{
    const char *name = strrchr(V.playing, '/');
    name = name ? name + 1 : V.playing;
    int cur = -1;
    for (int i = 0; i < V.n; i++)
        if (!V.e[i].dir && !strcmp(V.e[i].name, name)) cur = i;
    if (cur < 0) return -1;
    for (int i = cur + dir; i >= 0 && i < V.n; i += dir)
        if (!V.e[i].dir) return i;
    return -1;
}

static void set_playing(int idx)
{
    entry_t *e = &V.e[idx];
    char *p = zos_path_join(cur_dir(), e->name);
    snprintf(V.playing, sizeof V.playing, "%s", p);
    lv_free(p);
    const char *dot = strrchr(e->name, '.');
    snprintf(V.playing_name, sizeof V.playing_name, "%.*s", dot ? (int)(dot - e->name) : (int)strlen(e->name), e->name);
    zos_vlist_select(V.list, idx);
}

static double resume_point(void)
{
    double at, dur;
    bool done;
    pos_get(V.playing, &at, &dur, &done);
    return at > 10 && !done ? at - 3 : 0;
}

/* N / P while playing: save this one, play the next or previous file. */
static void skip(int dir)
{
    int idx = neighbour(dir);
    if (idx < 0) {
        zos_player_osd(dir > 0 ? "Last video in this folder" : "First video in this folder", 2000);
        return;
    }
    V.switching = true;
    zos_player_stop();            /* on_end saves the position */
    V.switching = false;
    set_playing(idx);
    play(resume_point());
}

static void on_end(bool eof, double last, void *ud)
{
    LV_UNUSED(ud);
    if (!V.scr || !V.playing[0]) return;
    const zos_player_state_t *s = zos_player_state();
    double dur = s->dur;
    bool done = eof || (dur > 0 && last > dur * 0.95);
    pos_set(V.playing, last, dur, done);
    pos_save();
    if (V.switching) return;
    /* the next episode, like a TV app */
    int next = eof && zos_setting_int("video_autoplay", 1) ? neighbour(1) : -1;
    if (next >= 0) {
        set_playing(next);
        play(resume_point());
        return;
    }
    V.playing[0] = 0;
    set_view(V_BROWSE);
    render_browse();
}

static void play(double from)
{
    zos_player_opts_t o = {.start = from, .video = true, .title = V.playing_name};
    zos_player_cbs_t cbs = {.on_change = on_change, .on_end = on_end};
    if (!zos_player_play(V.playing, &o, cbs, NULL)) {
        zos_notify(0xe5484d, "Video", "Could not start mpv");
        V.playing[0] = 0;
        return;
    }
    /* On the panel the video covers everything, so say which keys do what
     * on the video itself, once at the start (H shows it again). */
    char t[400];
    snprintf(t, sizeof t, "%s\n\n%s", V.playing_name, HELP_TEXT);
    zos_player_osd(t, 5000);
    V.last_save = lv_tick_get();
    set_view(V_PLAYER);
    render_player();
}

static void open_selected(void)
{
    int i = zos_vlist_selected(V.list);
    if (i < 0) return;
    entry_t *e = &V.e[i];
    char *p = zos_path_join(cur_dir(), e->name);
    if (e->dir) {
        if (V.depth < MAX_DEPTH) {
            V.stack[V.depth - 1].sel = i;
            V.stack[V.depth++] = (level_t){.path = strdup(p)};
            load_dir();
            render_browse();
        }
        lv_free(p);
        return;
    }
    snprintf(V.playing, sizeof V.playing, "%s", p);
    lv_free(p);
    const char *dot = strrchr(e->name, '.');
    snprintf(V.playing_name, sizeof V.playing_name, "%.*s", dot ? (int)(dot - e->name) : (int)strlen(e->name), e->name);
    double at, dur;
    bool done;
    pos_get(V.playing, &at, &dur, &done);
    if (at > 10 && !done) {
        char t[24], msg[200];
        zos_ui_fmt_time(at, t, sizeof t);
        snprintf(msg, sizeof msg, "Resume at %s?\n\n" LV_SYMBOL_NEW_LINE " resume    0 from start    Esc", t);
        lv_label_set_text(V.resume_lbl, msg);
        set_view(V_RESUME);
        return;
    }
    play(0);
}

/* ---------------- track picker ---------------- */

static void fill_tracks(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    const zos_player_state_t *s = zos_player_state();
    int id = V.track_ids[i];
    if (id < 0) {   /* "Off" */
        snprintf(r->title, sizeof r->title, "Off");
        bool any = false;
        for (int k = 0; k < s->ntracks; k++)
            if (!strcmp(s->tracks[k].type, "sub") && s->tracks[k].selected) any = true;
        if (!any) { r->title_color = ZOS_COLOR_ACCENT; snprintf(r->right, sizeof r->right, LV_SYMBOL_OK); }
        return;
    }
    for (int k = 0; k < s->ntracks; k++) {
        const zos_track_t *t = &s->tracks[k];
        if (strcmp(t->type, V.tracks_type) || t->id != id) continue;
        const char *nm = t->title[0] ? t->title : t->lang[0] ? t->lang : "Track";
        snprintf(r->title, sizeof r->title, "%d. %s", t->id, nm);
        snprintf(r->sub, sizeof r->sub, "%s%s%s%s%s", t->lang[0] && t->title[0] ? t->lang : "",
                 t->lang[0] && t->title[0] ? " \xc2\xb7 " : "", t->codec,
                 t->external ? " \xc2\xb7 file" : "", t->forced ? " \xc2\xb7 forced" : t->def ? " \xc2\xb7 default" : "");
        if (t->selected) { r->title_color = ZOS_COLOR_ACCENT; snprintf(r->right, sizeof r->right, LV_SYMBOL_OK); }
        return;
    }
}

static void open_tracks(const char *type)
{
    const zos_player_state_t *s = zos_player_state();
    V.tracks_type = type;
    V.ntrack_rows = 0;
    int sel = 0;
    if (!strcmp(type, "sub")) V.track_ids[V.ntrack_rows++] = -1;
    for (int k = 0; k < s->ntracks; k++)
        if (!strcmp(s->tracks[k].type, type)) {
            if (s->tracks[k].selected) sel = V.ntrack_rows;
            V.track_ids[V.ntrack_rows++] = s->tracks[k].id;
        }
    lv_label_set_text(V.t_title, !strcmp(type, "sub") ? "Subtitles" : "Audio track");
    zos_vlist_set_empty_text(V.tlist, "This video has no other tracks.");
    zos_vlist_set(V.tlist, V.ntrack_rows, fill_tracks, NULL);
    zos_vlist_select(V.tlist, sel);
    set_view(V_TRACKS);
}

static void pick_track(void)
{
    int i = zos_vlist_selected(V.tlist);
    if (i < 0) return;
    int id = V.track_ids[i];
    char v[16];
    snprintf(v, sizeof v, id < 0 ? "\"no\"" : "%d", id);
    bool sub = !strcmp(V.tracks_type, "sub");
    zos_player_set(sub ? "sid" : "aid", v);
    /* remember the language as the default for the next video */
    const zos_player_state_t *s = zos_player_state();
    for (int k = 0; k < s->ntracks; k++)
        if (s->tracks[k].id == id && !strcmp(s->tracks[k].type, V.tracks_type) && s->tracks[k].lang[0])
            zos_setting_set_str(sub ? "sub_lang" : "audio_lang", s->tracks[k].lang);
    if (sub && id < 0) zos_setting_set_str("sub_lang", "");
    set_view(V_PLAYER);
    render_player();
}

/* ---------------- keys ---------------- */

static void player_key(uint32_t k)
{
    /* With the video on this screen nothing of ours is visible, so track
     * keys cycle through tracks directly (mpv shows the choice). */
    bool blind = zos_on_device() && !zos_player_state()->on_hdmi;
    switch (k) {
    case ' ': case LV_KEY_ENTER: zos_player_toggle_pause(); break;
    case LV_KEY_LEFT: zos_player_seek(-10); break;
    case LV_KEY_RIGHT: zos_player_seek(10); break;
    case '[': case ',': zos_player_seek(-60); break;
    case ']': case '.': zos_player_seek(60); break;
    case LV_KEY_UP: case '+': case '=': zos_player_volume(5); break;
    case LV_KEY_DOWN: case '-': case '_': zos_player_volume(-5); break;
    case 'm': case 'M': zos_player_toggle_mute(); break;
    case 'n': case 'N': case ZOS_KEY_PGDN: skip(1); break;
    case 'p': case 'P': case ZOS_KEY_PGUP: skip(-1); break;
    case 'h': case 'H': case '?': case ZOS_KEY_TAB: zos_player_osd(HELP_TEXT, 5000); break;
    case 's': case 'S': if (blind) zos_player_command("osd-msg cycle sub"); else open_tracks("sub"); break;
    case 'a': case 'A': if (blind) zos_player_command("osd-msg cycle audio"); else open_tracks("audio"); break;
    case LV_KEY_ESC: case LV_KEY_BACKSPACE: case '`': case 'q': zos_player_stop(); break;
    default: break;
    }
}

static void browse_key(uint32_t k)
{
    if (zos_vlist_key(V.list, k)) return;
    switch (k) {
    case LV_KEY_ENTER: case LV_KEY_RIGHT: open_selected(); break;
    case 'o': case 'O':
        zos_setting_set_int("video_hdmi", !zos_setting_int("video_hdmi", 1));
        if (!zos_hdmi_connected()) zos_notify(0x8e4ec6, "Video", "No monitor on HDMI right now");
        render_browse();
        break;
    case LV_KEY_DEL: case 'f': case 'F': {
        int i = zos_vlist_selected(V.list);
        if (i >= 0 && !V.e[i].dir) {
            char *p = zos_path_join(cur_dir(), V.e[i].name);
            cJSON_DeleteItemFromObject(V.pos, p);
            pos_save();
            lv_free(p);
            zos_vlist_refresh(V.list);
        }
        break;
    }
    case LV_KEY_ESC: case LV_KEY_BACKSPACE: case LV_KEY_LEFT: case '`':
        if (V.depth > 1) {
            free(V.stack[--V.depth].path);
            load_dir();
            render_browse();
        } else {
            zos_app_exit();
        }
        break;
    default: break;
    }
}

static void sink_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (V.view) {
    case V_BROWSE: browse_key(k); break;
    case V_RESUME: {
        double at, dur;
        bool done;
        pos_get(V.playing, &at, &dur, &done);
        if (k == LV_KEY_ENTER || k == ' ') play(at > 3 ? at - 3 : 0);
        else if (k == '0' || k == 'r' || k == 'R') play(0);
        else if (k == LV_KEY_ESC || k == LV_KEY_BACKSPACE || k == '`') { V.playing[0] = 0; set_view(V_BROWSE); }
        break;
    }
    case V_PLAYER: player_key(k); break;
    case V_TRACKS:
        if (zos_vlist_key(V.tlist, k)) break;
        if (k == LV_KEY_ENTER) pick_track();
        else if (k == LV_KEY_ESC || k == LV_KEY_BACKSPACE || k == '`') { set_view(V_PLAYER); render_player(); }
        break;
    }
}

static void set_view(view_t v)
{
    V.view = v;
    zos_ui_show(V.browse_page, v == V_BROWSE || v == V_RESUME);
    zos_ui_show(V.resume_box, v == V_RESUME);
    zos_ui_show(V.player_page, v == V_PLAYER);
    zos_ui_show(V.tracks_page, v == V_TRACKS);
}

/* ---------------- build ---------------- */

static void build(lv_obj_t *scr)
{
    V.browse_page = zos_ui_page(scr);
    lv_obj_t *sb = zos_statusbar_create(V.browse_page, "Videos");
    V.b_title = lv_obj_get_child(sb, 0);
    V.list = zos_vlist_create(V.browse_page, 2, ZOS_STATUSBAR_H, ZOS_HOR_RES - 4,
                              ZOS_VER_RES - ZOS_STATUSBAR_H - 12, 28, 22);
    V.b_hint = zos_ui_hint(V.browse_page, "");

    V.resume_box = zos_ui_box(V.browse_page);
    lv_obj_set_size(V.resume_box, 250, 90);
    lv_obj_center(V.resume_box);
    lv_obj_set_style_bg_opa(V.resume_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(V.resume_box, lv_color_hex(0x232a33), 0);
    lv_obj_set_style_radius(V.resume_box, 10, 0);
    lv_obj_set_style_border_width(V.resume_box, 1, 0);
    lv_obj_set_style_border_color(V.resume_box, lv_color_hex(0x8e4ec6), 0);
    V.resume_lbl = zos_ui_label(V.resume_box, zos_font.body, ZOS_COLOR_TEXT);
    lv_obj_set_width(V.resume_lbl, 230);
    lv_obj_set_style_text_align(V.resume_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(V.resume_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_center(V.resume_lbl);
    zos_ui_show(V.resume_box, false);

    V.player_page = zos_ui_page(scr);
    zos_statusbar_create(V.player_page, "Now playing");
    V.p_title = zos_ui_label(V.player_page, zos_font.title, ZOS_COLOR_TEXT);
    zos_label_one_line(V.p_title, ZOS_HOR_RES - 16);
    lv_obj_set_pos(V.p_title, 8, ZOS_STATUSBAR_H + 6);
    V.p_where = zos_ui_label(V.player_page, zos_font.small, 0xc4b5fd);
    lv_obj_set_pos(V.p_where, 8, ZOS_STATUSBAR_H + 29);
    V.p_bar = lv_bar_create(V.player_page);
    lv_obj_set_size(V.p_bar, ZOS_HOR_RES - 16, 6);
    lv_bar_set_range(V.p_bar, 0, 1000);
    lv_obj_set_pos(V.p_bar, 8, ZOS_STATUSBAR_H + 50);
    lv_obj_set_style_bg_color(V.p_bar, lv_color_hex(0x2a313b), 0);
    lv_obj_set_style_bg_color(V.p_bar, lv_color_hex(0x8e4ec6), LV_PART_INDICATOR);
    V.p_time = zos_ui_label(V.player_page, zos_font.body, ZOS_COLOR_TEXT);
    lv_obj_set_pos(V.p_time, 8, ZOS_STATUSBAR_H + 61);
    V.p_state = zos_ui_label(V.player_page, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_align(V.p_state, LV_ALIGN_TOP_RIGHT, -8, ZOS_STATUSBAR_H + 63);
    V.p_tracks = zos_ui_label(V.player_page, zos_font.small, ZOS_COLOR_TEXT);
    zos_label_one_line(V.p_tracks, ZOS_HOR_RES - 16);
    lv_obj_set_pos(V.p_tracks, 8, ZOS_STATUSBAR_H + 86);
    zos_ui_hint(V.player_page, "Space pause  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " 10s  " LV_SYMBOL_UP LV_SYMBOL_DOWN
                               " vol  N/P next/prev  S subs  A audio  Esc");

    V.tracks_page = zos_ui_page(scr);
    lv_obj_t *tsb = zos_statusbar_create(V.tracks_page, "Subtitles");
    V.t_title = lv_obj_get_child(tsb, 0);
    V.tlist = zos_vlist_create(V.tracks_page, 2, ZOS_STATUSBAR_H, ZOS_HOR_RES - 4,
                               ZOS_VER_RES - ZOS_STATUSBAR_H - 12, 28, 0);
    zos_ui_hint(V.tracks_page, LV_SYMBOL_NEW_LINE " choose   Esc back");

    V.sink = zos_ui_box(scr);
    lv_obj_set_size(V.sink, 1, 1);
    lv_obj_add_event_cb(V.sink, sink_key, LV_EVENT_KEY, NULL);
}

static void video_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&V, 0, sizeof V);
    V.scr = scr;
    pos_load();
    build(scr);
    lv_group_add_obj(g, V.sink);
    lv_group_focus_obj(V.sink);
    zos_mkdirs(zos_cfg.video_dir);
    V.stack[0] = (level_t){.path = strdup(zos_cfg.video_dir)};
    V.depth = 1;
    load_dir();
    set_view(V_BROWSE);
    render_browse();
}

static void video_close(void)
{
    if (V.playing[0] && zos_player_state()->running) zos_player_stop();   /* saves the position */
    V.scr = NULL;
    free_entries();
    for (int i = 0; i < V.depth; i++) free(V.stack[i].path);
    if (V.pos) cJSON_Delete(V.pos);
    memset(&V, 0, sizeof V);
}

const zos_app_t zos_app_video = {
    .id = "video", .name = "Video", .tagline = "MP4 & MKV, subtitles, HDMI",
    .icon = &zos_icon_video, .color = 0x8e4ec6, .open = video_open, .close = video_close,
};
