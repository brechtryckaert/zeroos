#define _GNU_SOURCE
/* Camera: viewfinder, photos and video with the CardputerZero's IMX219,
 * through Raspberry Pi's rpicam-apps (libcamera).
 *
 * The viewfinder is rpicam-vid writing small raw YUV420 frames to a pipe,
 * drawn on a canvas. The camera can only be used by one program at a time,
 * so the viewfinder stops while a photo is taken (rpicam-still, full 8 MP)
 * or a video is recorded (rpicam-vid, 720p H.264 in MP4).
 * Photos go to ~/Pictures, videos to ~/Videos/Camera. G opens the gallery. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../core/imgdec.h"
#include "../../core/player.h"
#include "../../core/proc.h"
#include "../../core/task.h"
#include "../../core/ui.h"
#include "../../zos.h"

extern const lv_image_dsc_t zos_icon_camera;
extern const zos_app_t zos_app_camera;
extern char **environ;

#define PW 320                    /* viewfinder stream size */
#define PH 240
#define FRAME_BYTES (PW * PH * 3 / 2)
#define CW 227                    /* on screen: 4:3 at full height */
#define CH 170
#define THUMB 26

typedef enum { V_CAMERA, V_GALLERY, V_PHOTO } view_t;

typedef struct {
    char *path;
    bool video;
    time_t mtime;
    lv_image_dsc_t *thumb;
    bool thumb_pending;
} shot_t;

static struct {
    lv_obj_t *scr, *sink;
    view_t view;
    /* camera */
    lv_obj_t *cam_page, *canvas, *msg, *mode_lbl, *count_lbl, *last_box, *last_img, *rec_lbl, *side_hint, *busy_lbl;
    uint16_t *canvas_buf;
    uint8_t *frame;
    size_t frame_fill;
    int xmap[CW], ymap[CH];
    pid_t prev_pid;
    int prev_fd, prev_err;
    char prev_errbuf[512];
    size_t prev_errlen;
    uint32_t prev_started;
    bool video_mode, busy;
    pid_t rec_pid;
    char rec_path[512];
    uint32_t rec_started;
    lv_image_dsc_t *last_thumb;
    lv_timer_t *timer;
    char vid_exe[32], still_exe[32];
    bool no_camera;
    /* gallery */
    lv_obj_t *gal_page;
    zos_vlist_t *glist;
    shot_t *shots;
    int nshots;
    /* photo viewer */
    lv_obj_t *photo_page, *photo_img, *photo_info, *photo_msg;
    lv_image_dsc_t *photo;
    int photo_idx;
    bool confirm_delete;
} C;

static const char owner_tag;
static void start_preview(void);
static void stop_preview(void);
static void set_view(view_t v);

/* ---------------- helpers ---------------- */

static bool find_exe(const char *a, const char *b, char *out, size_t n)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    const char *names[] = {a, b};
    for (int k = 0; k < 2; k++) {
        char *copy = strdup(path), *save = NULL;
        for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s", d, names[k]);
            if (access(p, X_OK) == 0) {
                snprintf(out, n, "%s", names[k]);
                free(copy);
                return true;
            }
        }
        free(copy);
    }
    return false;
}

static void stamp(char *out, size_t n, const char *dir, const char *prefix, const char *ext)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tm);
    snprintf(out, n, "%s/%s_%s.%s", dir, prefix, ts, ext);
}

static char *camera_video_dir(void) { return zos_path_join(zos_cfg.video_dir, "Camera"); }

static void set_msg(const char *m)
{
    lv_label_set_text(C.msg, m ? m : "");
    zos_ui_show(C.msg, m && *m);
}

/* ---------------- viewfinder ---------------- */

static pid_t spawn_pipe(const char *const argv[], int *out_fd, int *err_fd)
{
    int po[2], pe[2];
    if (pipe2(po, O_CLOEXEC) || pipe2(pe, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, po[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(po[1]);
    close(pe[1]);
    if (rc) { close(po[0]); close(pe[0]); return -1; }
    fcntl(po[0], F_SETFL, O_NONBLOCK);
    fcntl(pe[0], F_SETFL, O_NONBLOCK);
    *out_fd = po[0];
    *err_fd = pe[0];
    return pid;
}

static void reap(pid_t pid, int sig, int wait_ms)
{
    if (pid <= 0) return;
    kill(pid, sig);
    int st;
    for (int i = 0; i < wait_ms / 10; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
}

static void start_preview(void)
{
    if (C.prev_pid > 0 || C.no_camera || C.rec_pid > 0 || C.busy) return;
    char w[8], h[8];
    snprintf(w, sizeof w, "%d", PW);
    snprintf(h, sizeof h, "%d", PH);
    const char *argv[] = {C.vid_exe, "-t", "0", "-n", "--codec", "yuv420", "--width", w, "--height", h,
                          "--framerate", "15", "--flush", "-o", "-", NULL};
    C.prev_pid = spawn_pipe(argv, &C.prev_fd, &C.prev_err);
    C.frame_fill = 0;
    C.prev_errlen = 0;
    C.prev_started = lv_tick_get();
    if (C.prev_pid <= 0) {
        C.prev_pid = 0;
        set_msg("Could not start the camera.");
    } else {
        set_msg("Starting camera\xe2\x80\xa6");
    }
}

static void stop_preview(void)
{
    if (C.prev_pid <= 0) return;
    close(C.prev_fd);
    close(C.prev_err);
    reap(C.prev_pid, SIGTERM, 1500);
    C.prev_pid = 0;
}

static inline uint16_t yuv565(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void draw_frame(void)
{
    const uint8_t *Y = C.frame, *U = C.frame + PW * PH, *Vp = U + PW * PH / 4;
    for (int y = 0; y < CH; y++) {
        int sy = C.ymap[y];
        const uint8_t *yr = Y + sy * PW, *ur = U + (sy / 2) * (PW / 2), *vr = Vp + (sy / 2) * (PW / 2);
        uint16_t *d = C.canvas_buf + y * CW;
        for (int x = 0; x < CW; x++) {
            int sx = C.xmap[x];
            d[x] = yuv565(yr[sx], ur[sx / 2], vr[sx / 2]);
        }
    }
    lv_obj_invalidate(C.canvas);
}

static void preview_poll(void)
{
    if (C.prev_pid <= 0) return;
    bool got = false;
    for (;;) {
        ssize_t r = read(C.prev_fd, C.frame + C.frame_fill, FRAME_BYTES - C.frame_fill);
        if (r <= 0) break;
        C.frame_fill += (size_t)r;
        if (C.frame_fill == FRAME_BYTES) { got = true; C.frame_fill = 0; }
    }
    if (C.prev_errlen < sizeof C.prev_errbuf - 1) {
        ssize_t r = read(C.prev_err, C.prev_errbuf + C.prev_errlen, sizeof C.prev_errbuf - 1 - C.prev_errlen);
        if (r > 0) { C.prev_errlen += (size_t)r; C.prev_errbuf[C.prev_errlen] = 0; }
    } else {
        char sink[256];
        while (read(C.prev_err, sink, sizeof sink) > 0) {}
    }
    if (got) {
        draw_frame();
        set_msg(NULL);
    }
    int st;
    if (waitpid(C.prev_pid, &st, WNOHANG) == C.prev_pid) {
        close(C.prev_fd);
        close(C.prev_err);
        C.prev_pid = 0;
        if (strstr(C.prev_errbuf, "no cameras") || strstr(C.prev_errbuf, "No camera")) {
            C.no_camera = true;
            set_msg("No camera found.\nThe camera is on the full CardputerZero\n(not the Lite). G opens the gallery.");
        } else {
            set_msg("The camera stopped.\nEnter to try again.");
        }
    }
}

/* ---------------- photo / video ---------------- */

typedef struct {
    char path[512];
    char still[32];
    int rc;
    char out[512];
    lv_image_dsc_t *thumb;
} photo_job_t;

static void photo_work(void *a)
{
    photo_job_t *j = a;
    const char *argv[] = {j->still, "-n", "-t", "800", "-q", "92", "-o", j->path, NULL};
    j->rc = zos_run(argv, j->out, sizeof j->out, 20000);
    if (j->rc == 0) j->thumb = zos_img_decode_file(j->path, 80, 60, true);
}

static void update_side(void);

static void photo_done(void *a)
{
    photo_job_t *j = a;
    C.busy = false;
    zos_ui_show(C.busy_lbl, false);
    if (j->rc == 0 && access(j->path, R_OK) == 0) {
        zos_img_free(C.last_thumb);
        C.last_thumb = j->thumb;
        j->thumb = NULL;
        const char *b = strrchr(j->path, '/');
        zos_notify(0x12a594, "Photo saved", b ? b + 1 : j->path);
    } else {
        zos_notify(0xe5484d, "Camera", "Could not take the photo");
    }
    update_side();
    if (C.view == V_CAMERA) start_preview();
}

static void photo_release(void *a)
{
    photo_job_t *j = a;
    zos_img_free(j->thumb);
    free(j);
}

static void take_photo(void)
{
    if (C.busy || C.no_camera) return;
    stop_preview();
    zos_mkdirs(zos_cfg.pictures_dir);
    photo_job_t *j = calloc(1, sizeof *j);
    stamp(j->path, sizeof j->path, zos_cfg.pictures_dir, "IMG", "jpg");
    snprintf(j->still, sizeof j->still, "%s", C.still_exe);
    C.busy = true;
    lv_label_set_text(C.busy_lbl, LV_SYMBOL_IMAGE "  Taking photo\xe2\x80\xa6");
    zos_ui_show(C.busy_lbl, true);
    zos_task_run(&owner_tag, photo_work, photo_done, photo_release, j);
}

static void start_recording(void)
{
    if (C.no_camera || C.busy) return;
    stop_preview();
    char *dir = camera_video_dir();
    zos_mkdirs(dir);
    stamp(C.rec_path, sizeof C.rec_path, dir, "VID", "mp4");
    lv_free(dir);
    const char *argv[] = {C.vid_exe, "-t", "0", "-n", "--codec", "libav", "--libav-format", "mp4",
                          "--width", "1280", "--height", "720", "--framerate", "30", "-o", C.rec_path, NULL};
    int out, err;
    C.rec_pid = spawn_pipe(argv, &out, &err);
    if (C.rec_pid <= 0) {
        C.rec_pid = 0;
        zos_notify(0xe5484d, "Camera", "Could not start recording");
        start_preview();
        return;
    }
    close(out);
    close(err);
    C.rec_started = lv_tick_get();
    lv_canvas_fill_bg(C.canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    update_side();
}

static void stop_recording(void)
{
    if (C.rec_pid <= 0) return;
    reap(C.rec_pid, SIGINT, 5000);   /* SIGINT lets rpicam-vid finish the MP4 */
    C.rec_pid = 0;
    const char *b = strrchr(C.rec_path, '/');
    zos_notify(0x12a594, "Video saved", b ? b + 1 : C.rec_path);
    update_side();
    start_preview();
}

/* ---------------- gallery ---------------- */

static int cmp_shot(const void *a, const void *b)
{
    const shot_t *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0;
}

static bool has_ext(const char *n, const char *const *ext)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    for (; *ext; ext++)
        if (!strcasecmp(dot, *ext)) return true;
    return false;
}

static void add_dir(const char *dir, bool video, int *cap)
{
    static const char *const img_ext[] = {".jpg", ".jpeg", ".png", NULL};
    static const char *const vid_ext[] = {".mp4", ".mkv", ".h264", NULL};
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || !has_ext(de->d_name, video ? vid_ext : img_ext)) continue;
        char *p = zos_path_join(dir, de->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            if (C.nshots == *cap) { *cap = *cap ? *cap * 2 : 64; C.shots = realloc(C.shots, (size_t)*cap * sizeof *C.shots); }
            C.shots[C.nshots++] = (shot_t){.path = strdup(p), .video = video, .mtime = st.st_mtime};
        }
        lv_free(p);
    }
    closedir(d);
}

static void free_shots(void)
{
    zos_task_cancel(&C.shots);
    for (int i = 0; i < C.nshots; i++) { free(C.shots[i].path); zos_img_free(C.shots[i].thumb); }
    free(C.shots);
    C.shots = NULL;
    C.nshots = 0;
}

static void load_shots(void)
{
    free_shots();
    int cap = 0;
    add_dir(zos_cfg.pictures_dir, false, &cap);
    char *vd = camera_video_dir();
    add_dir(vd, true, &cap);
    lv_free(vd);
    qsort(C.shots, (size_t)C.nshots, sizeof *C.shots, cmp_shot);
}

typedef struct { int idx; char *path; lv_image_dsc_t *img; } thumb_job_t;

static void thumb_work(void *a) { thumb_job_t *j = a; j->img = zos_img_decode_file(j->path, THUMB, THUMB, true); }
static void thumb_done(void *a)
{
    thumb_job_t *j = a;
    if (j->idx < C.nshots && !strcmp(C.shots[j->idx].path, j->path)) {
        C.shots[j->idx].thumb = j->img;
        j->img = NULL;
        if (C.view == V_GALLERY) zos_vlist_refresh(C.glist);
    }
}
static void thumb_release(void *a) { thumb_job_t *j = a; zos_img_free(j->img); free(j->path); free(j); }

static void fill_gallery(int i, zos_vrow_t *r, void *ud)
{
    LV_UNUSED(ud);
    shot_t *s = &C.shots[i];
    const char *b = strrchr(s->path, '/');
    snprintf(r->title, sizeof r->title, "%s", b ? b + 1 : s->path);
    struct tm tm;
    localtime_r(&s->mtime, &tm);
    strftime(r->sub, sizeof r->sub, s->video ? "Video \xc2\xb7 %d %b %Y, %H:%M" : "Photo \xc2\xb7 %d %b %Y, %H:%M", &tm);
    if (s->video) { r->symbol = LV_SYMBOL_VIDEO; r->symbol_color = 0x8e4ec6; return; }
    r->symbol = LV_SYMBOL_IMAGE;
    r->img = s->thumb;
    if (!s->thumb && !s->thumb_pending) {
        s->thumb_pending = true;
        thumb_job_t *j = calloc(1, sizeof *j);
        j->idx = i;
        j->path = strdup(s->path);
        zos_task_run(&C.shots, thumb_work, thumb_done, thumb_release, j);
    }
}

static void open_gallery(void)
{
    stop_preview();
    load_shots();
    char empty[300];
    snprintf(empty, sizeof empty, "No photos or videos yet.\nThey are saved in %s", zos_cfg.pictures_dir);
    zos_vlist_set_empty_text(C.glist, empty);
    zos_vlist_set(C.glist, C.nshots, fill_gallery, NULL);
    zos_vlist_select(C.glist, 0);
    set_view(V_GALLERY);
}

/* ---------------- photo viewer ---------------- */

typedef struct { int idx; char *path; lv_image_dsc_t *img; } view_job_t;

static void view_work(void *a) { view_job_t *j = a; j->img = zos_img_decode_file(j->path, ZOS_HOR_RES, ZOS_VER_RES, false); }
static void view_done(void *a)
{
    view_job_t *j = a;
    if (C.view != V_PHOTO || j->idx != C.photo_idx) return;
    zos_img_free(C.photo);
    C.photo = j->img;
    j->img = NULL;
    if (C.photo) lv_image_set_src(C.photo_img, C.photo);
    zos_ui_show(C.photo_img, C.photo != NULL);
    lv_label_set_text(C.photo_msg, C.photo ? "" : "Can't open this image.");
}
static void view_release(void *a) { view_job_t *j = a; zos_img_free(j->img); free(j->path); free(j); }

static void show_photo(int idx)
{
    if (idx < 0 || idx >= C.nshots) return;
    C.photo_idx = idx;
    C.confirm_delete = false;
    shot_t *s = &C.shots[idx];
    if (s->video) {
        zos_player_opts_t o = {.video = true};
        zos_player_cbs_t cbs = {0};
        zos_player_play(s->path, &o, cbs, NULL);
        return;
    }
    zos_ui_show(C.photo_img, false);
    lv_label_set_text(C.photo_msg, "Loading\xe2\x80\xa6");
    const char *b = strrchr(s->path, '/');
    char info[200];
    snprintf(info, sizeof info, "%s   %d/%d   " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT "  D delete  Esc", b ? b + 1 : s->path, idx + 1, C.nshots);
    lv_label_set_text(C.photo_info, info);
    set_view(V_PHOTO);
    view_job_t *j = calloc(1, sizeof *j);
    j->idx = idx;
    j->path = strdup(s->path);
    zos_task_run(&owner_tag, view_work, view_done, view_release, j);
}

static void delete_current(void)
{
    if (C.photo_idx < 0 || C.photo_idx >= C.nshots) return;
    unlink(C.shots[C.photo_idx].path);
    int keep = C.photo_idx;
    load_shots();
    zos_vlist_set(C.glist, C.nshots, fill_gallery, NULL);
    if (!C.nshots) { set_view(V_GALLERY); return; }
    show_photo(keep < C.nshots ? keep : C.nshots - 1);
}

/* ---------------- UI ---------------- */

static void update_side(void)
{
    lv_label_set_text(C.mode_lbl, C.video_mode ? LV_SYMBOL_VIDEO "  Video" : LV_SYMBOL_IMAGE "  Photo");
    if (C.rec_pid > 0) {
        uint32_t s = (lv_tick_get() - C.rec_started) / 1000;
        lv_label_set_text_fmt(C.rec_lbl, LV_SYMBOL_STOP " REC %02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
        zos_ui_show(C.rec_lbl, true);
        set_msg("Recording 720p\xe2\x80\xa6\nThe viewfinder is off while\nthe camera records.");
    } else {
        zos_ui_show(C.rec_lbl, false);
    }
    if (C.last_thumb) {
        lv_image_set_src(C.last_img, C.last_thumb);
        zos_ui_show(C.last_box, true);
    } else {
        zos_ui_show(C.last_box, false);
    }
    lv_label_set_text(C.side_hint, C.rec_pid > 0 ? LV_SYMBOL_NEW_LINE " stop" :
                      C.video_mode ? LV_SYMBOL_NEW_LINE " record\nTab photo\nG gallery\nEsc back"
                                   : LV_SYMBOL_NEW_LINE " shoot\nTab video\nG gallery\nEsc back");
}

static void timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (C.view == V_CAMERA) preview_poll();
    if (C.rec_pid > 0) {
        int st;
        if (waitpid(C.rec_pid, &st, WNOHANG) == C.rec_pid) {
            C.rec_pid = 0;
            zos_notify(0xe5484d, "Camera", "Recording stopped");
            update_side();
            start_preview();
        } else {
            update_side();
        }
    }
}

static void set_view(view_t v)
{
    C.view = v;
    zos_ui_show(C.cam_page, v == V_CAMERA);
    zos_ui_show(C.gal_page, v == V_GALLERY);
    zos_ui_show(C.photo_page, v == V_PHOTO);
    if (v != V_PHOTO && C.photo) {
        zos_ui_show(C.photo_img, false);
        lv_image_set_src(C.photo_img, LV_SYMBOL_DUMMY);   /* drop the reference before freeing */
        zos_img_free(C.photo);
        C.photo = NULL;
    }
}

static void camera_key(uint32_t k)
{
    switch (k) {
    case LV_KEY_ENTER: case ' ':
        if (C.rec_pid > 0) stop_recording();
        else if (C.prev_pid <= 0 && !C.busy && !C.no_camera) start_preview();
        else if (C.video_mode) start_recording();
        else take_photo();
        break;
    case ZOS_KEY_TAB: case 'm': case 'M':
        if (C.rec_pid <= 0) { C.video_mode = !C.video_mode; update_side(); }
        break;
    case 'g': case 'G':
        if (C.rec_pid <= 0 && !C.busy) open_gallery();
        break;
    case LV_KEY_ESC: case LV_KEY_BACKSPACE: case '`':
        if (C.rec_pid > 0) stop_recording();
        else zos_app_exit();
        break;
    default: break;
    }
}

static void sink_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (C.view) {
    case V_CAMERA: camera_key(k); break;
    case V_GALLERY:
        if (zos_vlist_key(C.glist, k)) break;
        if (k == LV_KEY_ENTER || k == LV_KEY_RIGHT) show_photo(zos_vlist_selected(C.glist));
        else if (k == LV_KEY_ESC || k == LV_KEY_BACKSPACE || k == LV_KEY_LEFT || k == '`') {
            free_shots();
            set_view(V_CAMERA);
            start_preview();
        }
        break;
    case V_PHOTO:
        if (k == LV_KEY_RIGHT || k == LV_KEY_DOWN || k == ' ') {
            int n = C.photo_idx + 1;
            while (n < C.nshots && C.shots[n].video) n++;
            if (n < C.nshots) show_photo(n);
        } else if (k == LV_KEY_LEFT || k == LV_KEY_UP) {
            int n = C.photo_idx - 1;
            while (n >= 0 && C.shots[n].video) n--;
            if (n >= 0) show_photo(n);
        } else if (k == 'd' || k == 'D' || k == LV_KEY_DEL) {
            if (C.confirm_delete) delete_current();
            else {
                C.confirm_delete = true;
                lv_label_set_text(C.photo_info, "Press D again to delete this photo");
            }
        } else if (k == LV_KEY_ESC || k == LV_KEY_BACKSPACE || k == '`') {
            set_view(V_GALLERY);
            zos_vlist_select(C.glist, C.photo_idx);
        }
        break;
    }
}

static void build(lv_obj_t *scr)
{
    C.cam_page = zos_ui_page(scr);
    lv_obj_set_style_bg_opa(C.cam_page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(C.cam_page, lv_color_hex(0x000000), 0);
    C.canvas_buf = lv_malloc(CW * CH * 2);
    C.canvas = lv_canvas_create(C.cam_page);
    lv_canvas_set_buffer(C.canvas, C.canvas_buf, CW, CH, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(C.canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_obj_set_pos(C.canvas, 0, 0);
    C.msg = zos_ui_label(C.cam_page, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_set_width(C.msg, CW - 16);
    lv_label_set_long_mode(C.msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(C.msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(C.msg, LV_ALIGN_LEFT_MID, 8, 0);
    C.busy_lbl = zos_ui_label(C.cam_page, zos_font.body, ZOS_COLOR_TEXT);
    lv_obj_set_style_bg_opa(C.busy_lbl, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(C.busy_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(C.busy_lbl, 6, 0);
    lv_obj_align(C.busy_lbl, LV_ALIGN_LEFT_MID, 40, 0);
    zos_ui_show(C.busy_lbl, false);

    lv_obj_t *side = zos_ui_box(C.cam_page);
    lv_obj_set_size(side, ZOS_HOR_RES - CW, ZOS_VER_RES);
    lv_obj_set_pos(side, CW, 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(side, lv_color_hex(ZOS_COLOR_BG), 0);
    C.mode_lbl = zos_ui_label(side, zos_font.body, 0x2dd4bf);
    lv_obj_align(C.mode_lbl, LV_ALIGN_TOP_MID, 0, 6);
    C.rec_lbl = zos_ui_label(side, zos_font.body, 0xef4444);
    lv_obj_align(C.rec_lbl, LV_ALIGN_TOP_MID, 0, 28);
    C.last_box = zos_ui_box(side);
    lv_obj_set_size(C.last_box, 80, 60);
    lv_obj_align(C.last_box, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_radius(C.last_box, 4, 0);
    lv_obj_set_style_clip_corner(C.last_box, true, 0);
    C.last_img = lv_image_create(C.last_box);
    lv_obj_center(C.last_img);
    C.side_hint = zos_ui_label(side, &lv_font_montserrat_10, 0x8a94a3);
    lv_obj_set_style_text_align(C.side_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(C.side_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    (void)C.count_lbl;

    C.gal_page = zos_ui_page(scr);
    zos_statusbar_create(C.gal_page, "Gallery");
    C.glist = zos_vlist_create(C.gal_page, 2, ZOS_STATUSBAR_H, ZOS_HOR_RES - 4, ZOS_VER_RES - ZOS_STATUSBAR_H - 12, 30, THUMB);
    zos_ui_hint(C.gal_page, LV_SYMBOL_NEW_LINE " view / play   Esc camera");

    C.photo_page = zos_ui_page(scr);
    lv_obj_set_style_bg_opa(C.photo_page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(C.photo_page, lv_color_hex(0x000000), 0);
    C.photo_img = lv_image_create(C.photo_page);
    lv_obj_center(C.photo_img);
    C.photo_msg = zos_ui_label(C.photo_page, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_center(C.photo_msg);
    C.photo_info = zos_ui_label(C.photo_page, &lv_font_montserrat_10, 0xd1d5db);
    lv_obj_set_style_bg_opa(C.photo_info, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(C.photo_info, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_hor(C.photo_info, 4, 0);
    lv_obj_align(C.photo_info, LV_ALIGN_BOTTOM_MID, 0, -2);

    C.sink = zos_ui_box(scr);
    lv_obj_set_size(C.sink, 1, 1);
    lv_obj_add_event_cb(C.sink, sink_key, LV_EVENT_KEY, NULL);
}

static void camera_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&C, 0, sizeof C);
    C.scr = scr;
    C.frame = lv_malloc(FRAME_BYTES);
    /* 320x240 -> 227x170 nearest-neighbour maps */
    for (int x = 0; x < CW; x++) C.xmap[x] = x * PW / CW;
    for (int y = 0; y < CH; y++) C.ymap[y] = y * PH / CH;
    build(scr);
    lv_group_add_obj(g, C.sink);
    lv_group_focus_obj(C.sink);
    set_view(V_CAMERA);
    update_side();
    if (!find_exe("rpicam-vid", "libcamera-vid", C.vid_exe, sizeof C.vid_exe) ||
        !find_exe("rpicam-still", "libcamera-still", C.still_exe, sizeof C.still_exe)) {
        C.no_camera = true;
        set_msg("Camera software (rpicam-apps) is not installed.\nG opens the gallery.");
    } else {
        start_preview();
    }
    C.timer = lv_timer_create(timer_cb, 30, NULL);
}

static void camera_close(void)
{
    zos_task_cancel(&owner_tag);
    if (C.rec_pid > 0) reap(C.rec_pid, SIGINT, 5000);   /* finish the MP4 */
    C.rec_pid = 0;
    stop_preview();
    if (C.timer) lv_timer_delete(C.timer);
    free_shots();
    zos_img_free(C.last_thumb);
    zos_img_free(C.photo);
    lv_free(C.canvas_buf);
    lv_free(C.frame);
    memset(&C, 0, sizeof C);
}

const zos_app_t zos_app_camera = {
    .id = "camera", .name = "Camera", .tagline = "Photos and video, 8 MP",
    .icon = &zos_icon_camera, .color = 0x12a594, .open = camera_open, .close = camera_close,
};
