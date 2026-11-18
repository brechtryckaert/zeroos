/* Music library scan, cover cache and the play queue. */
#include <cjson/cJSON.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../core/imgdec.h"
#include "../../core/player.h"
#include "../../core/settings.h"
#include "../../core/tags.h"
#include "../../core/task.h"
#include "../../zos.h"
#include "music.h"

static mlib_t LIB;
static struct {
    int *q, n, pos;          /* queue (library indexes) and current entry */
    bool active, shuffle, restarting;
    repeat_t repeat;
    void (*change)(void);
} Q;

static bool scanning;
static const char owner_tag;

const mlib_t *music_lib(void) { return &LIB; }
bool music_scanning(void) { return scanning; }

/* ======================= scan (worker thread) ======================= */

typedef struct {
    char *path;
    long long mtime, size;
    zos_tags_t tags;
} entry_t;

typedef struct {
    entry_t *e;
    int n, cap;
    cJSON *cache;          /* previous index, keyed by path */
    mlib_t lib;
    void (*done)(void);
} scan_t;

static bool is_audio(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *ext[] = {".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".wav", ".wma", ".mp4a"};
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (!strcasecmp(dot, ext[i])) return true;
    return false;
}

static void tags_from_cache(const cJSON *c, zos_tags_t *t)
{
    memset(t, 0, sizeof *t);
#define S(k, f) do { const cJSON *v = cJSON_GetObjectItem(c, k); if (cJSON_IsString(v)) snprintf(t->f, sizeof t->f, "%s", v->valuestring); } while (0)
#define N(k, f) do { const cJSON *v = cJSON_GetObjectItem(c, k); if (cJSON_IsNumber(v)) t->f = v->valueint; } while (0)
    S("t", title); S("a", artist); S("al", album); S("aa", album_artist);
    N("tr", track); N("d", disc); N("y", year);
    const cJSON *po = cJSON_GetObjectItem(c, "po"), *pl = cJSON_GetObjectItem(c, "pl");
    if (cJSON_IsNumber(po)) t->pic_offset = (long)po->valuedouble;
    if (cJSON_IsNumber(pl)) t->pic_len = (size_t)pl->valuedouble;
#undef S
#undef N
}

static void walk(scan_t *s, const char *dir, int depth)
{
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *p = zos_path_join(dir, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) { lv_free(p); continue; }
        if (S_ISDIR(st.st_mode)) {
            walk(s, p, depth + 1);
            lv_free(p);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_audio(de->d_name)) { lv_free(p); continue; }
        if (s->n == s->cap) {
            s->cap = s->cap ? s->cap * 2 : 256;
            s->e = realloc(s->e, (size_t)s->cap * sizeof *s->e);
        }
        entry_t *e = &s->e[s->n++];
        e->path = strdup(p);
        lv_free(p);
        e->mtime = (long long)st.st_mtime;
        e->size = (long long)st.st_size;
        const cJSON *c = s->cache ? cJSON_GetObjectItem(s->cache, e->path) : NULL;
        const cJSON *cm = c ? cJSON_GetObjectItem(c, "m") : NULL, *cs = c ? cJSON_GetObjectItem(c, "s") : NULL;
        if (c && cJSON_IsNumber(cm) && cJSON_IsNumber(cs) && (long long)cm->valuedouble == e->mtime &&
            (long long)cs->valuedouble == e->size)
            tags_from_cache(c, &e->tags);
        else
            zos_tags_read(e->path, &e->tags);
    }
    closedir(d);
}

static char *dup_or(const char *s, const char *def) { return strdup(s && *s ? s : def); }

static char *dirname_of(const char *path)
{
    const char *sl = strrchr(path, '/');
    return sl ? strndup(path, (size_t)(sl - path)) : strdup(".");
}

static char *basename_noext(const char *path)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *dot = strrchr(b, '.');
    return dot ? strndup(b, (size_t)(dot - b)) : strdup(b);
}

/* sorting needs the library being built; qsort has no context argument */
static mlib_t *SL;

static int cmp_track_in_album(const void *a, const void *b)
{
    const mtrack_t *x = &SL->t[*(const int *)a], *y = &SL->t[*(const int *)b];
    if (x->disc != y->disc) return x->disc - y->disc;
    if (x->track != y->track) return x->track - y->track;
    return strcasecmp(x->path, y->path);
}

static int cmp_album(const void *a, const void *b)
{
    const malbum_t *x = a, *y = b;
    int c = strcasecmp(x->artist, y->artist);
    if (c) return c;
    if (x->year != y->year) return x->year - y->year;
    return strcasecmp(x->name, y->name);
}

static int cmp_artist(const void *a, const void *b)
{
    return strcasecmp(((const martist_t *)a)->name, ((const martist_t *)b)->name);
}

static const char *cover_names[] = {"cover.jpg", "folder.jpg", "front.jpg", "cover.png", "folder.png",
                                    "Cover.jpg", "Folder.jpg", "Front.jpg", "album.jpg", "AlbumArt.jpg"};

static char *find_cover_file(const char *dir)
{
    for (size_t i = 0; i < sizeof cover_names / sizeof cover_names[0]; i++) {
        char *p = zos_path_join(dir, cover_names[i]);
        if (access(p, R_OK) == 0) {
            char *r = strdup(p);
            lv_free(p);
            return r;
        }
        lv_free(p);
    }
    return NULL;
}

static void build(scan_t *s)
{
    mlib_t *L = &s->lib;
    SL = L;
    L->nt = s->n;
    L->t = calloc((size_t)(s->n ? s->n : 1), sizeof *L->t);
    /* albums: one per (album name, folder) */
    L->al = calloc((size_t)(s->n ? s->n : 1), sizeof *L->al);
    for (int i = 0; i < s->n; i++) {
        entry_t *e = &s->e[i];
        mtrack_t *t = &L->t[i];
        t->path = e->path;
        e->path = NULL;
        t->title = e->tags.title[0] ? strdup(e->tags.title) : basename_noext(t->path);
        t->artist = dup_or(e->tags.artist[0] ? e->tags.artist : e->tags.album_artist, "Unknown artist");
        char *dir = dirname_of(t->path);
        if (e->tags.album[0]) t->album = strdup(e->tags.album);
        else { const char *b = strrchr(dir, '/'); t->album = strdup(b ? b + 1 : dir); }
        t->track = e->tags.track;
        t->disc = e->tags.disc;
        t->year = e->tags.year;
        t->pic_off = e->tags.pic_offset;
        t->pic_len = e->tags.pic_len;
        int a;
        for (a = 0; a < L->nal; a++)
            if (!strcmp(L->al[a].name, t->album) && !strcmp(L->al[a].dir, dir)) break;
        if (a == L->nal) {
            malbum_t *al = &L->al[L->nal++];
            al->name = strdup(t->album);
            al->dir = dir;
            al->artist = e->tags.album_artist[0] ? strdup(e->tags.album_artist) : strdup(t->artist);
            al->art_track = -1;
            dir = NULL;
        } else if (!e->tags.album_artist[0] && strcmp(L->al[a].artist, t->artist) &&
                   strcmp(L->al[a].artist, "Various artists")) {
            free(L->al[a].artist);
            L->al[a].artist = strdup("Various artists");
        }
        free(dir);
        malbum_t *al = &L->al[a];
        al->tracks = realloc(al->tracks, (size_t)(al->n + 1) * sizeof(int));
        al->tracks[al->n++] = i;
        if (!al->year && t->year) al->year = t->year;
        if (al->art_track < 0 && t->pic_len) al->art_track = i;
    }
    qsort(L->al, (size_t)L->nal, sizeof *L->al, cmp_album);
    for (int a = 0; a < L->nal; a++) {
        malbum_t *al = &L->al[a];
        qsort(al->tracks, (size_t)al->n, sizeof(int), cmp_track_in_album);
        for (int k = 0; k < al->n; k++) L->t[al->tracks[k]].album_idx = a;
        al->cover_file = find_cover_file(al->dir);
    }
    /* artists: by album artist */
    L->ar = calloc((size_t)(L->nal ? L->nal : 1), sizeof *L->ar);
    for (int a = 0; a < L->nal; a++) {
        int r;
        for (r = 0; r < L->nar; r++)
            if (!strcasecmp(L->ar[r].name, L->al[a].artist)) break;
        if (r == L->nar) L->ar[L->nar++].name = strdup(L->al[a].artist);
        martist_t *ar = &L->ar[r];
        ar->albums = realloc(ar->albums, (size_t)(ar->na + 1) * sizeof(int));
        ar->albums[ar->na++] = a;
        ar->ntracks += L->al[a].n;
    }
    qsort(L->ar, (size_t)L->nar, sizeof *L->ar, cmp_artist);
    /* songs: album order is already artist/year/album */
    L->songs = malloc((size_t)(L->nt ? L->nt : 1) * sizeof(int));
    int k = 0;
    for (int a = 0; a < L->nal; a++)
        for (int j = 0; j < L->al[a].n; j++) L->songs[k++] = L->al[a].tracks[j];
}

static void save_index(scan_t *s)
{
    cJSON *root = cJSON_CreateObject();
    for (int i = 0; i < s->lib.nt; i++) {
        mtrack_t *t = &s->lib.t[i];
        entry_t *e = &s->e[i];
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "m", (double)e->mtime);
        cJSON_AddNumberToObject(c, "s", (double)e->size);
        if (e->tags.title[0]) cJSON_AddStringToObject(c, "t", e->tags.title);
        if (e->tags.artist[0]) cJSON_AddStringToObject(c, "a", e->tags.artist);
        if (e->tags.album[0]) cJSON_AddStringToObject(c, "al", e->tags.album);
        if (e->tags.album_artist[0]) cJSON_AddStringToObject(c, "aa", e->tags.album_artist);
        if (e->tags.track) cJSON_AddNumberToObject(c, "tr", e->tags.track);
        if (e->tags.disc) cJSON_AddNumberToObject(c, "d", e->tags.disc);
        if (e->tags.year) cJSON_AddNumberToObject(c, "y", e->tags.year);
        if (e->tags.pic_len) {
            cJSON_AddNumberToObject(c, "po", (double)e->tags.pic_offset);
            cJSON_AddNumberToObject(c, "pl", (double)e->tags.pic_len);
        }
        cJSON_AddItemToObject(root, t->path, c);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    char p[512], tmp[520];
    snprintf(p, sizeof p, "%s/music-index.json", zos_cfg.state_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(txt, f);
        fclose(f);
        rename(tmp, p);
    }
    free(txt);
}

static cJSON *load_index(void)
{
    char p[512];
    snprintf(p, sizeof p, "%s/music-index.json", zos_cfg.state_dir);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    cJSON *j = NULL;
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
        b[n] = 0;
        j = cJSON_Parse(b);
    }
    free(b);
    fclose(f);
    return j;
}

static void scan_work(void *a)
{
    scan_t *s = a;
    s->cache = load_index();
    walk(s, zos_cfg.music_dir, 0);
    build(s);
    save_index(s);
}

static void lib_free(mlib_t *L)
{
    for (int i = 0; i < L->nt; i++) {
        free(L->t[i].path); free(L->t[i].title); free(L->t[i].artist); free(L->t[i].album);
    }
    for (int a = 0; a < L->nal; a++) {
        free(L->al[a].name); free(L->al[a].artist); free(L->al[a].dir); free(L->al[a].tracks); free(L->al[a].cover_file);
    }
    for (int r = 0; r < L->nar; r++) { free(L->ar[r].name); free(L->ar[r].albums); }
    free(L->t); free(L->al); free(L->ar); free(L->songs);
    memset(L, 0, sizeof *L);
}

static void covers_clear(void);
static void covers_ready(void);

static void scan_done(void *a)
{
    scan_t *s = a;
    /* Track indexes change with the new library: re-point the play queue
     * by path (anything that disappeared is dropped). */
    char **qp = NULL;
    int qn = 0;
    if (Q.active) {
        qp = malloc((size_t)Q.n * sizeof *qp);
        for (int i = 0; i < Q.n; i++) qp[i] = strdup(LIB.t[Q.q[i]].path);
        qn = Q.n;
    }
    covers_clear();
    lib_free(&LIB);
    LIB = s->lib;
    memset(&s->lib, 0, sizeof s->lib);
    if (qp) {
        int k = 0, newpos = 0;
        for (int i = 0; i < qn; i++) {
            for (int t = 0; t < LIB.nt; t++)
                if (!strcmp(LIB.t[t].path, qp[i])) {
                    if (i == Q.pos) newpos = k;
                    Q.q[k++] = t;
                    break;
                }
            free(qp[i]);
        }
        free(qp);
        Q.n = k;
        Q.pos = newpos;
        if (!k) music_stop();
    }
    scanning = false;
    covers_ready();                  /* list rows may still point at the old covers */
    if (s->done) s->done();
}

static void scan_release(void *a)
{
    scan_t *s = a;
    for (int i = 0; i < s->n; i++) free(s->e[i].path);
    free(s->e);
    if (s->cache) cJSON_Delete(s->cache);
    lib_free(&s->lib);
    free(s);
    scanning = false;
}

void music_scan(void (*done)(void))
{
    if (scanning) return;
    scanning = true;
    scan_t *s = calloc(1, sizeof *s);
    s->done = done;
    zos_task_run(&owner_tag, scan_work, scan_done, scan_release, s);
}

/* ======================= covers ======================= */

#define BIG_SLOTS 4

static struct {
    lv_image_dsc_t **small;     /* per album */
    bool *small_pending;
    int nsmall;
    struct { int album; lv_image_dsc_t *img; bool pending; } big[BIG_SLOTS];
    int big_next;
    void (*ready)(void);
} C;

static void covers_clear(void)
{
    zos_task_cancel(&C);
    for (int i = 0; i < C.nsmall; i++) zos_img_free(C.small[i]);
    free(C.small);
    free(C.small_pending);
    C.small = NULL;
    C.small_pending = NULL;
    C.nsmall = 0;
    for (int i = 0; i < BIG_SLOTS; i++) {
        zos_img_free(C.big[i].img);
        C.big[i].img = NULL;
        C.big[i].album = -1;
        C.big[i].pending = false;
    }
}

void music_set_cover_ready_cb(void (*ready)(void)) { C.ready = ready; }
static void covers_ready(void) { if (C.ready) C.ready(); }

typedef struct {
    int album, size, slot;
    char *file, *audio;
    long off;
    size_t len;
    lv_image_dsc_t *img;
} cover_job_t;

static void cover_work(void *a)
{
    cover_job_t *j = a;
    if (j->file) j->img = zos_img_decode_file(j->file, j->size, j->size, true);
    if (!j->img && j->audio) {
        zos_tags_t t = {.pic_offset = j->off, .pic_len = j->len};
        size_t n;
        void *b = zos_tags_picture(j->audio, &t, &n);
        if (b) j->img = zos_img_decode_mem(b, n, j->size, j->size, true);
        free(b);
    }
}

static void cover_done(void *a)
{
    cover_job_t *j = a;
    if (j->size <= 32) {
        if (j->album < C.nsmall) {
            C.small[j->album] = j->img;
            C.small_pending[j->album] = false;
            j->img = NULL;
        }
    } else if (C.big[j->slot].album == j->album) {
        C.big[j->slot].img = j->img;
        C.big[j->slot].pending = false;
        j->img = NULL;
    }
    if (C.ready) C.ready();
}

static void cover_release(void *a)
{
    cover_job_t *j = a;
    zos_img_free(j->img);
    free(j->file);
    free(j->audio);
    free(j);
}

static void request(int album, int size, int slot)
{
    const malbum_t *al = &LIB.al[album];
    if (!al->cover_file && al->art_track < 0) return;
    cover_job_t *j = calloc(1, sizeof *j);
    j->album = album;
    j->size = size;
    j->slot = slot;
    if (al->cover_file) j->file = strdup(al->cover_file);
    if (al->art_track >= 0) {
        const mtrack_t *t = &LIB.t[al->art_track];
        j->audio = strdup(t->path);
        j->off = t->pic_off;
        j->len = t->pic_len;
    }
    zos_task_run(&C, cover_work, cover_done, cover_release, j);
}

const lv_image_dsc_t *music_cover(int album, int size)
{
    if (album < 0 || album >= LIB.nal) return NULL;
    if (size <= 32) {
        if (C.nsmall != LIB.nal) {
            covers_clear();
            C.small = calloc((size_t)LIB.nal, sizeof *C.small);
            C.small_pending = calloc((size_t)LIB.nal, sizeof *C.small_pending);
            C.nsmall = LIB.nal;
        }
        if (C.small[album]) return C.small[album];
        if (!C.small_pending[album]) {
            C.small_pending[album] = true;
            request(album, size, 0);
        }
        return NULL;
    }
    for (int i = 0; i < BIG_SLOTS; i++)
        if (C.big[i].album == album && (C.big[i].img || C.big[i].pending)) return C.big[i].img;
    int s = C.big_next;
    C.big_next = (C.big_next + 1) % BIG_SLOTS;
    zos_img_free(C.big[s].img);
    C.big[s].img = NULL;
    C.big[s].album = album;
    C.big[s].pending = true;
    request(album, size, s);
    return NULL;
}

/* ======================= queue / playback ======================= */


void music_set_change_cb(void (*cb)(void)) { Q.change = cb; }
bool music_active(void) { return Q.active; }
bool music_shuffle(void) { return Q.shuffle; }
repeat_t music_repeat(void) { return Q.repeat; }

int music_current(void)
{
    return Q.active && Q.pos >= 0 && Q.pos < Q.n ? Q.q[Q.pos] : -1;
}

const int *music_queue(int *n, int *pos)
{
    *n = Q.active ? Q.n : 0;
    *pos = Q.pos;
    return Q.q;
}

static void notify(void)
{
    if (Q.change) Q.change();
}

static void on_change(void *ud)
{
    LV_UNUSED(ud);
    const zos_player_state_t *s = zos_player_state();
    if (s->playlist_pos >= 0 && s->playlist_pos < Q.n) Q.pos = s->playlist_pos;
    notify();
}

static void on_end(bool eof, double last, void *ud)
{
    LV_UNUSED(eof); LV_UNUSED(last); LV_UNUSED(ud);
    if (Q.restarting) return;
    Q.active = false;
    notify();
}

static void shuffle_ints(int *a, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

static void start_at(int pos, double offset)
{
    char path[512];
    snprintf(path, sizeof path, "%s/queue.m3u", zos_cfg.state_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < Q.n; i++) fprintf(f, "%s\n", LIB.t[Q.q[i]].path);
    fclose(f);
    Q.pos = pos;
    zos_player_opts_t o = {
        .start = offset,
        .playlist = path,
        .playlist_start = pos,
        .loop = Q.repeat == REPEAT_ALL ? "playlist" : Q.repeat == REPEAT_ONE ? "file" : NULL,
        .volume = zos_setting_int("volume", 80),
    };
    zos_player_cbs_t cbs = {.on_change = on_change, .on_end = on_end};
    Q.restarting = true;             /* stopping the old mpv must not end the queue */
    Q.active = zos_player_play(NULL, &o, cbs, NULL);
    Q.restarting = false;
    notify();
}

void music_play(const int *tracks, int n, int start, bool shuffle)
{
    if (n <= 0) return;
    Q.repeat = (repeat_t)zos_setting_int("music_repeat", REPEAT_OFF);
    free(Q.q);
    Q.q = malloc((size_t)n * sizeof(int));
    memcpy(Q.q, tracks, (size_t)n * sizeof(int));
    Q.n = n;
    Q.shuffle = shuffle;
    if (start < 0) start = shuffle ? rand() % n : 0;
    if (shuffle) {
        /* the chosen track first, the rest shuffled */
        int t = Q.q[0]; Q.q[0] = Q.q[start]; Q.q[start] = t;
        shuffle_ints(Q.q + 1, n - 1);
        start = 0;
    }
    start_at(start, 0);
}

void music_toggle_shuffle(void)
{
    if (!Q.active) { Q.shuffle = !Q.shuffle; notify(); return; }
    int cur = Q.q[Q.pos];
    double at = zos_player_state()->pos;
    Q.shuffle = !Q.shuffle;
    if (Q.shuffle) {
        /* keep what has played, shuffle what is left */
        shuffle_ints(Q.q + Q.pos + 1, Q.n - Q.pos - 1);
        start_at(Q.pos, at);
    } else {
        /* back to album order: re-sort the queue by library order */
        int *ord = malloc((size_t)Q.n * sizeof(int));
        int k = 0;
        for (int i = 0; i < LIB.nt && k < Q.n; i++) {
            int t = LIB.songs[i];
            for (int j = 0; j < Q.n; j++)
                if (Q.q[j] == t) { ord[k++] = t; break; }
        }
        memcpy(Q.q, ord, (size_t)k * sizeof(int));
        free(ord);
        int p = 0;
        for (int j = 0; j < Q.n; j++) if (Q.q[j] == cur) p = j;
        start_at(p, at);
    }
}

void music_cycle_repeat(void)
{
    Q.repeat = (Q.repeat + 1) % 3;
    zos_setting_set_int("music_repeat", Q.repeat);
    if (Q.active) {
        zos_player_set("loop-playlist", Q.repeat == REPEAT_ALL ? "\"inf\"" : "\"no\"");
        zos_player_set("loop-file", Q.repeat == REPEAT_ONE ? "\"inf\"" : "\"no\"");
    }
    notify();
}

void music_jump(int pos)
{
    if (!Q.active || pos < 0 || pos >= Q.n) return;
    char v[16];
    snprintf(v, sizeof v, "%d", pos);
    zos_player_set("playlist-pos", v);
}

void music_next(void)
{
    if (!Q.active) return;
    if (Q.pos + 1 < Q.n || Q.repeat == REPEAT_ALL) zos_player_command("playlist-next");
}

void music_prev(void)
{
    if (!Q.active) return;
    /* like most players: restart the song unless we are at its very start */
    if (zos_player_state()->pos > 4 || Q.pos == 0) zos_player_seek_abs(0);
    else zos_player_command("playlist-prev");
}

void music_stop(void)
{
    if (!Q.active) return;
    Q.active = false;
    zos_player_stop();
    notify();
}

void music_volume(int delta)
{
    int v = zos_setting_int("volume", 80) + delta;
    if (v < 0) v = 0;
    if (v > 130) v = 130;
    zos_setting_set_int("volume", v);
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    zos_player_set("volume", b);
}

__attribute__((constructor)) static void init_rand(void) { srand((unsigned)time(NULL)); }
