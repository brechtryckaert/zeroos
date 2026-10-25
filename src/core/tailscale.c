#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "http.h"
#include "tailscale.h"
#include "task.h"

static const char *sock_path(void)
{
    const char *e = getenv("ZOS_TAILSCALE_SOCKET");
    return e && *e ? e : "/var/run/tailscale/tailscaled.sock";
}

static bool call(const char *method, const char *path, const char *body, zos_http_resp_t *r)
{
    char url[160];
    snprintf(url, sizeof url, "http://local-tailscaled.sock/localapi/v0/%s", path);
    zos_http_req_t q = {.method = method, .url = url, .body = body, .unix_socket = sock_path(), .timeout_s = 8};
    return zos_http(&q, r);
}

static void copy(char *dst, size_t cap, const cJSON *j)
{
    dst[0] = 0;
    if (cJSON_IsString(j) && j->valuestring) snprintf(dst, cap, "%s", j->valuestring);
}

static void strip_dot(char *s)
{
    size_t n = strlen(s);
    if (n && s[n - 1] == '.') s[n - 1] = 0;
}

static void first_ip(char *dst, size_t cap, const cJSON *arr)
{
    dst[0] = 0;
    const cJSON *ip;
    cJSON_ArrayForEach(ip, arr) {
        if (!cJSON_IsString(ip)) continue;
        if (!dst[0] || !strchr(ip->valuestring, ':')) snprintf(dst, cap, "%s", ip->valuestring);
        if (!strchr(dst, ':')) break;   /* prefer IPv4 */
    }
}

static void parse_peer(ts_peer_t *p, const cJSON *j)
{
    memset(p, 0, sizeof *p);
    copy(p->id, sizeof p->id, cJSON_GetObjectItem(j, "ID"));
    copy(p->host, sizeof p->host, cJSON_GetObjectItem(j, "HostName"));
    copy(p->dns, sizeof p->dns, cJSON_GetObjectItem(j, "DNSName"));
    strip_dot(p->dns);
    copy(p->os, sizeof p->os, cJSON_GetObjectItem(j, "OS"));
    copy(p->relay, sizeof p->relay, cJSON_GetObjectItem(j, "Relay"));
    copy(p->last_seen, sizeof p->last_seen, cJSON_GetObjectItem(j, "LastSeen"));
    first_ip(p->ip, sizeof p->ip, cJSON_GetObjectItem(j, "TailscaleIPs"));
    p->online = cJSON_IsTrue(cJSON_GetObjectItem(j, "Online"));
    p->active = cJSON_IsTrue(cJSON_GetObjectItem(j, "Active"));
    p->exit_node = cJSON_IsTrue(cJSON_GetObjectItem(j, "ExitNode"));
    p->exit_option = cJSON_IsTrue(cJSON_GetObjectItem(j, "ExitNodeOption"));
    const cJSON *rx = cJSON_GetObjectItem(j, "RxBytes"), *tx = cJSON_GetObjectItem(j, "TxBytes");
    p->rx = cJSON_IsNumber(rx) ? (int64_t)rx->valuedouble : 0;
    p->tx = cJSON_IsNumber(tx) ? (int64_t)tx->valuedouble : 0;
    /* The DNS short name is what people type; HostName can be "localhost" on some OSes. */
    if (p->dns[0] && (!p->host[0] || !strcmp(p->host, "localhost"))) {
        snprintf(p->host, sizeof p->host, "%.*s", (int)strcspn(p->dns, "."), p->dns);
    }
}

static int peer_cmp(const void *a, const void *b)
{
    const ts_peer_t *x = a, *y = b;
    if (x->online != y->online) return x->online ? -1 : 1;
    return strcasecmp(x->host, y->host);
}

bool ts_fetch_status(ts_status_t *s)
{
    memset(s, 0, sizeof *s);
    struct stat st;
    if (stat(sock_path(), &st) != 0) {
        snprintf(s->err, sizeof s->err, "tailscaled is not running (%s missing)", sock_path());
        return false;
    }
    zos_http_resp_t r;
    if (!call("GET", "status", NULL, &r)) {
        snprintf(s->err, sizeof s->err, "%s", r.status ? "tailscaled refused the request" : r.err);
        if (r.status == 403) snprintf(s->err, sizeof s->err, "Permission denied: run 'sudo tailscale set --operator=$USER'");
        zos_http_free(&r);
        return false;
    }
    cJSON *j = cJSON_Parse(r.body);
    zos_http_free(&r);
    if (!j) {
        snprintf(s->err, sizeof s->err, "unreadable status from tailscaled");
        return false;
    }
    s->reachable = true;
    copy(s->state, sizeof s->state, cJSON_GetObjectItem(j, "BackendState"));
    copy(s->auth_url, sizeof s->auth_url, cJSON_GetObjectItem(j, "AuthURL"));
    copy(s->version, sizeof s->version, cJSON_GetObjectItem(j, "Version"));
    const cJSON *tn = cJSON_GetObjectItem(j, "CurrentTailnet");
    if (tn) copy(s->tailnet, sizeof s->tailnet, cJSON_GetObjectItem(tn, "Name"));
    const cJSON *self = cJSON_GetObjectItem(j, "Self");
    if (self) {
        ts_peer_t me;
        parse_peer(&me, self);
        snprintf(s->self_host, sizeof s->self_host, "%s", me.host);
        snprintf(s->self_dns, sizeof s->self_dns, "%s", me.dns);
        snprintf(s->self_ip, sizeof s->self_ip, "%s", me.ip);
    }
    if (!s->self_ip[0]) first_ip(s->self_ip, sizeof s->self_ip, cJSON_GetObjectItem(j, "TailscaleIPs"));
    const cJSON *h;
    cJSON_ArrayForEach(h, cJSON_GetObjectItem(j, "Health")) {
        if (s->nhealth < 3 && cJSON_IsString(h)) snprintf(s->health[s->nhealth++], sizeof s->health[0], "%s", h->valuestring);
    }
    const cJSON *peers = cJSON_GetObjectItem(j, "Peer");
    int n = cJSON_GetArraySize(peers);
    if (n > 0) {
        s->peers = calloc((size_t)n, sizeof *s->peers);
        const cJSON *p;
        cJSON_ArrayForEach(p, peers) parse_peer(&s->peers[s->npeers++], p);
        qsort(s->peers, (size_t)s->npeers, sizeof *s->peers, peer_cmp);
    }
    cJSON_Delete(j);
    return true;
}

void ts_status_free(ts_status_t *s)
{
    free(s->peers);
    s->peers = NULL;
    s->npeers = 0;
}

static bool simple(const char *method, const char *path, const char *body, char *err, int errlen)
{
    zos_http_resp_t r;
    bool ok = call(method, path, body, &r);
    if (!ok && err) {
        if (r.status == 403) snprintf(err, (size_t)errlen, "Not allowed: run 'sudo tailscale set --operator=$USER' once");
        else if (r.status) {
            /* LocalAPI errors are either plain text or {"error": "..."} */
            cJSON *j = r.body ? cJSON_Parse(r.body) : NULL;
            const cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
            snprintf(err, (size_t)errlen, "%s", cJSON_IsString(e) ? e->valuestring : (r.body ? r.body : "error"));
            cJSON_Delete(j);
            err[strcspn(err, "\n")] = 0;
        } else snprintf(err, (size_t)errlen, "%s", r.err);
    }
    zos_http_free(&r);
    return ok;
}

bool ts_set_running(bool on, char *err, int errlen)
{
    return simple("PATCH", "prefs", on ? "{\"WantRunning\":true,\"WantRunningSet\":true}"
                                       : "{\"WantRunning\":false,\"WantRunningSet\":true}", err, errlen);
}

bool ts_login(char *err, int errlen) { return simple("POST", "login-interactive", NULL, err, errlen); }

bool ts_set_exit_node(const char *id, char *err, int errlen)
{
    char body[160];
    snprintf(body, sizeof body, "{\"ExitNodeID\":\"%s\",\"ExitNodeIDSet\":true}", id ? id : "");
    return simple("PATCH", "prefs", body, err, errlen);
}

/* ---------------- background snapshot ---------------- */

static ts_status_t snap;
static bool have_snap, inflight;
static uint32_t next_at;
static const char owner;   /* jobs belong to this module */

static void poll_work(void *arg) { ts_fetch_status(arg); }

static void poll_done(void *arg)
{
    ts_status_free(&snap);
    snap = *(ts_status_t *)arg;
    ((ts_status_t *)arg)->peers = NULL;   /* ownership moved */
    have_snap = true;
}

static void poll_release(void *arg)
{
    ts_status_free(arg);
    free(arg);
    inflight = false;
}

static void tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    uint32_t now = lv_tick_get();
    if (inflight || (int32_t)(now - next_at) < 0) return;
    struct stat st;
    next_at = now + 15000;
    if (stat(sock_path(), &st) != 0) {   /* not installed / not running: cheap check, no thread */
        ts_status_free(&snap);
        memset(&snap, 0, sizeof snap);
        have_snap = true;
        return;
    }
    inflight = true;
    zos_task_run(&owner, poll_work, poll_done, poll_release, calloc(1, sizeof(ts_status_t)));
}

void ts_service_init(void) { lv_timer_create(tick, 500, NULL); }
const ts_status_t *ts_snapshot(void) { return have_snap ? &snap : NULL; }
void ts_poke(void) { next_at = lv_tick_get(); }
bool ts_connected(void) { return have_snap && snap.reachable && !strcmp(snap.state, "Running"); }
