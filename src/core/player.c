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
#include "player.h"

extern char **environ;

static struct {
    zos_player_state_t st;
    zos_player_cbs_t cbs;
    void *ud;
    pid_t pid;
    int fd;
    char sock[256];
    char rx[8192];
    size_t rxlen;
    lv_timer_t *timer;
    uint32_t started_at;
    bool eof;
    bool suspended;
} P = {.fd = -1};

static void send_cmd(const char *json)
{
    if (P.fd < 0) return;
    size_t n = strlen(json);
    if (write(P.fd, json, n) != (ssize_t)n || write(P.fd, "\n", 1) != 1) { /* mpv gone: the timer notices */ }
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
                if (!strcmp(name->valuestring, "time-pos") && cJSON_IsNumber(data)) P.st.pos = data->valuedouble;
                else if (!strcmp(name->valuestring, "duration") && cJSON_IsNumber(data)) P.st.dur = data->valuedouble;
                else if (!strcmp(name->valuestring, "pause")) P.st.paused = cJSON_IsTrue(data);
                else if (!strcmp(name->valuestring, "paused-for-cache")) P.st.buffering = cJSON_IsTrue(data);
                else if (!strcmp(name->valuestring, "volume") && cJSON_IsNumber(data)) P.st.volume = (int)data->valuedouble;
                changed = true;
            }
        } else if (!strcmp(ev->valuestring, "end-file")) {
            const cJSON *reason = cJSON_GetObjectItem(j, "reason");
            P.eof = cJSON_IsString(reason) && !strcmp(reason->valuestring, "eof");
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
    send_cmd("{\"command\":[\"observe_property\",1,\"time-pos\"]}");
    send_cmd("{\"command\":[\"observe_property\",2,\"duration\"]}");
    send_cmd("{\"command\":[\"observe_property\",3,\"pause\"]}");
    send_cmd("{\"command\":[\"observe_property\",4,\"paused-for-cache\"]}");
    send_cmd("{\"command\":[\"observe_property\",5,\"volume\"]}");
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
        char buf[2048];
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

bool zos_player_start(const char *url, double start_s, bool video, const char *title,
                      zos_player_cbs_t cbs, void *ud)
{
    if (P.st.running) zos_player_stop();
    memset(&P.st, 0, sizeof P.st);
    snprintf(P.sock, sizeof P.sock, "%s/mpv-%d.sock", zos_cfg.state_dir, (int)getpid());
    unlink(P.sock);

    char ipc[300], start[48], ttl[200];
    snprintf(ipc, sizeof ipc, "--input-ipc-server=%s", P.sock);
    snprintf(start, sizeof start, "--start=%.1f", start_s > 0 ? start_s : 0);
    snprintf(ttl, sizeof ttl, "--force-media-title=%s", title ? title : "");
    const char *argv[48];
    int a = 0;
    argv[a++] = "mpv";
    argv[a++] = ipc;
    argv[a++] = start;
    argv[a++] = ttl;
    argv[a++] = "--no-terminal";
    argv[a++] = "--idle=no";
    argv[a++] = "--keep-open=no";
    argv[a++] = "--cache=yes";
    argv[a++] = "--demuxer-max-bytes=16MiB";   /* 512 MB of RAM in total */
    if (!video) {
        argv[a++] = "--no-video";
        argv[a++] = "--force-window=no";
    } else if (zos_on_device()) {
        argv[a++] = "--vo=drm";
        argv[a++] = "--hwdec=v4l2m2m-copy";
        argv[a++] = "--profile=fast";
        argv[a++] = "--input-default-bindings=no";
    }
    static char extra[512];
    const char *ex = getenv("ZOS_MPV_ARGS");   /* e.g. "--vo=null --ao=null" for tests */
    if (ex) {
        snprintf(extra, sizeof extra, "%s", ex);
        for (char *tok = strtok(extra, " "); tok && a < 44; tok = strtok(NULL, " ")) argv[a++] = tok;
    }
    argv[a++] = "--";
    argv[a++] = url;
    argv[a] = NULL;

    pid_t pid;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);   /* keep mpv off our tty */
    int rc = posix_spawnp(&pid, "mpv", &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;

    P.pid = pid;
    P.cbs = cbs;
    P.ud = ud;
    P.eof = false;
    P.rxlen = 0;
    P.st.running = true;
    P.st.video = video;
    P.st.volume = 100;
    P.st.buffering = true;
    snprintf(P.st.title, sizeof P.st.title, "%s", title ? title : "");
    P.started_at = lv_tick_get();
    P.timer = lv_timer_create(tick, 50, NULL);
    if (video && zos_on_device()) {
        zos_display_suspend(true);
        P.suspended = true;
    }
    return true;
}

void zos_player_toggle_pause(void) { send_cmd("{\"command\":[\"cycle\",\"pause\"]}"); }

void zos_player_seek(double rel)
{
    char c[96];
    snprintf(c, sizeof c, "{\"command\":[\"seek\",%.1f,\"relative\"]}", rel);
    send_cmd(c);
}

void zos_player_volume(int delta)
{
    char c[96];
    snprintf(c, sizeof c, "{\"command\":[\"add\",\"volume\",%d]}", delta);
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
