/* Software updates (see update.h and tools/image/files/zeroos-update).
 *
 * Environment, for the simulator and tests:
 *   ZOS_UPDATE_HELPER   the helper [/usr/local/sbin/zeroos-update]
 *   ZOS_UPDATE_SUDO=0   run it directly instead of through sudo -n
 *   ZOS_ROOT            the helper's fake root (where its state files are) */
#include "update.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../zos.h"
#include "proc.h"
#include "settings.h"
#include "task.h"

#define CONFIRM_AFTER_MS (30 * 1000)
#define AUTO_FIRST_MS (90 * 1000)
#define AUTO_EVERY_MS (60 * 60 * 1000)
#define AUTO_INTERVAL_S (24 * 60 * 60)

static const char owner_tag;
static zos_update_info_t info;
static bool checking, quiet;
static unsigned generation;

static const char *helper(void)
{
    const char *h = getenv("ZOS_UPDATE_HELPER");
    return h && *h ? h : "/usr/local/sbin/zeroos-update";
}

static bool use_sudo(void)
{
    const char *s = getenv("ZOS_UPDATE_SUDO");
    return !(s && !strcmp(s, "0"));
}

static void state_path(char *out, size_t n, const char *rel)
{
    const char *r = getenv("ZOS_ROOT");
    snprintf(out, n, "%s%s", r ? r : "", rel);
}

/* argv for the helper: [sudo -n] helper a b c */
static int helper_argv(const char **argv, const char *a, const char *b, const char *c)
{
    int n = 0;
    if (use_sudo()) { argv[n++] = "sudo"; argv[n++] = "-n"; }
    argv[n++] = helper();
    if (a) argv[n++] = a;
    if (b) argv[n++] = b;
    if (c) argv[n++] = c;
    argv[n] = NULL;
    return n;
}

bool zos_update_available(void) { return access(helper(), X_OK) == 0; }
const zos_update_info_t *zos_update_info(void) { return &info; }
bool zos_update_checking(void) { return checking; }
unsigned zos_update_generation(void) { return generation; }

static void read_installed(char *out, size_t n)
{
    char p[512], l[160];
    out[0] = 0;
    state_path(p, sizeof p, "/etc/zeroos-release");
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(l, sizeof l, f))
        if (!strncmp(l, "ZEROOS_VERSION=", 15)) {
            snprintf(out, n, "%s", l + 15);
            out[strcspn(out, "\r\n")] = 0;
        }
    fclose(f);
}

/* ---------------- check ---------------- */

typedef struct {
    zos_update_info_t r;
    char out[16384];
} check_job_t;

static void str_item(cJSON *j, const char *k, char *out, size_t n)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    if (cJSON_IsString(v)) snprintf(out, n, "%s", v->valuestring);
}

static void check_work(void *a)
{
    check_job_t *c = a;
    const char *argv[8];
    helper_argv(argv, "check", "--json", NULL);
    int rc = zos_run(argv, c->out, sizeof c->out, 60000);
    read_installed(c->r.installed, sizeof c->r.installed);
    /* The helper prints one JSON line; sudo or the shell may add others. */
    char *js = strchr(c->out, '{');
    cJSON *j = js ? cJSON_Parse(js) : NULL;
    if (!j) {
        char *nl = strchr(c->out, '\n');
        if (nl) *nl = 0;
        snprintf(c->r.error, sizeof c->r.error, "%s",
                 rc == -1 ? "The update helper did not answer" : c->out[0] ? c->out : "The update helper failed");
        return;
    }
    str_item(j, "installed", c->r.installed, sizeof c->r.installed);
    str_item(j, "installed_tag", c->r.installed_tag, sizeof c->r.installed_tag);
    str_item(j, "latest", c->r.latest, sizeof c->r.latest);
    str_item(j, "error", c->r.error, sizeof c->r.error);
    str_item(j, "notes", c->r.notes, sizeof c->r.notes);
    char pub[40] = "";
    str_item(j, "published", pub, sizeof pub);
    snprintf(c->r.published, sizeof c->r.published, "%.10s", pub);
    cJSON *u = cJSON_GetObjectItem(j, "update");
    c->r.update = cJSON_IsTrue(u) && !c->r.error[0];
    cJSON *s = cJSON_GetObjectItem(j, "size");
    if (cJSON_IsNumber(s)) c->r.size = (long)s->valuedouble;
    cJSON_Delete(j);
}

static void check_done(void *a)
{
    check_job_t *c = a;
    info = c->r;
    info.checked = time(NULL);
    checking = false;
    generation++;
    if (!info.error[0]) zos_setting_set_int("update_last", (int)(info.checked / 60));
    /* Tell once per new version, wherever the user is (unless they are
     * looking at the Software update page already). */
    if (info.update && strcmp(zos_setting_str("update_notified", ""), info.latest) != 0) {
        if (!quiet) {
            char t[160];
            snprintf(t, sizeof t, "ZeroOS %s is available: Settings \xe2\x80\xba Software update", info.latest);
            zos_notify(0x2563eb, "Software update", t);
        }
        zos_setting_set_str("update_notified", info.latest);
    }
}

static void check_release(void *a) { free(a); }

static void check(bool from_user)
{
    if (checking) return;
    quiet = from_user;
    if (!zos_update_available()) {
        memset(&info, 0, sizeof info);
        read_installed(info.installed, sizeof info.installed);
        snprintf(info.error, sizeof info.error, "Updates need zeroos-update (installed with the ZeroOS image)");
        info.checked = time(NULL);
        generation++;
        return;
    }
    checking = true;
    generation++;
    zos_task_run(&owner_tag, check_work, check_done, check_release, calloc(1, sizeof(check_job_t)));
}

void zos_update_check(void) { check(true); }

/* ---------------- install / status / rollback ---------------- */

bool zos_update_status(zos_update_status_t *st)
{
    memset(st, 0, sizeof *st);
    snprintf(st->state, sizeof st->state, "idle");
    char p[512], buf[1024];
    state_path(p, sizeof p, "/run/zeroos-update/status");
    FILE *f = fopen(p, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) return false;
    str_item(j, "state", st->state, sizeof st->state);
    str_item(j, "msg", st->msg, sizeof st->msg);
    str_item(j, "version", st->version, sizeof st->version);
    cJSON *v = cJSON_GetObjectItem(j, "pct");
    if (cJSON_IsNumber(v)) st->pct = v->valueint;
    v = cJSON_GetObjectItem(j, "time");
    if (cJSON_IsNumber(v)) st->time = (time_t)v->valuedouble;
    cJSON_Delete(j);
    return true;
}

bool zos_update_busy(void)
{
    zos_update_status_t st;
    if (!zos_update_status(&st)) return false;
    static const char *active[] = {"starting", "checking", "downloading", "unpacking", "installing"};
    for (size_t i = 0; i < sizeof active / sizeof active[0]; i++)
        if (!strcmp(st.state, active[i])) return time(NULL) - st.time < 30 * 60;   /* a stale file is not a running install */
    return false;
}

static bool start_helper(const char *cmd, const char *arg, char *err, size_t n)
{
    if (!zos_update_available()) { snprintf(err, n, "zeroos-update is not installed"); return false; }
    if (zos_update_busy()) { snprintf(err, n, "An update is already running"); return false; }
    const char *argv[8];
    helper_argv(argv, cmd, "--detach", arg);
    zos_run_bg(argv);
    return true;
}

bool zos_update_install(char *err, size_t n)
{
    return start_helper("install", info.latest[0] ? info.latest : NULL, err, n);
}

bool zos_update_rollback_version(char *out, size_t n)
{
    char p[512];
    state_path(p, sizeof p, "/var/lib/zeroos/rollback/files.tar.gz");
    if (access(p, R_OK) != 0) return false;
    state_path(p, sizeof p, "/var/lib/zeroos/rollback/VERSION");
    FILE *f = fopen(p, "r");
    out[0] = 0;
    if (f) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
        fclose(f);
    }
    if (!out[0]) snprintf(out, n, "the previous version");
    return true;
}

bool zos_update_rollback(char *err, size_t n) { return start_helper("rollback", NULL, err, n); }

/* ---------------- startup ---------------- */

static void confirm_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    const char *argv[8];
    helper_argv(argv, "confirm", NULL, NULL);
    zos_run_bg(argv);
}

static void auto_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!zos_setting_int("update_auto", 1) || !zos_update_available() || zos_update_busy()) return;
    long last = (long)zos_setting_int("update_last", 0) * 60;
    if (time(NULL) - last < AUTO_INTERVAL_S) return;
    check(false);
}

void zos_update_init(void)
{
    char p[512], l[200];
    read_installed(info.installed, sizeof info.installed);
    bool confirm = false;
    state_path(p, sizeof p, "/var/lib/zeroos/update-pending");
    if (access(p, F_OK) == 0) confirm = true;
    state_path(p, sizeof p, "/var/lib/zeroos/update-result");
    FILE *f = fopen(p, "r");
    if (f) {
        char what[24] = "", a[64] = "", b[64] = "", t[200];
        if (fgets(l, sizeof l, f)) sscanf(l, "%23s %63s %63s", what, a, b);
        fclose(f);
        if (!strcmp(what, "updated")) {
            snprintf(t, sizeof t, "Now running ZeroOS %s", a);
            zos_notify(0x22c55e, "Software update", t);
        } else if (!strcmp(what, "rolledback")) {
            snprintf(t, sizeof t, "Back on ZeroOS %s", a);
            zos_notify(0x52525b, "Software update", t);
        } else if (!strcmp(what, "failed")) {
            snprintf(t, sizeof t, "ZeroOS %s did not start, so %s is back", b, a);
            zos_notify(0xe5484d, "Software update", t);
        }
        confirm = true;
    }
    if (confirm && zos_update_available()) {
        lv_timer_t *t = lv_timer_create(confirm_cb, CONFIRM_AFTER_MS, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
    lv_timer_t *t = lv_timer_create(auto_cb, AUTO_EVERY_MS, NULL);
    lv_timer_t *first = lv_timer_create(auto_cb, AUTO_FIRST_MS, NULL);
    lv_timer_set_repeat_count(first, 1);
    LV_UNUSED(t);
}
