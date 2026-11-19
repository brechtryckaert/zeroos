/* Settings: Wi-Fi, display & HDMI, sound, date & time, battery & storage,
 * users, about, software update, power.
 *
 * System changes go through the usual tools (nmcli, amixer, timedatectl,
 * systemctl) from worker threads; the image's polkit rule lets the ZeroOS
 * user make them. Brightness writes the backlight in sysfs directly. */
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "../../core/form.h"
#include "../../core/hdmi.h"
#include "../../core/player.h"
#include "../../core/proc.h"
#include "../../core/settings.h"
#include "../../core/task.h"
#include "../../core/ui.h"
#include "../../core/update.h"
#include "../../core/users.h"
#include "../../zos.h"

extern const lv_image_dsc_t zos_icon_settings;
extern const zos_app_t zos_app_settings;

typedef enum { P_HOME, P_WIFI, P_DISPLAY, P_SOUND, P_TIME, P_STORAGE, P_ABOUT, P_POWER, P_UPDATE, P_USERS, P_USER } page_t;

enum {
    /* home */
    A_WIFI = 1, A_DISPLAY, A_SOUND, A_TIME, A_STORAGE, A_ABOUT, A_POWER, A_UPDATE, A_USERS,
    /* wifi */
    A_WIFI_RADIO, A_WIFI_CURRENT, A_WIFI_NET,
    /* display */
    A_BRIGHT, A_MIRROR, A_VIDEO_HDMI, A_HDMI_AUDIO, A_SUBLANG,
    /* sound */
    A_SYSVOL, A_MEDIAVOL, A_TEST,
    /* time */
    A_TZ,
    /* software update */
    A_UPD_CHECK, A_UPD_INSTALL, A_UPD_AUTO, A_UPD_ROLLBACK,
    /* users */
    A_USER_OPEN, A_USER_ADD, A_LOGOUT, A_USER_NAME, A_USER_LOCK, A_USER_ADMIN, A_USER_REMOVE,
    /* power */
    A_RESTART_UI, A_REBOOT, A_SHUTDOWN,
    A_NONE,
};

typedef struct {
    int action, arg;
    const char *sym;
    uint32_t sym_color;
    char title[64], sub[128], right[48];
    bool header;
} srow_t;

#define MAX_ROWS 64
#define MAX_NETS 40

typedef struct {
    char ssid[64];
    int signal;
    bool secured, active, known;
} net_t;

static struct {
    lv_obj_t *scr, *sink, *page_obj, *title;
    lv_group_t *g;
    zos_vlist_t *list;
    lv_obj_t *hint;
    page_t page;
    int home_sel;
    srow_t rows[MAX_ROWS];
    int nrows;
    /* wifi */
    net_t nets[MAX_NETS];
    int nnets;
    bool wifi_on, wifi_loaded, wifi_busy;
    char wifi_msg[160];
    /* sound */
    char mixer_ctl[48];
    int sysvol;
    /* power */
    int confirm;
    int want_sel;      /* selection to apply on the next render (-1 keep) */
    int users_sel;     /* row to return to on the Users page */
    /* forms */
    zos_form_t *form;
    zos_field_t fields[3];
    char form_buf[128], form_buf2[64], form_buf3[64];
    char user_id[12];   /* the user on P_USER */
    char pending_ssid[64];
    int form_kind;      /* 1 wifi password, 2 time zone, 3 subtitle language, 4 add user, 5 rename, 6 lock */
    lv_timer_t *tick;
} S;

static const char owner_tag;
static void render(void);

/* ---------------- rows ---------------- */

static srow_t *add(int action, const char *sym, const char *title, const char *right, const char *subfmt, ...)
{
    if (S.nrows >= MAX_ROWS) return &S.rows[MAX_ROWS - 1];
    srow_t *r = &S.rows[S.nrows++];
    memset(r, 0, sizeof *r);
    r->action = action;
    r->sym = sym;
    snprintf(r->title, sizeof r->title, "%s", title);
    if (right) snprintf(r->right, sizeof r->right, "%s", right);
    if (subfmt) {
        va_list ap;
        va_start(ap, subfmt);
        vsnprintf(r->sub, sizeof r->sub, subfmt, ap);
        va_end(ap);
    }
    return r;
}

static void header(const char *t)
{
    srow_t *r = add(A_NONE, NULL, t, NULL, NULL);
    r->header = true;
}

static void fill(int i, zos_vrow_t *v, void *ud)
{
    LV_UNUSED(ud);
    srow_t *r = &S.rows[i];
    v->header = r->header;
    v->symbol = r->sym;
    v->symbol_color = r->sym_color;
    snprintf(v->title, sizeof v->title, "%s", r->title);
    snprintf(v->sub, sizeof v->sub, "%s", r->sub);
    snprintf(v->right, sizeof v->right, "%s", r->right);
}

static const char *onoff(bool b) { return b ? "On" : "Off"; }

/* ---------------- system info helpers ---------------- */

static void ip_addresses(char *out, size_t n)
{
    out[0] = 0;
    struct ifaddrs *ifa, *i;
    if (getifaddrs(&ifa)) return;
    size_t len = 0;
    for (i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        char a[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a);
        len += (size_t)snprintf(out + len, n > len ? n - len : 0, "%s%s %s", len ? ", " : "", i->ifa_name, a);
    }
    freeifaddrs(ifa);
    if (!out[0]) snprintf(out, n, "not connected");
}

static bool read_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

static bool backlight_dir(char *out, size_t n)
{
    const char *e = getenv("ZOS_BACKLIGHT");
    if (e) { snprintf(out, n, "%s", e); return true; }
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return false;
    struct dirent *de;
    bool ok = false;
    while ((de = readdir(d)))
        if (de->d_name[0] != '.') { snprintf(out, n, "/sys/class/backlight/%s", de->d_name); ok = true; break; }
    closedir(d);
    return ok;
}

static int brightness_get(void)
{
    char dir[256], p[300], v[32];
    if (!backlight_dir(dir, sizeof dir)) return -1;
    snprintf(p, sizeof p, "%s/max_brightness", dir);
    if (!read_line(p, v, sizeof v)) return -1;
    int max = atoi(v);
    snprintf(p, sizeof p, "%s/brightness", dir);
    if (!read_line(p, v, sizeof v) || max <= 0) return -1;
    return (atoi(v) * 100 + max / 2) / max;
}

static bool brightness_set(int pct)
{
    char dir[256], p[300], v[32];
    if (!backlight_dir(dir, sizeof dir)) return false;
    snprintf(p, sizeof p, "%s/max_brightness", dir);
    if (!read_line(p, v, sizeof v)) return false;
    int max = atoi(v);
    if (pct < 5) pct = 5;          /* never fully dark: there'd be no way back */
    if (pct > 100) pct = 100;
    snprintf(p, sizeof p, "%s/brightness", dir);
    FILE *f = fopen(p, "w");
    if (!f) return false;
    fprintf(f, "%d\n", (pct * max + 50) / 100);
    fclose(f);
    zos_setting_set_int("brightness", pct);
    return true;
}

static int battery(char *status, size_t n)
{
    const char *base = getenv("ZOS_POWER_SUPPLY");
    char root[256];
    snprintf(root, sizeof root, "%s", base ? base : "/sys/class/power_supply");
    DIR *d = opendir(root);
    if (!d) return -1;
    struct dirent *de;
    int pct = -1;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[400], v[32];
        snprintf(p, sizeof p, "%s/%s/capacity", root, de->d_name);
        if (!read_line(p, v, sizeof v)) continue;
        pct = atoi(v);
        snprintf(p, sizeof p, "%s/%s/status", root, de->d_name);
        if (!read_line(p, status, n)) status[0] = 0;
        break;
    }
    closedir(d);
    return pct;
}

static void disk(const char *path, char *out, size_t n)
{
    struct statvfs s;
    if (statvfs(path, &s)) { snprintf(out, n, "?"); return; }
    char fr[24], tot[24];
    zos_ui_fmt_size((double)s.f_bavail * s.f_frsize, fr, sizeof fr);
    zos_ui_fmt_size((double)s.f_blocks * s.f_frsize, tot, sizeof tot);
    snprintf(out, n, "%s free of %s", fr, tot);
}

static void meminfo(char *out, size_t n)
{
    FILE *f = fopen("/proc/meminfo", "r");
    long total = 0, avail = 0;
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
        sscanf(line, "MemTotal: %ld kB", &total);
        sscanf(line, "MemAvailable: %ld kB", &avail);
    }
    if (f) fclose(f);
    snprintf(out, n, "%ld MB free of %ld MB", avail / 1024, total / 1024);
}

/* ---------------- wifi (worker) ---------------- */

typedef struct {
    int op;                 /* 0 scan, 1 connect, 2 radio on/off, 3 disconnect, 4 forget, 5 mixer get, 6 mixer set */
    char ssid[64], pass[128], ctl[48];
    int arg;
    int rc;
    char out[8192];
    net_t nets[MAX_NETS];
    int nnets;
    bool radio;
    int vol;
} job_t;

/* nmcli -t escapes ':' as '\:' */
static int split_terse(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line, *w = line; *p; p++) {
        if (*p == '\\' && p[1]) { *w++ = *++p; continue; }
        if (*p == ':' && n < max) { *w++ = 0; f[n++] = w; continue; }
        *w++ = *p;
        if (!p[1]) *w = 0;
    }
    return n;
}

static void wifi_scan(job_t *j)
{
    const char *radio[] = {"nmcli", "radio", "wifi", NULL};
    char r[64] = "";
    zos_run(radio, r, sizeof r, 5000);
    j->radio = strncmp(r, "enabled", 7) == 0;
    char known[2048] = "";
    const char *con[] = {"nmcli", "-t", "-f", "NAME,TYPE", "connection", "show", NULL};
    zos_run(con, known, sizeof known, 8000);
    const char *list[] = {"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL,SECURITY", "device", "wifi", "list", "--rescan", "auto", NULL};
    j->rc = zos_run(list, j->out, sizeof j->out, 20000);
    for (char *save = NULL, *line = strtok_r(j->out, "\n", &save); line && j->nnets < MAX_NETS; line = strtok_r(NULL, "\n", &save)) {
        char *f[4];
        if (split_terse(line, f, 4) < 4 || !f[1][0]) continue;
        bool dup = false;
        for (int k = 0; k < j->nnets; k++)
            if (!strcmp(j->nets[k].ssid, f[1])) {
                dup = true;
                if (!strcmp(f[0], "yes")) j->nets[k].active = true;
            }
        if (dup) continue;
        net_t *nt = &j->nets[j->nnets++];
        snprintf(nt->ssid, sizeof nt->ssid, "%s", f[1]);
        nt->active = !strcmp(f[0], "yes");
        nt->signal = atoi(f[2]);
        nt->secured = f[3][0] && strcmp(f[3], "--");
        char needle[80];
        snprintf(needle, sizeof needle, "%s:802-11-wireless", nt->ssid);
        nt->known = strstr(known, needle) != NULL;
    }
    /* strongest first, connected on top */
    for (int a = 0; a < j->nnets; a++)
        for (int b = a + 1; b < j->nnets; b++) {
            net_t *x = &j->nets[a], *y = &j->nets[b];
            if ((y->active && !x->active) || (y->active == x->active && y->signal > x->signal)) {
                net_t t = *x; *x = *y; *y = t;
            }
        }
}

static void mixer_find(job_t *j)
{
    char out[2048] = "";
    const char *argv[] = {"amixer", "scontrols", NULL};
    zos_run(argv, out, sizeof out, 5000);
    static const char *pref[] = {"Master", "PCM", "Speaker", "Headphone", "DAC", "Digital", "Playback"};
    j->ctl[0] = 0;
    for (size_t i = 0; i < sizeof pref / sizeof pref[0] && !j->ctl[0]; i++) {
        char needle[64];
        snprintf(needle, sizeof needle, "'%s'", pref[i]);
        if (strstr(out, needle)) snprintf(j->ctl, sizeof j->ctl, "%s", pref[i]);
    }
    if (!j->ctl[0]) {   /* first control of any name */
        char *q = strchr(out, '\'');
        char *e = q ? strchr(q + 1, '\'') : NULL;
        if (q && e) snprintf(j->ctl, sizeof j->ctl, "%.*s", (int)(e - q - 1), q + 1);
    }
}

static int mixer_value(const char *ctl)
{
    char out[1024] = "";
    const char *argv[] = {"amixer", "-M", "sget", ctl, NULL};
    if (zos_run(argv, out, sizeof out, 5000) != 0) return -1;
    char *p = strchr(out, '[');
    while (p && !strchr(p, '%')) p = strchr(p + 1, '[');
    return p ? atoi(p + 1) : -1;
}

static void job_work(void *a)
{
    job_t *j = a;
    switch (j->op) {
    case 0: wifi_scan(j); break;
    case 1: {
        if (j->pass[0]) {
            const char *argv[] = {"nmcli", "device", "wifi", "connect", j->ssid, "password", j->pass, NULL};
            j->rc = zos_run(argv, j->out, sizeof j->out, 45000);
        } else if (j->arg) {   /* saved network */
            const char *argv[] = {"nmcli", "connection", "up", "id", j->ssid, NULL};
            j->rc = zos_run(argv, j->out, sizeof j->out, 45000);
        } else {
            const char *argv[] = {"nmcli", "device", "wifi", "connect", j->ssid, NULL};
            j->rc = zos_run(argv, j->out, sizeof j->out, 45000);
        }
        break;
    }
    case 2: {
        const char *argv[] = {"nmcli", "radio", "wifi", j->arg ? "on" : "off", NULL};
        j->rc = zos_run(argv, j->out, sizeof j->out, 10000);
        break;
    }
    case 3: {
        const char *argv[] = {"nmcli", "connection", "down", "id", j->ssid, NULL};
        j->rc = zos_run(argv, j->out, sizeof j->out, 15000);
        break;
    }
    case 4: {
        const char *argv[] = {"nmcli", "connection", "delete", "id", j->ssid, NULL};
        j->rc = zos_run(argv, j->out, sizeof j->out, 15000);
        break;
    }
    case 5:
        mixer_find(j);
        j->vol = j->ctl[0] ? mixer_value(j->ctl) : -1;
        return;
    case 6: {
        char v[16];
        snprintf(v, sizeof v, "%d%%", j->arg);
        const char *argv[] = {"amixer", "-q", "-M", "sset", j->ctl, v, NULL};
        j->rc = zos_run(argv, j->out, sizeof j->out, 5000);
        j->vol = mixer_value(j->ctl);
        return;
    }
    }
    if (j->op != 0) { job_t s = {0}; wifi_scan(&s); memcpy(j->nets, s.nets, sizeof s.nets); j->nnets = s.nnets; j->radio = s.radio; }
}

static void job_done(void *a)
{
    job_t *j = a;
    if (j->op <= 4) {
        memcpy(S.nets, j->nets, sizeof S.nets);
        S.nnets = j->nnets;
        S.wifi_on = j->radio;
        S.wifi_loaded = true;
        S.wifi_busy = false;
        if (j->op == 1) {
            if (j->rc == 0) snprintf(S.wifi_msg, sizeof S.wifi_msg, "Connected to %s", j->ssid);
            else {
                char *nl = strchr(j->out, '\n');
                if (nl) *nl = 0;
                snprintf(S.wifi_msg, sizeof S.wifi_msg, "Could not connect: %s", j->out[0] ? j->out : "error");
            }
        } else if (j->op >= 2 && j->rc != 0) {
            char *nl = strchr(j->out, '\n');
            if (nl) *nl = 0;
            snprintf(S.wifi_msg, sizeof S.wifi_msg, "%s", j->out[0] ? j->out : "Failed");
        }
    } else {
        if (j->ctl[0]) snprintf(S.mixer_ctl, sizeof S.mixer_ctl, "%s", j->ctl);
        S.sysvol = j->vol;
    }
    render();
}

static void job_release(void *a) { free(a); }

static void run_job(int op, const char *ssid, const char *pass, int arg)
{
    job_t *j = calloc(1, sizeof *j);
    j->op = op;
    j->arg = arg;
    if (ssid) snprintf(j->ssid, sizeof j->ssid, "%s", ssid);
    if (pass) snprintf(j->pass, sizeof j->pass, "%s", pass);
    snprintf(j->ctl, sizeof j->ctl, "%s", S.mixer_ctl);
    if (op <= 4) S.wifi_busy = true;
    zos_task_run(&owner_tag, job_work, job_done, job_release, j);
}

/* ---------------- forms ---------------- */

static void close_form(void)
{
    if (!S.form) return;
    lv_obj_delete_async(zos_form_obj(S.form));
    S.form = NULL;
    lv_group_remove_all_objs(S.g);
    lv_group_add_obj(S.g, S.sink);
    lv_group_focus_obj(S.sink);
}

static void tz_job_work(void *a)
{
    job_t *j = a;
    const char *argv[] = {"timedatectl", "set-timezone", j->ssid, NULL};
    j->rc = zos_run(argv, j->out, sizeof j->out, 10000);
}

static void tz_job_done(void *a)
{
    job_t *j = a;
    if (j->rc == 0) {
        setenv("TZ", j->ssid, 1);   /* this process picks it up right away */
        tzset();
        zos_notify(0x52525b, "Time zone", j->ssid);
    } else {
        zos_notify(0xe5484d, "Time zone", j->out[0] ? j->out : "Could not change it");
    }
    render();
}

static void open_page(page_t p);

/* Account forms: errors stay on the form so you can fix them. */
static bool user_form_submit(zos_form_t *f)
{
    char err[128] = "";
    bool ok = true;
    char *nm = S.form_buf;
    while (*nm == ' ') nm++;
    for (size_t l = strlen(nm); l && nm[l - 1] == ' '; l--) nm[l - 1] = 0;
    switch (S.form_kind) {
    case 4: {
        const zos_user_t *u = zos_users_add(nm, S.form_buf2, S.form_buf3, false, err, sizeof err);
        ok = u != NULL;
        if (ok) zos_notify(u->color, "Users", "Added. They log in from the login screen.");
        break;
    }
    case 5: ok = zos_users_rename(S.user_id, nm, err, sizeof err); break;
    case 6:
        ok = zos_users_set_secret(S.user_id, S.form_buf2, S.form_buf3, err, sizeof err);
        if (ok) zos_notify(0x52525b, "Users", S.form_buf2[0] ? "Lock changed" : "Lock removed");
        break;
    default: return false;
    }
    if (!ok) { zos_form_message(f, err, true); return true; }
    memset(S.form_buf2, 0, sizeof S.form_buf2);
    memset(S.form_buf3, 0, sizeof S.form_buf3);
    close_form();
    render();
    return true;
}

static void form_submit(zos_form_t *f, void *ud)
{
    LV_UNUSED(ud);
    if (S.form_kind >= 4 && user_form_submit(f)) return;
    char v[128];
    snprintf(v, sizeof v, "%s", S.form_buf);
    int kind = S.form_kind;
    close_form();
    if (kind == 1) {
        snprintf(S.wifi_msg, sizeof S.wifi_msg, "Connecting to %s\xe2\x80\xa6", S.pending_ssid);
        run_job(1, S.pending_ssid, v, 0);
    } else if (kind == 2 && v[0]) {
        char zp[300];
        snprintf(zp, sizeof zp, "/usr/share/zoneinfo/%s", v);
        if (access(zp, R_OK) != 0) {
            zos_notify(0xe5484d, "Time zone", "Unknown zone, e.g. Europe/Brussels");
        } else {
            job_t *j = calloc(1, sizeof *j);
            snprintf(j->ssid, sizeof j->ssid, "%s", v);
            zos_task_run(&owner_tag, tz_job_work, tz_job_done, job_release, j);
        }
    } else if (kind == 3) {
        zos_setting_set_str("sub_lang", v);
    }
    render();
}

static void form_cancel(zos_form_t *f, void *ud)
{
    LV_UNUSED(f); LV_UNUSED(ud);
    close_form();
}

static void open_user_form(int kind)
{
    const zos_user_t *u = zos_users_find(S.user_id);
    S.form_kind = kind;
    S.form_buf[0] = S.form_buf2[0] = S.form_buf3[0] = 0;
    int n = 0;
    const char *title = "Add a user", *msg = NULL;
    if (kind == 4 || kind == 5) {
        if (kind == 5 && u) snprintf(S.form_buf, sizeof S.form_buf, "%s", u->name);
        S.fields[n++] = (zos_field_t){.label = "Name", .buf = S.form_buf, .cap = ZOS_USER_NAME_MAX, .placeholder = "e.g. Lena"};
    }
    if (kind == 4 || kind == 6) {
        S.fields[n++] = (zos_field_t){.label = "PIN / pass", .buf = S.form_buf2, .cap = sizeof S.form_buf2,
                                      .placeholder = kind == 6 ? "empty: no lock" : "optional", .password = true};
        S.fields[n++] = (zos_field_t){.label = "Repeat", .buf = S.form_buf3, .cap = sizeof S.form_buf3, .password = true};
        msg = "4-8 digits make a PIN, anything longer a password. Leave it empty for no lock.";
    }
    if (kind == 5) title = "Rename";
    if (kind == 6) title = "Lock";
    char t[64];
    if (u && kind != 4) { snprintf(t, sizeof t, "%s \xc2\xb7 %s", title, u->name); title = t; }
    S.form = zos_form_create(S.scr, S.g, title, S.fields, n, form_submit, form_cancel, NULL);
    if (msg) zos_form_message(S.form, msg, false);
    zos_form_focus(S.form);
}

static void open_form(int kind, const char *title, const char *label, const char *initial, const char *ph, bool password)
{
    S.form_kind = kind;
    snprintf(S.form_buf, sizeof S.form_buf, "%s", initial ? initial : "");
    S.fields[0] = (zos_field_t){.label = label, .buf = S.form_buf, .cap = sizeof S.form_buf,
                                .placeholder = ph, .password = password};
    S.form = zos_form_create(S.scr, S.g, title, S.fields, 1, form_submit, form_cancel, NULL);
    zos_form_focus(S.form);
}

/* ---------------- pages ---------------- */

static void build_home(void)
{
    char ip[200], b[64] = "", st[32] = "";
    ip_addresses(ip, sizeof ip);
    int active = -1;
    for (int i = 0; i < S.nnets; i++) if (S.nets[i].active) active = i;
    add(A_WIFI, LV_SYMBOL_WIFI, "Wi-Fi", NULL, "%s", active >= 0 ? S.nets[active].ssid : ip);
    int br = brightness_get();
    char hd[96];
    zos_hdmi_describe(hd, sizeof hd);
    if (br >= 0) snprintf(b, sizeof b, "Brightness %d%% \xc2\xb7 HDMI: %s", br, hd);
    else snprintf(b, sizeof b, "HDMI: %s", hd);
    add(A_DISPLAY, LV_SYMBOL_EYE_OPEN, "Display & HDMI", NULL, "%s", b);
    add(A_SOUND, LV_SYMBOL_VOLUME_MAX, "Sound", NULL, "Media volume %d%%", zos_setting_int("volume", 80));
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char tbuf[64];
    strftime(tbuf, sizeof tbuf, "%H:%M \xc2\xb7 %a %d %b %Y \xc2\xb7 %Z", &tm);
    add(A_TIME, LV_SYMBOL_BELL, "Date & time", NULL, "%s", tbuf);
    int pct = battery(st, sizeof st);
    char dsk[64];
    disk("/", dsk, sizeof dsk);
    if (pct >= 0) add(A_STORAGE, LV_SYMBOL_BATTERY_FULL, "Battery & storage", NULL, "%d%% %s \xc2\xb7 %s", pct, st, dsk);
    else add(A_STORAGE, LV_SYMBOL_SD_CARD, "Battery & storage", NULL, "%s", dsk);
    const zos_user_t *me = zos_user_current();
    if (me) {
        srow_t *ur = add(A_USERS, LV_SYMBOL_LIST, "Users", NULL, "%s%s \xc2\xb7 %d user%s", me->name,
                         me->admin ? " (admin)" : "", zos_users_count(), zos_users_count() == 1 ? "" : "s");
        ur->sym_color = me->color;
    }
    char ver[64] = "";
    FILE *f = fopen("/etc/zeroos-release", "r");
    if (f) { char l[128]; while (fgets(l, sizeof l, f)) sscanf(l, "ZEROOS_VERSION=%63s", ver); fclose(f); }
    add(A_ABOUT, LV_SYMBOL_HOME, "About", NULL, "ZeroOS %s", ver[0] ? ver : "(development build)");
    const zos_update_info_t *u = zos_update_info();
    zos_update_status_t ust;
    zos_update_status(&ust);
    srow_t *r;
    if (zos_update_busy())
        r = add(A_UPDATE, LV_SYMBOL_DOWNLOAD, "Software update", NULL, "%s", ust.msg);
    else if (u->update)
        r = add(A_UPDATE, LV_SYMBOL_DOWNLOAD, "Software update", "New", "ZeroOS %s is available", u->latest);
    else if (u->checked && !u->error[0])
        r = add(A_UPDATE, LV_SYMBOL_DOWNLOAD, "Software update", NULL, "Up to date");
    else
        r = add(A_UPDATE, LV_SYMBOL_DOWNLOAD, "Software update", NULL, "Check for a new ZeroOS");
    if (u->update) r->sym_color = 0x2563eb;
    add(A_POWER, LV_SYMBOL_POWER, "Power", NULL, "Restart or shut down");
}

static void bars(int signal, char *out, size_t n)
{
    snprintf(out, n, "%s %d%%", signal > 75 ? "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86\xe2\x96\x88" : signal > 50 ? "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86" :
             signal > 25 ? "\xe2\x96\x82\xe2\x96\x84" : "\xe2\x96\x82", signal);
}

static void build_wifi(void)
{
    add(A_WIFI_RADIO, LV_SYMBOL_WIFI, "Wi-Fi", S.wifi_loaded ? onoff(S.wifi_on) : "\xe2\x80\xa6", S.wifi_msg[0] ? "%s" : NULL, S.wifi_msg);
    for (int i = 0; i < S.nnets; i++)
        if (S.nets[i].active) {
            char ip[200];
            ip_addresses(ip, sizeof ip);
            srow_t *r = add(A_WIFI_CURRENT, LV_SYMBOL_OK, S.nets[i].ssid, NULL, "Connected \xc2\xb7 %s \xc2\xb7 F forget", ip);
            r->sym_color = 0x22c55e;
            r->arg = i;
        }
    if (!S.wifi_on && S.wifi_loaded) return;
    header(S.wifi_busy ? "NETWORKS \xc2\xb7 SEARCHING\xe2\x80\xa6" : "NETWORKS");
    for (int i = 0; i < S.nnets; i++) {
        if (S.nets[i].active) continue;
        char sig[32];
        bars(S.nets[i].signal, sig, sizeof sig);
        srow_t *r = add(A_WIFI_NET, S.nets[i].secured ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_WIFI, S.nets[i].ssid, sig,
                        "%s%s", S.nets[i].secured ? "Secured" : "Open", S.nets[i].known ? " \xc2\xb7 saved" : "");
        r->arg = i;
    }
}

static void build_display(void)
{
    int br = brightness_get();
    char v[32], hd[96];
    if (br >= 0) {
        snprintf(v, sizeof v, LV_SYMBOL_LEFT " %d%% " LV_SYMBOL_RIGHT, br);
        add(A_BRIGHT, LV_SYMBOL_IMAGE, "Brightness", v, NULL);
    } else {
        add(A_NONE, LV_SYMBOL_IMAGE, "Brightness", "n/a", "No backlight control found");
    }
    header("HDMI");
    zos_hdmi_describe(hd, sizeof hd);
    add(A_NONE, LV_SYMBOL_VIDEO, "Monitor", NULL, "%s", hd);
    add(A_MIRROR, NULL, "Show ZeroOS on the monitor", onoff(zos_hdmi_mirror_enabled()), "Mirror this screen, scaled up");
    add(A_VIDEO_HDMI, NULL, "Play videos on the monitor", onoff(zos_setting_int("video_hdmi", 1)), "This screen becomes the remote");
    add(A_HDMI_AUDIO, NULL, "Video sound through HDMI", onoff(zos_setting_int("hdmi_audio", 1)), "Otherwise speaker / headphones");
    header("SUBTITLES");
    const char *sl = zos_setting_str("sub_lang", "");
    add(A_SUBLANG, NULL, "Preferred subtitle language", sl[0] ? sl : "auto", "e.g. en or nl,en (Enter to change)");
}

static void build_sound(void)
{
    char v[48];
    if (S.mixer_ctl[0] && S.sysvol >= 0) {
        snprintf(v, sizeof v, LV_SYMBOL_LEFT " %d%% " LV_SYMBOL_RIGHT, S.sysvol);
        add(A_SYSVOL, LV_SYMBOL_VOLUME_MAX, "System volume", v, "ALSA '%s'", S.mixer_ctl);
    } else {
        add(A_NONE, LV_SYMBOL_VOLUME_MAX, "System volume", "\xe2\x80\xa6", "Looking for a mixer");
    }
    snprintf(v, sizeof v, LV_SYMBOL_LEFT " %d%% " LV_SYMBOL_RIGHT, zos_setting_int("volume", 80));
    add(A_MEDIAVOL, LV_SYMBOL_AUDIO, "Media volume", v, "Music and video start at this level");
    add(A_TEST, LV_SYMBOL_PLAY, "Play a test tone", NULL, "One second, 440 Hz");
}

static void build_time(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char t[64], d[64], tz[64] = "";
    strftime(t, sizeof t, "%H:%M:%S", &tm);
    strftime(d, sizeof d, "%A %d %B %Y", &tm);
    add(A_NONE, LV_SYMBOL_BELL, "Time", t, NULL);
    add(A_NONE, NULL, "Date", NULL, "%s", d);
    if (!read_line("/etc/timezone", tz, sizeof tz)) snprintf(tz, sizeof tz, "%s", tm.tm_zone);
    add(A_TZ, LV_SYMBOL_GPS, "Time zone", NULL, "%s (Enter to change)", tz);
    add(A_NONE, LV_SYMBOL_REFRESH, "Set automatically", "On", "From the internet (NTP) when online");
}

static void build_storage(void)
{
    char st[32] = "", v[64];
    int pct = battery(st, sizeof st);
    if (pct >= 0) {
        snprintf(v, sizeof v, "%d%%", pct);
        add(A_NONE, LV_SYMBOL_BATTERY_FULL, "Battery", v, "%s", st);
    } else {
        add(A_NONE, LV_SYMBOL_BATTERY_EMPTY, "Battery", "n/a", "No fuel gauge found");
    }
    header("STORAGE");
    disk("/", v, sizeof v);
    add(A_NONE, LV_SYMBOL_SD_CARD, "SD card", NULL, "%s", v);
    meminfo(v, sizeof v);
    add(A_NONE, LV_SYMBOL_SETTINGS, "Memory", NULL, "%s", v);
}

static void build_about(void)
{
    char ver[64] = "", date[32] = "", os[128] = "", model[96] = "", ip[200], up[48];
    FILE *f = fopen("/etc/zeroos-release", "r");
    if (f) {
        char l[128];
        while (fgets(l, sizeof l, f)) { sscanf(l, "ZEROOS_VERSION=%63s", ver); sscanf(l, "ZEROOS_BUILD_DATE=%31s", date); }
        fclose(f);
    }
    f = fopen("/etc/os-release", "r");
    if (f) {
        char l[200];
        while (fgets(l, sizeof l, f))
            if (!strncmp(l, "PRETTY_NAME=", 12)) { snprintf(os, sizeof os, "%s", l + 12); os[strcspn(os, "\"\n")] = 0; if (os[0] == '"') memmove(os, os + 1, strlen(os)); }
        fclose(f);
        os[strcspn(os, "\"")] = 0;
    }
    if (!read_line("/proc/device-tree/model", model, sizeof model)) snprintf(model, sizeof model, "unknown");
    struct utsname u;
    uname(&u);
    char host[64];
    gethostname(host, sizeof host);
    ip_addresses(ip, sizeof ip);
    double secs = 0;
    f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &secs) != 1) secs = 0; fclose(f); }
    snprintf(up, sizeof up, "%dd %dh %dm", (int)secs / 86400, (int)secs / 3600 % 24, (int)secs / 60 % 60);
    add(A_NONE, LV_SYMBOL_HOME, "ZeroOS", ver[0] ? ver : "dev", date[0] ? "Built %s" : NULL, date);
    add(A_NONE, NULL, "System", NULL, "%s", os[0] ? os : u.sysname);
    add(A_NONE, NULL, "Kernel", NULL, "%s", u.release);
    add(A_NONE, NULL, "Device", NULL, "%s", model);
    add(A_NONE, NULL, "Name", host, NULL);
    add(A_NONE, LV_SYMBOL_WIFI, "Network", NULL, "%s", ip);
    add(A_NONE, NULL, "Up for", up, NULL);
}

/* Release notes as rows: the bullets, up to a "---" line (the rest is
 * for the GitHub page), wrapped to the width of a row. */
static void notes_rows(const char *notes)
{
    const int width = 40;
    const char *p = notes;
    while (*p && S.nrows < MAX_ROWS - 1) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        p = e ? e + 1 : p + n;
        line[strcspn(line, "\r")] = 0;
        if (!strncmp(line, "---", 3)) break;
        char *t = line;
        while (*t == ' ') t++;
        if (!*t || !strncmp(t, "Full changelog", 14) || !strncmp(t, "**Full Changelog", 16)) continue;
        bool bullet = (t[0] == '*' || t[0] == '-') && t[1] == ' ';
        if (bullet) t += 2;
        while (*t == '#') t++;
        while (*t == ' ') t++;
        bool first = true;
        while (*t && S.nrows < MAX_ROWS - 1) {
            int len = (int)strlen(t), cut = len;
            if (len > width - 2) {
                cut = width - 2;
                while (cut > 10 && t[cut] != ' ') cut--;
                if (t[cut] != ' ') cut = width - 2;
            }
            char row[64];
            snprintf(row, sizeof row, "%s%.*s", first ? (bullet ? "\xe2\x80\xa2 " : "") : (bullet ? "   " : ""), cut, t);
            add(A_NONE, NULL, row, NULL, NULL);
            t += cut;
            while (*t == ' ') t++;
            first = false;
        }
    }
}

static void build_update(void)
{
    const zos_update_info_t *u = zos_update_info();
    zos_update_status_t st;
    zos_update_status(&st);
    bool busy = zos_update_busy();
    srow_t *r;
    add(A_NONE, LV_SYMBOL_HOME, "Installed", u->installed[0] ? u->installed : "development build",
        u->installed_tag[0] && strcmp(u->installed_tag, u->installed) ? "From release %s" : NULL, u->installed_tag);
    if (busy) {
        char pct[16] = "";
        if (!strcmp(st.state, "downloading")) snprintf(pct, sizeof pct, "%d%%", st.pct);
        r = add(A_NONE, LV_SYMBOL_DOWNLOAD, st.msg[0] ? st.msg : "Updating\xe2\x80\xa6", pct,
                "ZeroOS restarts by itself when it's done");
        r->sym_color = 0x2563eb;
    } else {
        if (!strcmp(st.state, "error") && time(NULL) - st.time < 15 * 60) {
            r = add(A_NONE, LV_SYMBOL_WARNING, "The update didn't work", NULL, "%s", st.msg);
            r->sym_color = 0xe5484d;
        }
        bool fresh = time(NULL) - st.time < 3 * 60;
        if (fresh && !strcmp(st.state, "done")) {
            r = add(A_NONE, LV_SYMBOL_OK, st.msg, NULL, "One moment\xe2\x80\xa6");
            r->sym_color = 0x22c55e;
        } else if (zos_update_checking()) {
            add(A_NONE, LV_SYMBOL_REFRESH, "Checking for updates\xe2\x80\xa6", NULL, NULL);
        } else if (u->update && !zos_user_is_admin()) {
            r = add(A_NONE, LV_SYMBOL_DOWNLOAD, "A new ZeroOS is available", u->latest, "Ask an admin to install it");
            r->sym_color = 0x2563eb;
        } else if (u->update) {
            char size[24] = "";
            if (u->size > 0) zos_ui_fmt_size((double)u->size, size, sizeof size);
            char sub[96] = "";
            if (u->published[0]) snprintf(sub, sizeof sub, "Released %s \xc2\xb7 ", u->published);
            if (size[0]) snprintf(sub + strlen(sub), sizeof sub - strlen(sub), "%s \xc2\xb7 ", size);
            if (strlen(sub) > 3) sub[strlen(sub) - 4] = 0;   /* the last " · " */
            r = add(A_UPD_INSTALL, LV_SYMBOL_DOWNLOAD,
                    S.confirm == A_UPD_INSTALL ? "Enter again: install and restart" : "Install ZeroOS", u->latest,
                    "%s", sub[0] ? sub : "ZeroOS restarts after");
            r->sym_color = 0x2563eb;
        } else if (u->checked && u->error[0]) {
            r = add(A_UPD_CHECK, LV_SYMBOL_WARNING, "Couldn't check for updates", NULL, "%s", u->error);
            r->sym_color = 0xf59e0b;
        } else if (u->checked) {
            struct tm tm;
            char t[16];
            localtime_r(&u->checked, &tm);
            strftime(t, sizeof t, "%H:%M", &tm);
            r = add(A_UPD_CHECK, LV_SYMBOL_OK, "ZeroOS is up to date", NULL, "Checked at %s \xc2\xb7 Enter to check again", t);
            r->sym_color = 0x22c55e;
        } else {
            add(A_UPD_CHECK, LV_SYMBOL_REFRESH, "Check for updates", NULL, "From GitHub");
        }
    }
    add(A_UPD_AUTO, NULL, "Check automatically", onoff(zos_setting_int("update_auto", 1)), "Once a day, when online");
    char prev[64];
    if (!busy && zos_user_is_admin() && zos_update_rollback_version(prev, sizeof prev)) {
        char t[64];
        snprintf(t, sizeof t, "Go back to %s", prev);
        add(A_UPD_ROLLBACK, LV_SYMBOL_LOOP, S.confirm == A_UPD_ROLLBACK ? "Press Enter again to go back" : t, NULL,
            "Undo the last update");
    }
    if (u->update && u->notes[0]) {
        char h[64];
        snprintf(h, sizeof h, "WHAT'S NEW IN %s", u->latest);
        header(h);
        notes_rows(u->notes);
    }
}

static const char *lock_name(zos_lock_t l)
{
    return l == ZOS_LOCK_PIN ? "PIN" : l == ZOS_LOCK_PASSWORD ? "Password" : "No lock";
}

static void build_users(void)
{
    const zos_user_t *me = zos_user_current();
    for (int i = 0; i < zos_users_count(); i++) {
        const zos_user_t *u = zos_users_get(i);
        bool self = me && !strcmp(u->id, me->id);
        bool can = self || zos_user_is_admin();
        char t[48];
        snprintf(t, sizeof t, "%s%s", u->name, self ? " (you)" : "");
        srow_t *r = add(can ? A_USER_OPEN : A_NONE, LV_SYMBOL_STOP, t, u->admin ? "admin" : NULL,
                        "%s \xc2\xb7 Users/%s", lock_name(u->lock), u->name);
        r->sym_color = u->color;
        r->arg = i;
    }
    if (zos_user_is_admin()) add(A_USER_ADD, LV_SYMBOL_PLUS, "Add a user", NULL, "With their own books, music, videos and photos");
    add(A_LOGOUT, LV_SYMBOL_LOOP, "Log out", NULL, "Back to the login screen");
}

static void build_user(void)
{
    const zos_user_t *u = zos_users_find(S.user_id);
    if (!u) { add(A_NONE, LV_SYMBOL_WARNING, "This user is gone", NULL, NULL); return; }
    const zos_user_t *me = zos_user_current();
    bool self = me && !strcmp(u->id, me->id);
    add(A_USER_NAME, LV_SYMBOL_EDIT, "Name", u->name, "Enter to change (renames their folder too)");
    add(A_USER_LOCK, LV_SYMBOL_EYE_CLOSE, "Lock", lock_name(u->lock),
        u->lock == ZOS_LOCK_NONE ? "Enter to set a PIN or password" : "Enter to change or remove it");
    if (zos_user_is_admin())
        add(A_USER_ADMIN, LV_SYMBOL_SETTINGS, "Admin", onoff(u->admin), "Admins manage users and install updates");
    char m[600];
    zos_users_media_dir(u, m, sizeof m);
    add(A_NONE, LV_SYMBOL_DIRECTORY, "Their files", NULL, "%s", m);
    if (zos_user_is_admin() && !self)
        add(A_USER_REMOVE, LV_SYMBOL_TRASH, S.confirm == A_USER_REMOVE ? "Press Enter again to remove" : "Remove this user",
            NULL, "Their files stay in Users/%s", u->name);
}

static void build_power(void)
{
    if (zos_user_current()) add(A_LOGOUT, LV_SYMBOL_LOOP, "Log out", NULL, "Back to the login screen");
    add(A_RESTART_UI, LV_SYMBOL_REFRESH, S.confirm == A_RESTART_UI ? "Press Enter again to restart ZeroOS" : "Restart ZeroOS",
        NULL, "Just the interface; music stops");
    add(A_REBOOT, LV_SYMBOL_LOOP, S.confirm == A_REBOOT ? "Press Enter again to reboot" : "Reboot", NULL, NULL);
    add(A_SHUTDOWN, LV_SYMBOL_POWER, S.confirm == A_SHUTDOWN ? "Press Enter again to shut down" : "Shut down", NULL,
        "Then switch off or unplug");
}

static void render(void)
{
    if (!S.scr) return;
    static const char *titles[] = {"Settings", "Wi-Fi", "Display & HDMI", "Sound", "Date & time",
                                   "Battery & storage", "About", "Power", "Software update", "Users", "User"};
    static const char *hints[] = {
        LV_SYMBOL_NEW_LINE " open   Esc home",
        LV_SYMBOL_NEW_LINE " connect   R rescan   F forget   Esc back",
        LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " adjust   " LV_SYMBOL_NEW_LINE " toggle   Esc back",
        LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " adjust   " LV_SYMBOL_NEW_LINE " play   Esc back",
        LV_SYMBOL_NEW_LINE " change   Esc back",
        "Esc back", "Esc back",
        LV_SYMBOL_NEW_LINE " choose   Esc back",
        LV_SYMBOL_NEW_LINE " choose   R check again   Esc back",
        LV_SYMBOL_NEW_LINE " open   Esc back",
        LV_SYMBOL_NEW_LINE " change   Esc back",
    };
    S.nrows = 0;
    switch (S.page) {
    case P_HOME: build_home(); break;
    case P_WIFI: build_wifi(); break;
    case P_DISPLAY: build_display(); break;
    case P_SOUND: build_sound(); break;
    case P_TIME: build_time(); break;
    case P_STORAGE: build_storage(); break;
    case P_ABOUT: build_about(); break;
    case P_POWER: build_power(); break;
    case P_UPDATE: build_update(); break;
    case P_USERS: build_users(); break;
    case P_USER: build_user(); break;
    }
    const zos_user_t *pu = S.page == P_USER ? zos_users_find(S.user_id) : NULL;
    lv_label_set_text(S.title, pu ? pu->name : titles[S.page]);
    lv_label_set_text(S.hint, hints[S.page]);
    int sel = S.want_sel >= 0 ? S.want_sel : zos_vlist_selected(S.list);
    S.want_sel = -1;
    zos_vlist_set(S.list, S.nrows, fill, NULL);
    zos_vlist_select(S.list, sel < 0 ? 0 : sel);
}

static void open_page(page_t p)
{
    if (S.page == P_HOME) S.home_sel = zos_vlist_selected(S.list);
    S.page = p;
    S.confirm = 0;
    S.want_sel = p == P_HOME ? S.home_sel : 0;
    if (p == P_WIFI) {
        S.wifi_msg[0] = 0;
        run_job(0, NULL, NULL, 0);
    }
    if (p == P_SOUND) run_job(5, NULL, NULL, 0);
    if (p == P_UPDATE) {
        const zos_update_info_t *u = zos_update_info();
        if (!zos_update_busy() && (!u->checked || u->error[0] || time(NULL) - u->checked > 10 * 60)) zos_update_check();
    }
    lv_timer_set_period(S.tick, p == P_UPDATE ? 700 : 2000);
    render();
}

static void adjust(int dir)
{
    int i = zos_vlist_selected(S.list);
    if (i < 0) return;
    switch (S.rows[i].action) {
    case A_BRIGHT: {
        int b = brightness_get();
        if (b >= 0) brightness_set(b + dir * 10);
        break;
    }
    case A_SYSVOL:
        if (S.sysvol >= 0) {
            int v = S.sysvol + dir * 5;
            run_job(6, NULL, NULL, v < 0 ? 0 : v > 100 ? 100 : v);
        }
        break;
    case A_MEDIAVOL: {
        int v = zos_setting_int("volume", 80) + dir * 5;
        zos_setting_set_int("volume", v < 0 ? 0 : v > 130 ? 130 : v);
        break;
    }
    default: return;
    }
    render();
}

static void activate(void)
{
    int i = zos_vlist_selected(S.list);
    if (i < 0) return;
    srow_t *r = &S.rows[i];
    switch (r->action) {
    case A_WIFI: open_page(P_WIFI); return;
    case A_DISPLAY: open_page(P_DISPLAY); return;
    case A_SOUND: open_page(P_SOUND); return;
    case A_TIME: open_page(P_TIME); return;
    case A_STORAGE: open_page(P_STORAGE); return;
    case A_ABOUT: open_page(P_ABOUT); return;
    case A_POWER: open_page(P_POWER); return;
    case A_UPDATE: open_page(P_UPDATE); return;
    case A_USERS: open_page(P_USERS); return;
    case A_USER_OPEN: {
        const zos_user_t *u = zos_users_get(r->arg);
        if (!u) return;
        snprintf(S.user_id, sizeof S.user_id, "%s", u->id);
        S.users_sel = i;
        open_page(P_USER);
        return;
    }
    case A_USER_ADD: open_user_form(4); return;
    case A_USER_NAME: open_user_form(5); return;
    case A_USER_LOCK: open_user_form(6); return;
    case A_USER_ADMIN: {
        const zos_user_t *u = zos_users_find(S.user_id);
        char err[96];
        if (u && !zos_users_set_admin(u->id, !u->admin, err, sizeof err)) zos_notify(0xe5484d, "Users", err);
        break;
    }
    case A_USER_REMOVE: {
        if (S.confirm != r->action) { S.confirm = r->action; break; }
        S.confirm = 0;
        char err[96];
        if (!zos_users_remove(S.user_id, err, sizeof err)) { zos_notify(0xe5484d, "Users", err); break; }
        zos_notify(0x52525b, "Users", "Removed. Their files are still on the drive.");
        open_page(P_USERS);
        return;
    }
    case A_LOGOUT: zos_logout(); return;
    case A_UPD_CHECK: zos_update_check(); break;
    case A_UPD_AUTO: zos_setting_set_int("update_auto", !zos_setting_int("update_auto", 1)); break;
    case A_UPD_INSTALL: case A_UPD_ROLLBACK: {
        if (S.confirm != r->action) { S.confirm = r->action; break; }
        S.confirm = 0;
        char err[160];
        bool ok = r->action == A_UPD_INSTALL ? zos_update_install(err, sizeof err) : zos_update_rollback(err, sizeof err);
        if (!ok) zos_notify(0xe5484d, "Software update", err);
        break;
    }
    case A_WIFI_RADIO: run_job(2, NULL, NULL, !S.wifi_on); break;
    case A_WIFI_NET: {
        net_t *n = &S.nets[r->arg];
        snprintf(S.pending_ssid, sizeof S.pending_ssid, "%s", n->ssid);
        if (n->secured && !n->known) {
            char t[100];
            snprintf(t, sizeof t, "Wi-Fi \xc2\xb7 %s", n->ssid);
            open_form(1, t, "Password", "", "network password", true);
            return;
        }
        snprintf(S.wifi_msg, sizeof S.wifi_msg, "Connecting to %s\xe2\x80\xa6", n->ssid);
        run_job(1, n->ssid, NULL, n->known);
        break;
    }
    case A_MIRROR: zos_hdmi_set_mirror(!zos_hdmi_mirror_enabled()); break;
    case A_VIDEO_HDMI: zos_setting_set_int("video_hdmi", !zos_setting_int("video_hdmi", 1)); break;
    case A_HDMI_AUDIO: zos_setting_set_int("hdmi_audio", !zos_setting_int("hdmi_audio", 1)); break;
    case A_SUBLANG: open_form(3, "Subtitle language", "Languages", zos_setting_str("sub_lang", ""), "en  or  nl,en", false); return;
    case A_TEST: {
        zos_player_opts_t o = {0};
        zos_player_cbs_t cbs = {0};
        zos_player_play("av://lavfi:sine=frequency=440:duration=1", &o, cbs, NULL);
        break;
    }
    case A_TZ: {
        char tz[64] = "";
        read_line("/etc/timezone", tz, sizeof tz);
        open_form(2, "Time zone", "Zone", tz, "Europe/Brussels", false);
        return;
    }
    case A_RESTART_UI: case A_REBOOT: case A_SHUTDOWN:
        if (S.confirm != r->action) { S.confirm = r->action; break; }
        if (r->action == A_RESTART_UI) {
            zos_users_keep_login(60);
            zos_player_stop();
            exit(0);   /* systemd starts ZeroOS again */
        } else {
            const char *argv[] = {"systemctl", r->action == A_REBOOT ? "reboot" : "poweroff", NULL};
            zos_notify(0x52525b, "Power", r->action == A_REBOOT ? "Rebooting\xe2\x80\xa6" : "Shutting down\xe2\x80\xa6");
            zos_run_bg(argv);
        }
        break;
    default: return;
    }
    render();
}

static void sink_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (zos_vlist_key(S.list, k)) { S.confirm = 0; return; }
    switch (k) {
    case LV_KEY_ENTER: case ' ': activate(); break;
    case LV_KEY_RIGHT: if (S.page == P_HOME) activate(); else adjust(1); break;
    case LV_KEY_LEFT:
        if (S.page == P_DISPLAY || S.page == P_SOUND) adjust(-1);
        else if (S.page == P_USER) { open_page(P_USERS); S.want_sel = S.users_sel; render(); }
        else if (S.page != P_HOME) open_page(P_HOME);
        break;
    case 'r': case 'R':
        if (S.page == P_WIFI) run_job(0, NULL, NULL, 0), render();
        if (S.page == P_UPDATE && !zos_update_busy()) zos_update_check(), render();
        break;
    case 'f': case 'F':
        if (S.page == P_WIFI) {
            int i = zos_vlist_selected(S.list);
            if (i >= 0 && (S.rows[i].action == A_WIFI_CURRENT || S.rows[i].action == A_WIFI_NET) && S.nets[S.rows[i].arg].known) {
                snprintf(S.wifi_msg, sizeof S.wifi_msg, "Forgot %s", S.nets[S.rows[i].arg].ssid);
                run_job(4, S.nets[S.rows[i].arg].ssid, NULL, 0);
                render();
            }
        }
        break;
    case LV_KEY_ESC: case LV_KEY_BACKSPACE: case '`':
        if (S.page == P_HOME) zos_app_exit();
        else if (S.page == P_USER) { open_page(P_USERS); S.want_sel = S.users_sel; render(); }
        else open_page(P_HOME);
        break;
    default: break;
    }
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!S.form && (S.page == P_TIME || S.page == P_HOME || S.page == P_DISPLAY || S.page == P_STORAGE ||
                    S.page == P_UPDATE)) render();
}

static void settings_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&S, 0, sizeof S);
    S.scr = scr;
    S.g = g;
    S.sysvol = -1;
    S.want_sel = -1;
    lv_obj_t *sb = zos_statusbar_create(scr, "Settings");
    S.title = lv_obj_get_child(sb, 0);
    S.list = zos_vlist_create(scr, 2, ZOS_STATUSBAR_H, ZOS_HOR_RES - 4, ZOS_VER_RES - ZOS_STATUSBAR_H - 12, 28, 22);
    S.hint = zos_ui_hint(scr, "");
    S.sink = zos_ui_box(scr);
    lv_obj_set_size(S.sink, 1, 1);
    lv_obj_add_event_cb(S.sink, sink_key, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g, S.sink);
    lv_group_focus_obj(S.sink);
    S.page = P_HOME;
    run_job(0, NULL, NULL, 0);   /* Wi-Fi name for the home row */
    render();
    S.tick = lv_timer_create(tick_cb, 2000, NULL);
}

static void settings_close(void)
{
    zos_task_cancel(&owner_tag);
    lv_timer_delete(S.tick);
    memset(&S, 0, sizeof S);
}

/* Applied once at startup (main.c). */
void zos_settings_apply_saved(void)
{
    int b = zos_setting_int("brightness", -1);
    if (b > 0) brightness_set(b);
}

const zos_app_t zos_app_settings = {
    .id = "settings", .name = "Settings", .tagline = "Wi-Fi, display, HDMI, sound",
    .icon = &zos_icon_settings, .color = 0x52525b, .open = settings_open, .close = settings_close,
};
