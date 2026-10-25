/* Tailscale: connection status, sign-in by QR code, devices, exit node.
 *
 * Talks to tailscaled's LocalAPI (src/core/tailscale.c) from worker threads.
 * Enter on a device opens SSH to it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../../core/tailscale.h"
#include "../../core/task.h"
#include "../../zos.h"
#include "../ssh/ssh.h"

extern const lv_image_dsc_t zos_icon_vpn;
extern const zos_app_t zos_app_tailscale;

#define POLL_MS 3000

static struct {
    lv_obj_t *scr, *sink;
    lv_obj_t *card, *dot, *state_lbl, *self_lbl, *net_lbl, *msg_lbl;
    lv_obj_t *login, *qr, *login_url, *login_title;
    lv_obj_t *list, *hint;
    ts_status_t st;
    bool have, busy;
    int sel;
    lv_timer_t *poll;
    char msg[160];
    uint32_t msg_until;
} T;

static const char owner_tag;   /* task owner for this app's jobs */

/* ---------------- jobs ---------------- */

typedef struct {
    int op;              /* 0 status, 1 connect, 2 disconnect, 3 login, 4 exit node */
    char arg[48];
    bool ok;
    char err[160];
    ts_status_t st;
} job_t;

static void job_work(void *a)
{
    job_t *j = a;
    switch (j->op) {
    case 1: j->ok = ts_set_running(true, j->err, sizeof j->err); break;
    case 2: j->ok = ts_set_running(false, j->err, sizeof j->err); break;
    case 3: j->ok = ts_login(j->err, sizeof j->err); break;
    case 4: j->ok = ts_set_exit_node(j->arg, j->err, sizeof j->err); break;
    }
    if (j->op == 0 || j->ok) ts_fetch_status(&j->st);   /* always come back with fresh state */
}

static void render(void);

static void set_msg(const char *m)
{
    snprintf(T.msg, sizeof T.msg, "%s", m);
    T.msg_until = lv_tick_get() + 4000;
}

static void job_done(void *a)
{
    job_t *j = a;
    if (j->op && !j->ok) set_msg(j->err[0] ? j->err : "Request failed");
    else if (j->op == 4) set_msg(j->arg[0] ? "Exit node set" : "Exit node cleared");
    ts_status_free(&T.st);
    T.st = j->st;
    j->st.peers = NULL;
    T.have = true;
    ts_poke();   /* let the status bar catch up */
    render();
}

static void job_release(void *a)
{
    job_t *j = a;
    ts_status_free(&j->st);
    free(j);
    T.busy = false;
}

static void run(int op, const char *arg)
{
    if (T.busy && op == 0) return;
    T.busy = true;
    job_t *j = calloc(1, sizeof *j);
    j->op = op;
    if (arg) snprintf(j->arg, sizeof j->arg, "%s", arg);
    zos_task_run(&owner_tag, job_work, job_done, job_release, j);
}

static void poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    run(0, NULL);
}

/* ---------------- rendering ---------------- */

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

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static const char *os_tag(const char *os)
{
    if (!strcasecmp(os, "android")) return "android";
    if (!strcasecmp(os, "iOS")) return "iOS";
    if (!strcasecmp(os, "macOS")) return "mac";
    if (!strcasecmp(os, "windows")) return "win";
    return os;
}

static void select_row(int i)
{
    int n = T.st.npeers;
    if (!n) return;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    lv_obj_t *old = lv_obj_get_child(T.list, T.sel);
    if (old) lv_obj_remove_state(old, LV_STATE_CHECKED);
    T.sel = i;
    lv_obj_t *row = lv_obj_get_child(T.list, i);
    if (row) {
        lv_obj_add_state(row, LV_STATE_CHECKED);
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }
}

static void render_peers(void)
{
    lv_obj_clean(T.list);
    for (int i = 0; i < T.st.npeers; i++) {
        ts_peer_t *p = &T.st.peers[i];
        lv_obj_t *row = box(T.list);
        lv_obj_set_size(row, lv_pct(100), 23);
        lv_obj_set_style_pad_hor(row, 7, 0);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(ZOS_COLOR_SURFACE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);

        lv_obj_t *d = box(row);
        lv_obj_set_size(d, 6, 6);
        lv_obj_set_style_radius(d, 3, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(p->online ? 0x22c55e : 0x4b5563), 0);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *h = label(row, zos_font.body, p->online ? ZOS_COLOR_TEXT : ZOS_COLOR_MUTED);
        lv_label_set_text(h, p->host);
        zos_label_one_line(h, 128);
        lv_obj_align(h, LV_ALIGN_LEFT_MID, 12, 0);

        char tags[48];
        snprintf(tags, sizeof tags, "%s%s", os_tag(p->os), p->exit_node ? "  \xe2\x96\xb6 exit" : p->exit_option ? "  can exit" : "");
        lv_obj_t *t = label(row, zos_font.small, p->exit_node ? 0x60a5fa : 0x5b6472);
        lv_label_set_text(t, tags);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 144, 0);

        lv_obj_t *ip = label(row, zos_font.small, ZOS_COLOR_MUTED);
        lv_label_set_text(ip, p->ip);
        lv_obj_align(ip, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (!T.st.npeers) {
        lv_obj_t *e = label(T.list, zos_font.small, ZOS_COLOR_MUTED);
        lv_label_set_text(e, "No other devices in this tailnet yet.");
        lv_obj_set_style_pad_all(e, 6, 0);
    }
    int keep = T.sel;
    T.sel = 0;
    select_row(keep);
}

static void render(void)
{
    const ts_status_t *s = &T.st;
    bool needs_login = s->reachable && !strcmp(s->state, "NeedsLogin");
    show(T.login, needs_login);
    show(T.card, !needs_login);
    show(T.list, s->reachable && !needs_login);

    uint32_t dot = 0x6b7280;
    const char *title = "Checking\xe2\x80\xa6";
    char self[160] = "", net[120] = "";
    if (!T.have) {
    } else if (!s->reachable) {
        dot = 0xef4444;
        title = "tailscaled not reachable";
        snprintf(self, sizeof self, "%s", s->err);
    } else if (!strcmp(s->state, "Running")) {
        dot = 0x22c55e;
        title = "Connected";
        snprintf(self, sizeof self, "%s \xc2\xb7 %s", s->self_host, s->self_ip);
        const char *exit = NULL;
        for (int i = 0; i < s->npeers; i++)
            if (s->peers[i].exit_node) exit = s->peers[i].host;
        snprintf(net, sizeof net, "%s%s%s", s->tailnet, exit ? " \xc2\xb7 exit via " : "", exit ? exit : "");
    } else if (!strcmp(s->state, "Stopped")) {
        dot = 0x6b7280;
        title = "Disconnected";
        snprintf(self, sizeof self, "%s \xc2\xb7 press C to connect", s->self_host);
        snprintf(net, sizeof net, "%s", s->tailnet);
    } else if (!strcmp(s->state, "NeedsMachineAuth")) {
        dot = 0xf59e0b;
        title = "Waiting for admin approval";
        snprintf(self, sizeof self, "Approve this device in the Tailscale admin console");
    } else {
        dot = 0xf59e0b;
        title = s->state[0] ? s->state : "Starting";
    }
    if (s->nhealth && strcmp(s->state, "Stopped")) snprintf(net, sizeof net, "\xe2\x9a\xa0 %.100s", s->health[0]);

    lv_obj_set_style_bg_color(T.dot, lv_color_hex(dot), 0);
    lv_label_set_text(T.state_lbl, title);
    lv_label_set_text(T.self_lbl, self);
    lv_label_set_text(T.net_lbl, net);
    bool show_msg = T.msg[0] && (int32_t)(lv_tick_get() - T.msg_until) < 0;
    lv_label_set_text(T.msg_lbl, show_msg ? T.msg : "");
    show(T.msg_lbl, show_msg);
    show(T.net_lbl, !show_msg);

    if (needs_login) {
        if (s->auth_url[0]) {
            lv_qrcode_update(T.qr, s->auth_url, (uint32_t)strlen(s->auth_url));
            show(T.qr, true);
            lv_label_set_text(T.login_title, "Sign in to Tailscale");
            char u[300];
            snprintf(u, sizeof u, "Scan with your phone, or open:\n%s\n\nThis screen updates once you approve.", s->auth_url);
            lv_label_set_text(T.login_url, u);
        } else {
            show(T.qr, false);
            lv_label_set_text(T.login_title, "Not signed in");
            lv_label_set_text(T.login_url, "Press Enter to get a sign-in link.\nYou'll scan it with your phone.");
        }
    }
    if (s->reachable && !needs_login) render_peers();

    if (needs_login) lv_label_set_text(T.hint, LV_SYMBOL_NEW_LINE " sign-in link    Esc back");
    else lv_label_set_text(T.hint, LV_SYMBOL_NEW_LINE " SSH    E exit node    C connect/disconnect    Esc");
}

/* ---------------- keys ---------------- */

static void on_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    const ts_status_t *s = &T.st;
    bool needs_login = s->reachable && !strcmp(s->state, "NeedsLogin");
    if (k == LV_KEY_ESC || k == 'q' || k == '`' || k == LV_KEY_BACKSPACE) {
        zos_app_exit();
    } else if (needs_login) {
        if (k == LV_KEY_ENTER || k == 'l' || k == ' ') {
            run(3, NULL);
            set_msg("Asking for a sign-in link\xe2\x80\xa6");
        }
    } else if (k == LV_KEY_DOWN) {
        select_row(T.sel + 1);
    } else if (k == LV_KEY_UP) {
        select_row(T.sel - 1);
    } else if (k == 'c') {
        if (!strcmp(s->state, "Running")) run(2, NULL), set_msg("Disconnecting\xe2\x80\xa6");
        else run(1, NULL), set_msg("Connecting\xe2\x80\xa6");
        render();
    } else if (k == 'e' && s->npeers) {
        ts_peer_t *p = &s->peers[T.sel];
        if (p->exit_node) run(4, "");
        else if (p->exit_option && p->online) run(4, p->id);
        else set_msg(p->exit_option ? "That exit node is offline" : "Not offered as an exit node");
        render();
    } else if ((k == LV_KEY_ENTER || k == ' ') && s->npeers) {
        ts_peer_t *p = &s->peers[T.sel];
        if (!p->online) {
            set_msg("That device is offline");
            render();
        } else {
            ssh_open_host(p->host, p->ip);
        }
    }
}

/* ---------------- lifecycle ---------------- */

static void ts_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&T, 0, sizeof T);
    T.scr = scr;
    zos_statusbar_create(scr, "Tailscale");

    T.card = box(scr);
    lv_obj_set_size(T.card, ZOS_HOR_RES - 8, 42);
    lv_obj_set_pos(T.card, 4, ZOS_STATUSBAR_H + 1);
    lv_obj_set_style_bg_opa(T.card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(T.card, lv_color_hex(ZOS_COLOR_SURFACE), 0);
    lv_obj_set_style_radius(T.card, 8, 0);
    T.dot = box(T.card);
    lv_obj_set_size(T.dot, 9, 9);
    lv_obj_set_style_radius(T.dot, 5, 0);
    lv_obj_set_style_bg_opa(T.dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(T.dot, 9, 6);
    T.state_lbl = label(T.card, zos_font.title, ZOS_COLOR_TEXT);
    lv_obj_set_pos(T.state_lbl, 24, 1);
    T.self_lbl = label(T.card, zos_font.small, ZOS_COLOR_MUTED);
    zos_label_one_line(T.self_lbl, 296);
    lv_obj_set_pos(T.self_lbl, 9, 26);
    T.net_lbl = label(T.card, zos_font.small, 0x60a5fa);
    lv_obj_align(T.net_lbl, LV_ALIGN_TOP_RIGHT, -8, 4);
    T.msg_lbl = label(T.card, zos_font.small, ZOS_COLOR_ACCENT);
    lv_obj_align(T.msg_lbl, LV_ALIGN_TOP_RIGHT, -8, 4);

    T.list = lv_obj_create(scr);
    lv_obj_remove_style_all(T.list);
    lv_obj_set_pos(T.list, 0, ZOS_STATUSBAR_H + 46);
    lv_obj_set_size(T.list, ZOS_HOR_RES, ZOS_VER_RES - ZOS_STATUSBAR_H - 46 - 13);
    lv_obj_set_style_pad_hor(T.list, 4, 0);
    lv_obj_set_flex_flow(T.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(T.list, LV_SCROLLBAR_MODE_OFF);

    T.login = box(scr);
    lv_obj_set_size(T.login, ZOS_HOR_RES, ZOS_VER_RES - ZOS_STATUSBAR_H - 13);
    lv_obj_set_pos(T.login, 0, ZOS_STATUSBAR_H);
    T.qr = lv_qrcode_create(T.login);
    lv_qrcode_set_size(T.qr, 124);
    lv_qrcode_set_dark_color(T.qr, lv_color_black());
    lv_qrcode_set_light_color(T.qr, lv_color_white());
    lv_obj_set_style_border_width(T.qr, 4, 0);
    lv_obj_set_style_border_color(T.qr, lv_color_white(), 0);
    lv_obj_set_pos(T.qr, 8, 3);
    T.login_title = label(T.login, zos_font.title, ZOS_COLOR_TEXT);
    lv_obj_set_pos(T.login_title, 144, 6);
    T.login_url = label(T.login, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_set_width(T.login_url, 168);
    lv_label_set_long_mode(T.login_url, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(T.login_url, 144, 30);

    T.hint = label(scr, &lv_font_montserrat_10, 0x5b6472);
    lv_obj_align(T.hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    T.sink = box(scr);
    lv_obj_set_size(T.sink, 1, 1);
    lv_obj_add_event_cb(T.sink, on_key, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g, T.sink);

    render();
    run(0, NULL);
    T.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}

static void ts_close(void)
{
    zos_task_cancel(&owner_tag);
    lv_timer_delete(T.poll);
    ts_status_free(&T.st);
    memset(&T, 0, sizeof T);
}

const zos_app_t zos_app_tailscale = {
    .id = "tailscale", .name = "Tailscale", .tagline = "VPN: your devices, anywhere",
    .icon = &zos_icon_vpn, .color = 0x4f46e5, .open = ts_open, .close = ts_close,
};
