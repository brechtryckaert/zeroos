#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../zos.h"
#include "hdmi.h"
#include "player.h"
#include "settings.h"

extern char **environ;

static struct {
    zos_player_state_t st;
    zos_player_cbs_t cbs;
    void *ud;
    pid_t pid;
    int fd;
    char sock[256];
    char rx[32768];
    size_t rxlen;
    lv_timer_t *timer;
    uint32_t started_at;
    bool eof;
    bool suspended;
    bool hdmi_held;
    bool restarting;            /* switching output: don't report the end */
    char pending[8][256];       /* commands to send once mpv's IPC is up */
    int npending;
    /* the current request, to restart it on another output */
    char *url, *title, *playlist;
    zos_player_opts_t opts;
} P = {.fd = -1};

static void queue_cmd(const char *json)
{
    if (P.npending < 8) snprintf(P.pending[P.npending++], sizeof P.pending[0], "%s", json);
}

static void send_cmd(const char *json)
{
    if (P.fd < 0) return;
    size_t n = strlen(json);
    if (write(P.fd, json, n) != (ssize_t)n || write(P.fd, "\n", 1) != 1) { /* mpv gone: the timer notices */ }
}

static void copy_str(char *dst, size_t n, const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    snprintf(dst, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static void parse_tracks(const cJSON *arr)
{
    P.st.ntracks = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, arr) {
        if (P.st.ntracks >= ZOS_PLAYER_MAX_TRACKS) break;
        zos_track_t *k = &P.st.tracks[P.st.ntracks++];
        memset(k, 0, sizeof *k);
        const cJSON *id = cJSON_GetObjectItem(t, "id");
        k->id = cJSON_IsNumber(id) ? id->valueint : 0;
        copy_str(k->type, sizeof k->type, t, "type");
        copy_str(k->lang, sizeof k->lang, t, "lang");
        copy_str(k->title, sizeof k->title, t, "title");
        copy_str(k->codec, sizeof k->codec, t, "codec");
        k->selected = cJSON_IsTrue(cJSON_GetObjectItem(t, "selected"));
        k->external = cJSON_IsTrue(cJSON_GetObjectItem(t, "external"));
        k->def = cJSON_IsTrue(cJSON_GetObjectItem(t, "default"));
        k->forced = cJSON_IsTrue(cJSON_GetObjectItem(t, "forced"));
    }
}

static void handle_line(const char *line)
{
    cJSON *j = cJSON_Parse(line);
    if (!j) return;
    const cJSON *ev = cJSON_GetObjectItem(j, "event");
    bool changed = false;
    if (cJSON_IsString(ev)) {
        if (!strcmp(ev->valuestring, "property-change")) {
            const cJSON *name = cJSON_GetObjectItem(j, "name"), *data = cJSON_GetObjectItem(j, "data");
            if (cJSON_IsString(name)) {
                const char *n = name->valuestring;
                if (!strcmp(n, "time-pos") && cJSON_IsNumber(data)) P.st.pos = data->valuedouble;
                else if (!strcmp(n, "duration") && cJSON_IsNumber(data)) P.st.dur = data->valuedouble;
                else if (!strcmp(n, "pause")) P.st.paused = cJSON_IsTrue(data);
                else if (!strcmp(n, "paused-for-cache")) P.st.buffering = cJSON_IsTrue(data);
                else if (!strcmp(n, "volume") && cJSON_IsNumber(data)) P.st.volume = (int)data->valuedouble;
                else if (!strcmp(n, "playlist-pos") && cJSON_IsNumber(data)) P.st.playlist_pos = data->valueint;
                else if (!strcmp(n, "playlist-count") && cJSON_IsNumber(data)) P.st.playlist_count = data->valueint;
                else if (!strcmp(n, "track-list") && cJSON_IsArray(data)) parse_tracks(data);
                else if (!strcmp(n, "media-title") && cJSON_IsString(data) && !P.st.title[0])
                    snprintf(P.st.title, sizeof P.st.title, "%s", data->valuestring);
                changed = true;
            }
        } else if (!strcmp(ev->valuestring, "end-file")) {
            const cJSON *reason = cJSON_GetObjectItem(j, "reason");
            P.eof = cJSON_IsString(reason) && !strcmp(reason->valuestring, "eof");
        } else if (!strcmp(ev->valuestring, "start-file")) {
            P.st.pos = 0;
            P.st.dur = 0;
            changed = true;
        }
    }
    cJSON_Delete(j);
    if (changed && P.cbs.on_change) P.cbs.on_change(P.ud);
}

static void try_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", P.sock);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    P.fd = fd;
    static const char *props[] = {"time-pos", "duration", "pause", "paused-for-cache", "volume",
                                  "playlist-pos", "playlist-count", "track-list", "media-title"};
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        char c[128];
        snprintf(c, sizeof c, "{\"command\":[\"observe_property\",%d,\"%s\"]}", (int)i + 1, props[i]);
        send_cmd(c);
    }
    for (int i = 0; i < P.npending; i++) send_cmd(P.pending[i]);
    P.npending = 0;
}

static void finish(void)
{
    double last = P.st.pos;
    bool eof = P.eof;
    if (P.fd >= 0) close(P.fd);
    P.fd = -1;
    unlink(P.sock);
    lv_timer_delete(P.timer);
    P.timer = NULL;
    P.st.running = false;
    if (P.suspended) {
        zos_display_suspend(false);
        P.suspended = false;
    }
    if (P.hdmi_held) {
        zos_hdmi_hold(false);
        P.hdmi_held = false;
    }
    P.npending = 0;
    if (P.restarting) return;   /* zos_player_play follows with the same callbacks */
    zos_player_cbs_t cbs = P.cbs;
    void *ud = P.ud;
    memset(&P.cbs, 0, sizeof P.cbs);
    if (cbs.on_end) cbs.on_end(eof, last, ud);
}

static void tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (P.fd < 0 && lv_tick_get() - P.started_at < 15000) try_connect();
    if (P.fd >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(P.fd, buf, sizeof buf)) > 0) {
            if (P.rxlen + (size_t)n >= sizeof P.rx) P.rxlen = 0;   /* overlong line: drop it */
            memcpy(P.rx + P.rxlen, buf, (size_t)n);
            P.rxlen += (size_t)n;
            char *nl;
            while ((nl = memchr(P.rx, '\n', P.rxlen))) {
                *nl = 0;
                handle_line(P.rx);
                size_t used = (size_t)(nl - P.rx) + 1;
                memmove(P.rx, nl + 1, P.rxlen - used);
                P.rxlen -= used;
            }
        }
    }
    int status;
    if (waitpid(P.pid, &status, WNOHANG) == P.pid) finish();
}

zos_out_t zos_player_video_target(void)
{
    if (zos_hdmi_connected() && zos_hdmi_card() && zos_setting_int("video_hdmi", 1)) return ZOS_OUT_HDMI;
    return ZOS_OUT_PANEL;
}

#define MAXARG 64
static char argbuf[14][320];

bool zos_player_play(const char *url, const zos_player_opts_t *o, zos_player_cbs_t cbs, void *ud)
{
    if (P.st.running) zos_player_stop();
    memset(&P.st, 0, sizeof P.st);
    /* keep a copy of the request (restarts on hot-plug); o may point at P.opts */
    zos_player_opts_t keep = *o;
    char *u = url ? strdup(url) : NULL, *t = o->title ? strdup(o->title) : NULL,
         *pl = o->playlist ? strdup(o->playlist) : NULL;
    free(P.url); free(P.title); free(P.playlist);
    P.url = u; P.title = t; P.playlist = pl;
    P.opts = keep;
    P.opts.title = P.title;
    P.opts.playlist = P.playlist;
    o = &P.opts;
    url = P.url;
    snprintf(P.sock, sizeof P.sock, "%s/mpv-%d.sock", zos_cfg.state_dir, (int)getpid());
    unlink(P.sock);

    zos_out_t out = o->video ? (o->out == ZOS_OUT_AUTO ? zos_player_video_target() : o->out) : ZOS_OUT_AUTO;
    bool hdmi = o->video && out == ZOS_OUT_HDMI;

    const char *argv[MAXARG];
    int a = 0, b = 0;
    argv[a++] = "mpv";
    snprintf(argbuf[b], sizeof argbuf[b], "--input-ipc-server=%s", P.sock); argv[a++] = argbuf[b++];
    snprintf(argbuf[b], sizeof argbuf[b], "--start=%.1f", o->start > 0 ? o->start : 0); argv[a++] = argbuf[b++];
    if (o->title) {
        snprintf(argbuf[b], sizeof argbuf[b], "--force-media-title=%s", o->title);
        argv[a++] = argbuf[b++];
    }
    argv[a++] = "--no-terminal";
    argv[a++] = "--idle=no";
    argv[a++] = "--keep-open=no";
    argv[a++] = "--cache=yes";
    argv[a++] = "--demuxer-max-bytes=16MiB";   /* 512 MB of RAM in total */
    argv[a++] = "--input-default-bindings=no";
    /* Every playback starts at the media volume from Settings (0 included). */
    int vol = zos_setting_int("volume", 80);
    if (vol < 0) vol = 0;
    if (vol > 130) vol = 130;
    snprintf(argbuf[b], sizeof argbuf[b], "--volume=%d", vol);
    argv[a++] = argbuf[b++];
    if (o->loop && !strcmp(o->loop, "file")) argv[a++] = "--loop-file=inf";
    if (o->loop && !strcmp(o->loop, "playlist")) argv[a++] = "--loop-playlist=inf";
    if (!o->video) {
        argv[a++] = "--no-video";
        argv[a++] = "--force-window=no";
    } else {
        const char *slang = zos_setting_str("sub_lang", "");
        if (slang[0]) {
            snprintf(argbuf[b], sizeof argbuf[b], "--slang=%s", slang);
            argv[a++] = argbuf[b++];
        }
        const char *alang = zos_setting_str("audio_lang", "");
        if (alang[0]) {
            snprintf(argbuf[b], sizeof argbuf[b], "--alang=%s", alang);
            argv[a++] = argbuf[b++];
        }
        argv[a++] = "--sub-auto=fuzzy";            /* film.srt, film.en.srt next to the file */
        if (zos_on_device()) {
            argv[a++] = "--vo=drm";
            argv[a++] = "--hwdec=auto-copy";        /* v4l2m2m H.264 on the CM0 */
            argv[a++] = "--profile=fast";
            if (hdmi) {
                snprintf(argbuf[b], sizeof argbuf[b], "--drm-device=%s", zos_hdmi_card());
                argv[a++] = argbuf[b++];
                argv[a++] = "--drm-mode=preferred";
            } else {
                const char *panel = getenv("ZOS_PANEL_DRM");
                if (panel) {
                    snprintf(argbuf[b], sizeof argbuf[b], "--drm-device=%s", panel);
                    argv[a++] = argbuf[b++];
                }
                argv[a++] = "--sub-scale=1.4";     /* readable on 170 lines */
            }
        }
        if (hdmi && zos_setting_int("hdmi_audio", 1)) {
            snprintf(argbuf[b], sizeof argbuf[b], "--audio-device=%s", zos_hdmi_audio_device());
            argv[a++] = argbuf[b++];
        }
    }
    static char extra[512];
    const char *ex = getenv("ZOS_MPV_ARGS");   /* e.g. "--vo=null --ao=null" for tests */
    if (ex) {
        snprintf(extra, sizeof extra, "%s", ex);
        for (char *tok = strtok(extra, " "); tok && a < MAXARG - 6; tok = strtok(NULL, " ")) argv[a++] = tok;
    }
    if (o->playlist) {
        snprintf(argbuf[b], sizeof argbuf[b], "--playlist=%s", o->playlist); argv[a++] = argbuf[b++];
        snprintf(argbuf[b], sizeof argbuf[b], "--playlist-start=%d", o->playlist_start); argv[a++] = argbuf[b++];
    } else {
        argv[a++] = "--";
        argv[a++] = url;
    }
    argv[a] = NULL;

    if (hdmi) {
        zos_hdmi_hold(true);      /* mpv needs the HDMI output for itself */
        P.hdmi_held = true;
    }
    pid_t pid;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);   /* keep mpv off our tty */
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (!getenv("ZOS_MPV_DEBUG")) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    int rc = posix_spawnp(&pid, "mpv", &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        if (P.hdmi_held) { zos_hdmi_hold(false); P.hdmi_held = false; }
        return false;
    }

    P.pid = pid;
    P.cbs = cbs;
    P.ud = ud;
    P.eof = false;
    P.rxlen = 0;
    P.st.running = true;
    P.st.video = o->video;
    P.st.on_hdmi = hdmi;
    P.st.volume = vol;
    P.st.buffering = true;
    snprintf(P.st.title, sizeof P.st.title, "%s", o->title ? o->title : "");
    P.started_at = lv_tick_get();
    P.timer = lv_timer_create(tick, 50, NULL);
    if (o->video && !hdmi && zos_on_device()) {
        zos_display_suspend(true);
        P.suspended = true;
    }
    return true;
}

bool zos_player_start(const char *url, double start_s, bool video, const char *title,
                      zos_player_cbs_t cbs, void *ud)
{
    zos_player_opts_t o = {.start = start_s, .video = video, .title = title};
    return zos_player_play(url, &o, cbs, ud);
}

/* osd-msg / osd-msg-bar: mpv shows what happened on the video itself
 * (paused, the seek bar, the volume), which is the only feedback there is
 * while a video fills the panel. */
void zos_player_toggle_pause(void) { send_cmd("{\"command\":[\"osd-msg\",\"cycle\",\"pause\"]}"); }

void zos_player_toggle_mute(void) { send_cmd("{\"command\":[\"osd-msg\",\"cycle\",\"mute\"]}"); }

void zos_player_osd(const char *text, int ms)
{
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateString("show-text"));
    cJSON_AddItemToArray(a, cJSON_CreateString(text));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(ms));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "command", a);
    char *j = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (P.fd >= 0) send_cmd(j);
    else if (P.st.running) queue_cmd(j);
    free(j);
}

void zos_player_seek(double rel)
{
    char c[96];
    snprintf(c, sizeof c, "{\"command\":[\"osd-msg-bar\",\"seek\",%.1f,\"relative\"]}", rel);
    send_cmd(c);
}

void zos_player_seek_abs(double s)
{
    char c[96];
    snprintf(c, sizeof c, "{\"command\":[\"seek\",%.1f,\"absolute\"]}", s);
    send_cmd(c);
}

void zos_player_volume(int delta)
{
    int v = zos_setting_int("volume", 80) + delta;
    v = v < 0 ? 0 : v > 130 ? 130 : v;
    zos_setting_set_int("volume", v);
    char c[96];
    snprintf(c, sizeof c, "{\"command\":[\"osd-msg-bar\",\"set\",\"volume\",\"%d\"]}", v);
    send_cmd(c);
    P.st.volume = v;
}

void zos_player_command(const char *cmd)
{
    /* "cycle sub" -> {"command":["cycle","sub"]} */
    char c[256], words[128];
    snprintf(words, sizeof words, "%s", cmd);
    size_t n = (size_t)snprintf(c, sizeof c, "{\"command\":[");
    int i = 0;
    for (char *save = NULL, *w = strtok_r(words, " ", &save); w; w = strtok_r(NULL, " ", &save))
        n += (size_t)snprintf(c + n, sizeof c - n, "%s\"%s\"", i++ ? "," : "", w);
    snprintf(c + n, sizeof c - n, "]}");
    send_cmd(c);
}

void zos_player_set(const char *prop, const char *json_value)
{
    char c[256];
    snprintf(c, sizeof c, "{\"command\":[\"set_property\",\"%s\",%s]}", prop, json_value);
    send_cmd(c);
}

void zos_player_stop(void)
{
    if (!P.st.running) return;
    send_cmd("{\"command\":[\"quit\"]}");
    /* If IPC never came up, make sure it still goes away. */
    if (P.fd < 0) kill(P.pid, SIGTERM);
    int status;
    for (int i = 0; i < 40 && waitpid(P.pid, &status, WNOHANG) == 0; i++) usleep(25000);
    if (waitpid(P.pid, &status, WNOHANG) == 0) {
        kill(P.pid, SIGKILL);
        waitpid(P.pid, &status, 0);
    }
    finish();
}

const zos_player_state_t *zos_player_state(void) { return &P.st; }

/* A monitor was plugged in or out (hdmi.c). A video playing on the "wrong"
 * output is restarted on the right one, at the same spot, with the same
 * audio and subtitle tracks. */
void zos_player_hdmi_changed(void)
{
    if (!P.st.running || !P.st.video || P.opts.out != ZOS_OUT_AUTO) return;
    bool want_hdmi = zos_player_video_target() == ZOS_OUT_HDMI;
    if (want_hdmi == P.st.on_hdmi) return;
    int sid = -1, aid = -1;
    bool sub_off = true;
    for (int i = 0; i < P.st.ntracks; i++) {
        const zos_track_t *k = &P.st.tracks[i];
        if (!k->selected) continue;
        if (!strcmp(k->type, "sub")) { sid = k->id; sub_off = false; }
        if (!strcmp(k->type, "audio")) aid = k->id;
    }
    bool paused = P.st.paused;
    double at = P.st.pos > 2 ? P.st.pos - 2 : 0;
    int ppos = P.st.playlist_pos;
    zos_player_cbs_t cbs = P.cbs;
    void *ud = P.ud;

    P.restarting = true;
    zos_player_stop();
    P.restarting = false;
    P.opts.start = at;
    P.opts.playlist_start = ppos > 0 ? ppos : P.opts.playlist_start;
    if (!zos_player_play(P.url, &P.opts, cbs, ud)) {
        if (cbs.on_end) cbs.on_end(false, at, ud);
        return;
    }
    char c[128];
    if (aid >= 0) { snprintf(c, sizeof c, "{\"command\":[\"set_property\",\"aid\",%d]}", aid); queue_cmd(c); }
    if (sid >= 0) { snprintf(c, sizeof c, "{\"command\":[\"set_property\",\"sid\",%d]}", sid); queue_cmd(c); }
    else if (sub_off) queue_cmd("{\"command\":[\"set_property\",\"sid\",\"no\"]}");
    if (paused) queue_cmd("{\"command\":[\"set_property\",\"pause\",true]}");
    zos_notify(0x8e4ec6, "Video", want_hdmi ? "Moved to the monitor" : "Moved back to this screen");
}
