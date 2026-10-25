/* SSH: a terminal for the system OpenSSH client.
 *
 * `ssh` runs in a pseudo-terminal, so keys, agents, ~/.ssh/config,
 * known_hosts and password prompts all behave exactly as on a desktop.
 * libvterm renders it (term.c). Ctrl+] opens the session menu: scrollback,
 * text size, disconnect.
 *
 * Views: hosts (saved + tailnet devices) -> form -> terminal; "My SSH key"
 * shows (or generates) ~/.ssh/id_ed25519.pub with a QR code. */
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../core/form.h"
#include "../../core/tailscale.h"
#include "../../core/task.h"
#include "ssh.h"
#include "term.h"

extern char **environ;
extern const lv_image_dsc_t zos_icon_terminal;

#define MAX_HOSTS 32
static const term_font_t *const FONTS[] = {&term_font_6x12, &term_font_6x10, &term_font_5x8};
#define NFONTS 3

typedef struct {
    char name[48], user[48], host[128], port[8];
} host_t;

typedef enum { V_HOSTS, V_FORM, V_TERM, V_KEY } view_t;
typedef enum { IT_NEW, IT_SAVED, IT_TAILNET, IT_KEY, IT_HEADER } item_kind_t;

typedef struct {
    item_kind_t kind;
    int idx;
    char label[64], sub[96];
    char ts_host[64], ts_ip[48];
} item_t;

static struct {
    lv_obj_t *scr, *sink;
    lv_group_t *g;
    view_t view;

    host_t hosts[MAX_HOSTS];
    int nhosts;
    int font_idx;
    char default_user[48];

    lv_obj_t *hosts_view, *list, *hint;
    item_t items[MAX_HOSTS + 64];
    int nitems, sel;
    bool confirm_delete;

    /* form */
    zos_form_t *form;
    host_t edit;
    int edit_idx;   /* -1 = new */
    zos_field_t fields[4];

    /* session */
    term_t *term;
    lv_obj_t *term_view, *menu, *menu_lbl, *scroll_lbl;
    bool menu_open;
    pid_t pid;
    int fd;
    bool alive;
    lv_timer_t *io;
    host_t cur;

    /* key view */
    lv_obj_t *key_view, *qr, *key_title, *key_text;
    bool keygen_busy;
} S;

static char pending_label[64], pending_host[128];
static const char owner_tag;

static void set_view(view_t v);
static void hosts_render(void);

/* ---------------- storage ---------------- */

static void hosts_path(char *out, size_t n) { snprintf(out, n, "%s/ssh_hosts.tsv", zos_cfg.state_dir); }

static void load_hosts(void)
{
    char p[512];
    hosts_path(p, sizeof p);
    S.nhosts = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    while (S.nhosts < MAX_HOSTS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "#font=", 6)) {
            S.font_idx = atoi(line + 6) % NFONTS;
            continue;
        }
        if (!strncmp(line, "#user=", 6)) {
            snprintf(S.default_user, sizeof S.default_user, "%s", line + 6);
            continue;
        }
        host_t *h = &S.hosts[S.nhosts];
        char *fld[4] = {0};
        int n = 0;
        for (char *s = line, *tab; n < 4; s = tab + 1) {
            fld[n++] = s;
            if (!(tab = strchr(s, '\t'))) break;
            *tab = 0;
        }
        if (n < 4) continue;
        snprintf(h->name, sizeof h->name, "%s", fld[0]);
        snprintf(h->user, sizeof h->user, "%s", fld[1]);
        snprintf(h->host, sizeof h->host, "%s", fld[2]);
        snprintf(h->port, sizeof h->port, "%s", fld[3]);
        S.nhosts++;
    }
    fclose(f);
}

static void save_hosts(void)
{
    char p[512], tmp[520];
    hosts_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "#font=%d\n#user=%s\n", S.font_idx, S.default_user);
    for (int i = 0; i < S.nhosts; i++)
        fprintf(f, "%s\t%s\t%s\t%s\n", S.hosts[i].name, S.hosts[i].user, S.hosts[i].host, S.hosts[i].port);
    fclose(f);
    rename(tmp, p);
}

static void clean_field(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n') *s = ' ';
}

/* ---------------- session ---------------- */

static void term_out(const char *b, size_t n, void *ud)
{
    LV_UNUSED(ud);
    if (S.alive && S.fd >= 0) {
        ssize_t w = write(S.fd, b, n);
        LV_UNUSED(w);
    }
}

static void winsize(void)
{
    if (S.fd < 0) return;
    int rows, cols;
    term_size(S.term, &rows, &cols);
    const term_font_t *f = FONTS[S.font_idx];
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, (unsigned short)(cols * f->w),
                         (unsigned short)(rows * f->h)};
    ioctl(S.fd, TIOCSWINSZ, &ws);
}

static void session_ended(int status)
{
    S.alive = false;
    if (S.fd >= 0) close(S.fd);
    S.fd = -1;
    char msg[160];
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        snprintf(msg, sizeof msg, "Could not run ssh. Is openssh-client installed?");
    else if (WIFEXITED(status))
        snprintf(msg, sizeof msg, "Connection closed (exit %d).  Enter: reconnect   Esc: hosts", WEXITSTATUS(status));
    else
        snprintf(msg, sizeof msg, "Session ended.  Enter: reconnect   Esc: hosts");
    term_local(S.term, msg);
}

static void io_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!S.alive) return;
    char buf[4096];
    for (int i = 0; i < 16; i++) {
        ssize_t n = read(S.fd, buf, sizeof buf);
        if (n > 0) {
            term_feed(S.term, buf, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        /* EIO / EOF: the child is gone */
        int status = 0;
        waitpid(S.pid, &status, 0);
        session_ended(status);
        return;
    }
    int status;
    if (waitpid(S.pid, &status, WNOHANG) == S.pid) {   /* exited without closing the pty yet */
        while (read(S.fd, buf, sizeof buf) > 0) {}
        session_ended(status);
    }
}

static void connect_host(const host_t *h)
{
    S.cur = *h;
    if (!S.term) {
        S.term = term_create(S.term_view, FONTS[S.font_idx], term_out, NULL);
        lv_obj_move_background(term_obj(S.term));
    }
    char target[200];
    if (h->user[0]) snprintf(target, sizeof target, "%s@%s", h->user, h->host);
    else snprintf(target, sizeof target, "%s", h->host);
    char banner[240];
    snprintf(banner, sizeof banner, "Connecting to %s%s%s\xe2\x80\xa6  (Ctrl+] for menu)", target,
             strcmp(h->port, "22") && h->port[0] ? " port " : "", strcmp(h->port, "22") && h->port[0] ? h->port : "");
    term_local(S.term, banner);

    int rows, cols;
    term_size(S.term, &rows, &cols);
    const term_font_t *f = FONTS[S.font_idx];
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, (unsigned short)(cols * f->w),
                         (unsigned short)(rows * f->h)};
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) {
        term_local(S.term, "Could not create a terminal (forkpty failed).");
        return;
    }
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        /* Debian's ssh_config forwards LANG (SendEnv), so the remote side
         * talks UTF-8 to us: box drawing, accents, Braille graphs. */
        if (!getenv("LANG") || !*getenv("LANG")) setenv("LANG", "C.UTF-8", 1);
        const char *argv[24];
        int a = 0;
        argv[a++] = "ssh";
        if (h->port[0] && strcmp(h->port, "22")) {
            argv[a++] = "-p";
            argv[a++] = h->port;
        }
        argv[a++] = "-o";
        argv[a++] = "ServerAliveInterval=30";
        argv[a++] = "-o";
        argv[a++] = "ConnectTimeout=15";
        /* extra options, e.g. for tests: ZOS_SSH_ARGS="-i key -o UserKnownHostsFile=..." */
        static char extra[512];
        const char *ex = getenv("ZOS_SSH_ARGS");
        if (ex) {
            snprintf(extra, sizeof extra, "%s", ex);
            for (char *tok = strtok(extra, " "); tok && a < 20; tok = strtok(NULL, " ")) argv[a++] = tok;
        }
        argv[a++] = target;
        argv[a] = NULL;
        execvp("ssh", (char *const *)argv);
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    S.pid = pid;
    S.fd = fd;
    S.alive = true;
    set_view(V_TERM);
}

static void disconnect(void)
{
    if (S.alive && S.pid > 0) kill(S.pid, SIGHUP);
}

/* ---------------- terminal view ---------------- */

static void menu_update(void)
{
    int off = term_scrollback_offset(S.term);
    char s[200];
    snprintf(s, sizeof s, LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll    F size (%s)    D disconnect    Esc back",
             FONTS[S.font_idx]->name);
    lv_label_set_text(S.menu_lbl, s);
    if (off) {
        lv_label_set_text_fmt(S.scroll_lbl, "\xe2\x86\x91 %d", off);
        lv_obj_remove_flag(S.scroll_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(S.scroll_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void open_menu(bool open)
{
    S.menu_open = open;
    if (open) {
        lv_obj_remove_flag(S.menu, LV_OBJ_FLAG_HIDDEN);
        menu_update();
    } else {
        lv_obj_add_flag(S.menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(S.scroll_lbl, LV_OBJ_FLAG_HIDDEN);
        term_scroll(S.term, -100000);
    }
}

static void term_view_key(uint32_t k)
{
    if (S.menu_open) {
        int rows, cols;
        term_size(S.term, &rows, &cols);
        if (k == LV_KEY_ESC || k == ZOS_KEY_CTRL(']') || k == LV_KEY_ENTER) open_menu(false);
        else if (k == LV_KEY_UP) term_scroll(S.term, 1);
        else if (k == LV_KEY_DOWN) term_scroll(S.term, -1);
        else if (k == ZOS_KEY_PGUP || k == LV_KEY_LEFT) term_scroll(S.term, rows - 1);
        else if (k == ZOS_KEY_PGDN || k == LV_KEY_RIGHT) term_scroll(S.term, -(rows - 1));
        else if (k == 'f') {
            S.font_idx = (S.font_idx + 1) % NFONTS;
            term_set_font(S.term, FONTS[S.font_idx]);
            winsize();
            save_hosts();
        } else if (k == 'd' || k == 'q') {
            open_menu(false);
            if (S.alive) disconnect();
            else set_view(V_HOSTS);
        }
        if (S.menu_open) menu_update();
        return;
    }
    if (k == ZOS_KEY_CTRL(']')) {
        open_menu(true);
        return;
    }
    if (!S.alive) {
        if (k == LV_KEY_ENTER) connect_host(&S.cur);
        else if (k == LV_KEY_ESC || k == 'q') set_view(V_HOSTS);
        return;
    }
    term_key(S.term, k);
}

/* ---------------- key view ---------------- */

static void key_path(char *out, size_t n, bool pub)
{
    /* ssh looks in the passwd home, not $HOME, so we do too */
    struct passwd *pw = getpwuid(getuid());
    const char *home = pw && pw->pw_dir ? pw->pw_dir : getenv("HOME");
    snprintf(out, n, "%s/.ssh/id_ed25519%s", home ? home : "/root", pub ? ".pub" : "");
}

static void key_render(void)
{
    char p[512], key[800] = "";
    key_path(p, sizeof p, true);
    FILE *f = fopen(p, "r");
    if (f) {
        if (!fgets(key, sizeof key, f)) key[0] = 0;
        fclose(f);
        key[strcspn(key, "\n")] = 0;
    }
    if (key[0]) {
        lv_qrcode_update(S.qr, key, (uint32_t)strlen(key));
        lv_obj_remove_flag(S.qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(S.key_title, "Your SSH key");
        char t[900];
        snprintf(t, sizeof t, "Add it to ~/.ssh/authorized_keys on your servers (scan the code to copy it).\n\n%s", key);
        lv_label_set_text(S.key_text, t);
    } else {
        lv_obj_add_flag(S.qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(S.key_title, S.keygen_busy ? "Generating\xe2\x80\xa6" : "No SSH key yet");
        lv_label_set_text(S.key_text, S.keygen_busy ? "" :
                          "Press Enter to create one (ed25519, no passphrase).\n"
                          "Passwords work too: ssh asks for them in the terminal.");
    }
}

typedef struct { int status; } keygen_t;

static void keygen_work(void *a)
{
    keygen_t *k = a;
    char priv[512], dir[512], comment[96], host[64] = "zeroos";
    key_path(priv, sizeof priv, false);
    snprintf(dir, sizeof dir, "%s", priv);
    *strrchr(dir, '/') = 0;
    mkdir(dir, 0700);
    gethostname(host, sizeof host);
    snprintf(comment, sizeof comment, "zeroos@%s", host);
    char *argv[] = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", comment, "-f", priv, NULL};
    pid_t pid;
    if (posix_spawnp(&pid, "ssh-keygen", NULL, NULL, argv, environ) != 0) {
        k->status = -1;
        return;
    }
    waitpid(pid, &k->status, 0);
}

static void keygen_done(void *a)
{
    LV_UNUSED(a);
    S.keygen_busy = false;
    key_render();
}

static void key_view_key(uint32_t k)
{
    if (k == LV_KEY_ESC || k == 'q' || k == LV_KEY_BACKSPACE) set_view(V_HOSTS);
    else if (k == LV_KEY_ENTER && !S.keygen_busy) {
        char p[512];
        key_path(p, sizeof p, true);
        if (access(p, R_OK) == 0) return;
        S.keygen_busy = true;
        key_render();
        zos_task_run(&owner_tag, keygen_work, keygen_done, free, calloc(1, sizeof(keygen_t)));
    }
}

/* ---------------- form ---------------- */

static void form_submit(zos_form_t *f, void *ud)
{
    LV_UNUSED(ud);
    host_t *h = &S.edit;
    clean_field(h->name);
    clean_field(h->user);
    clean_field(h->host);
    if (!h->host[0]) {
        zos_form_message(f, "Host is required.", true);
        return;
    }
    if (!h->port[0]) snprintf(h->port, sizeof h->port, "22");
    if (!h->name[0]) snprintf(h->name, sizeof h->name, "%s", h->host);
    if (h->user[0]) snprintf(S.default_user, sizeof S.default_user, "%s", h->user);
    if (S.edit_idx >= 0) {
        S.hosts[S.edit_idx] = *h;
    } else {
        /* reuse an existing entry for the same target instead of duplicating */
        int found = -1;
        for (int i = 0; i < S.nhosts; i++)
            if (!strcmp(S.hosts[i].host, h->host) && !strcmp(S.hosts[i].user, h->user) && !strcmp(S.hosts[i].port, h->port))
                found = i;
        if (found >= 0) S.hosts[found] = *h;
        else if (S.nhosts < MAX_HOSTS) S.hosts[S.nhosts++] = *h;
    }
    save_hosts();
    host_t target = *h;
    lv_obj_delete(zos_form_obj(S.form));
    S.form = NULL;
    connect_host(&target);
}

static void form_cancel(zos_form_t *f, void *ud)
{
    LV_UNUSED(f);
    LV_UNUSED(ud);
    lv_obj_delete(zos_form_obj(S.form));
    S.form = NULL;
    set_view(V_HOSTS);
}

static void open_form(const host_t *h, int idx)
{
    memset(&S.edit, 0, sizeof S.edit);
    if (h) S.edit = *h;
    if (!S.edit.port[0]) snprintf(S.edit.port, sizeof S.edit.port, "22");
    if (!S.edit.user[0]) snprintf(S.edit.user, sizeof S.edit.user, "%s", S.default_user);
    S.edit_idx = idx;
    S.fields[0] = (zos_field_t){"Host", S.edit.host, sizeof S.edit.host, "name, IP or tailnet device", false, false};
    S.fields[1] = (zos_field_t){"User", S.edit.user, sizeof S.edit.user, "login name", false, false};
    S.fields[2] = (zos_field_t){"Port", S.edit.port, sizeof S.edit.port, "22", false, true};
    S.fields[3] = (zos_field_t){"Name", S.edit.name, sizeof S.edit.name, "shown in the list", false, false};
    S.form = zos_form_create(S.scr, S.g, idx >= 0 ? "Edit connection" : "New connection", S.fields, 4,
                             form_submit, form_cancel, NULL);
    S.view = V_FORM;
    lv_obj_add_flag(S.hosts_view, LV_OBJ_FLAG_HIDDEN);
    zos_form_focus(S.form);
}

/* ---------------- hosts view ---------------- */

static void add_item(item_kind_t kind, int idx, const char *label, const char *sub)
{
    if (S.nitems >= (int)(sizeof S.items / sizeof S.items[0])) return;
    item_t *it = &S.items[S.nitems++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->idx = idx;
    snprintf(it->label, sizeof it->label, "%s", label);
    snprintf(it->sub, sizeof it->sub, "%s", sub ? sub : "");
}

static void select_item(int i, int dir)
{
    if (!S.nitems) return;
    if (i < 0) i = 0;
    if (i >= S.nitems) i = S.nitems - 1;
    while (S.items[i].kind == IT_HEADER) {   /* skip section headers */
        i += dir ? dir : 1;
        if (i < 0 || i >= S.nitems) return;
    }
    lv_obj_t *old = lv_obj_get_child(S.list, S.sel);
    if (old) lv_obj_remove_state(old, LV_STATE_CHECKED);
    S.sel = i;
    lv_obj_t *row = lv_obj_get_child(S.list, i);
    lv_obj_add_state(row, LV_STATE_CHECKED);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    S.confirm_delete = false;
    lv_label_set_text(S.hint, S.items[i].kind == IT_SAVED
        ? LV_SYMBOL_NEW_LINE " connect    E edit    D delete    Esc back"
        : LV_SYMBOL_NEW_LINE " open    Esc back");
}

static void hosts_render(void)
{
    S.nitems = 0;
    add_item(IT_NEW, 0, "+  New connection", NULL);
    for (int i = 0; i < S.nhosts; i++) {
        char sub[200];
        snprintf(sub, sizeof sub, "%s%s%s%s%s", S.hosts[i].user, S.hosts[i].user[0] ? "@" : "", S.hosts[i].host,
                 strcmp(S.hosts[i].port, "22") ? ":" : "", strcmp(S.hosts[i].port, "22") ? S.hosts[i].port : "");
        add_item(IT_SAVED, i, S.hosts[i].name, sub);
    }
    const ts_status_t *ts = ts_snapshot();
    if (ts && ts->reachable && !strcmp(ts->state, "Running")) {
        int online = 0;
        for (int i = 0; i < ts->npeers; i++) online += ts->peers[i].online;
        if (online) {
            add_item(IT_HEADER, 0, "Tailnet", NULL);
            for (int i = 0; i < ts->npeers && S.nitems < (int)(sizeof S.items / sizeof S.items[0]) - 2; i++) {
                if (!ts->peers[i].online) continue;
                add_item(IT_TAILNET, i, ts->peers[i].host, ts->peers[i].ip);
                item_t *it = &S.items[S.nitems - 1];
                snprintf(it->ts_host, sizeof it->ts_host, "%s", ts->peers[i].host);
                snprintf(it->ts_ip, sizeof it->ts_ip, "%s", ts->peers[i].ip);
            }
        }
    }
    add_item(IT_KEY, 0, "My SSH key", NULL);

    int keep = S.sel;
    lv_obj_clean(S.list);
    for (int i = 0; i < S.nitems; i++) {
        item_t *it = &S.items[i];
        lv_obj_t *row = lv_obj_create(S.list);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        if (it->kind == IT_HEADER) {
            lv_obj_set_size(row, lv_pct(100), 16);
            lv_obj_t *h = lv_label_create(row);
            lv_obj_set_style_text_font(h, zos_font.small, 0);
            lv_obj_set_style_text_color(h, lv_color_hex(0x818cf8), 0);
            lv_label_set_text(h, "TAILNET DEVICES");
            lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 8, 0);
            continue;
        }
        lv_obj_set_size(row, lv_pct(100), it->sub[0] ? 30 : 22);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(ZOS_COLOR_SURFACE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(row, 3, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(row, lv_color_hex(0x10b981), LV_STATE_CHECKED);
        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_font(l, zos_font.body, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(it->kind == IT_NEW || it->kind == IT_KEY ? 0x34d399 : ZOS_COLOR_TEXT), 0);
        lv_label_set_text(l, it->label);
        zos_label_one_line(l, 280);
        lv_obj_align(l, it->sub[0] ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 0, it->sub[0] ? 1 : 0);
        if (it->sub[0]) {
            lv_obj_t *s = lv_label_create(row);
            lv_obj_set_style_text_font(s, zos_font.small, 0);
            lv_obj_set_style_text_color(s, lv_color_hex(ZOS_COLOR_MUTED), 0);
            lv_label_set_text(s, it->sub);
            zos_label_one_line(s, 280);
            lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, -1);
        }
    }
    S.sel = 0;
    select_item(keep < S.nitems ? keep : 0, 1);
}

static void delete_host(int idx)
{
    for (int i = idx; i < S.nhosts - 1; i++) S.hosts[i] = S.hosts[i + 1];
    S.nhosts--;
    save_hosts();
    hosts_render();
}

static void hosts_key(uint32_t k)
{
    item_t *it = S.nitems ? &S.items[S.sel] : NULL;
    if (k == LV_KEY_DOWN) select_item(S.sel + 1, 1);
    else if (k == LV_KEY_UP) select_item(S.sel - 1, -1);
    else if (k == LV_KEY_ESC || k == 'q' || k == '`' || k == LV_KEY_BACKSPACE) zos_app_exit();
    else if (!it) return;
    else if (k == LV_KEY_ENTER || k == ' ') {
        switch (it->kind) {
        case IT_NEW: open_form(NULL, -1); break;
        case IT_SAVED: connect_host(&S.hosts[it->idx]); break;
        case IT_TAILNET: {
            host_t h = {0};
            snprintf(h.name, sizeof h.name, "%s", it->ts_host);
            snprintf(h.host, sizeof h.host, "%s", it->ts_ip);
            open_form(&h, -1);
            break;
        }
        case IT_KEY: set_view(V_KEY); break;
        default: break;
        }
    } else if (k == 'e' && it->kind == IT_SAVED) {
        open_form(&S.hosts[it->idx], it->idx);
    } else if (k == 'd' && it->kind == IT_SAVED) {
        if (S.confirm_delete) delete_host(it->idx);
        else {
            S.confirm_delete = true;
            lv_label_set_text_fmt(S.hint, "Press D again to delete \"%s\"", S.hosts[it->idx].name);
        }
    }
}

/* ---------------- plumbing ---------------- */

static void sink_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (S.view) {
    case V_HOSTS: hosts_key(k); break;
    case V_TERM: term_view_key(k); break;
    case V_KEY: key_view_key(k); break;
    default: break;
    }
}

static void set_view(view_t v)
{
    S.view = v;
    if (v == V_HOSTS) lv_obj_remove_flag(S.hosts_view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S.hosts_view, LV_OBJ_FLAG_HIDDEN);
    if (v == V_TERM) lv_obj_remove_flag(S.term_view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S.term_view, LV_OBJ_FLAG_HIDDEN);
    if (v == V_KEY) lv_obj_remove_flag(S.key_view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S.key_view, LV_OBJ_FLAG_HIDDEN);
    if (v != V_FORM) {
        lv_group_remove_all_objs(S.g);
        lv_group_add_obj(S.g, S.sink);
        lv_group_focus_obj(S.sink);
    }
    if (v == V_HOSTS) hosts_render();
    if (v == V_KEY) key_render();
}

void ssh_open_host(const char *label, const char *host)
{
    snprintf(pending_label, sizeof pending_label, "%s", label ? label : host);
    snprintf(pending_host, sizeof pending_host, "%s", host);
    zos_app_switch(&zos_app_ssh);
}

static void ssh_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&S, 0, sizeof S);
    S.scr = scr;
    S.g = g;
    S.fd = -1;
    const char *u = getenv("ZOS_SSH_USER");
    if (!u) u = getenv("USER");
    snprintf(S.default_user, sizeof S.default_user, "%s", u ? u : "pi");
    load_hosts();
    ts_poke();

    S.hosts_view = lv_obj_create(scr);
    lv_obj_remove_style_all(S.hosts_view);
    lv_obj_set_size(S.hosts_view, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_remove_flag(S.hosts_view, LV_OBJ_FLAG_SCROLLABLE);
    zos_statusbar_create(S.hosts_view, "SSH");
    S.list = lv_obj_create(S.hosts_view);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_pos(S.list, 0, ZOS_STATUSBAR_H + 1);
    lv_obj_set_size(S.list, ZOS_HOR_RES, ZOS_VER_RES - ZOS_STATUSBAR_H - 14);
    lv_obj_set_style_pad_hor(S.list, 4, 0);
    lv_obj_set_style_pad_row(S.list, 1, 0);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(S.list, LV_SCROLLBAR_MODE_OFF);
    S.hint = lv_label_create(S.hosts_view);
    lv_obj_set_style_text_font(S.hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(S.hint, lv_color_hex(0x5b6472), 0);
    lv_obj_align(S.hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    S.term_view = lv_obj_create(scr);
    lv_obj_remove_style_all(S.term_view);
    lv_obj_set_size(S.term_view, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_remove_flag(S.term_view, LV_OBJ_FLAG_SCROLLABLE);
    S.menu = lv_obj_create(S.term_view);
    lv_obj_remove_style_all(S.menu);
    lv_obj_set_size(S.menu, ZOS_HOR_RES, 15);
    lv_obj_align(S.menu, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(S.menu, 235, 0);
    lv_obj_set_style_bg_color(S.menu, lv_color_hex(0x10b981), 0);
    S.menu_lbl = lv_label_create(S.menu);
    lv_obj_set_style_text_font(S.menu_lbl, &lv_font_montserrat_10, 0);   /* has the arrow symbols */
    lv_obj_set_style_text_color(S.menu_lbl, lv_color_hex(0x04130d), 0);
    lv_obj_center(S.menu_lbl);
    lv_obj_add_flag(S.menu, LV_OBJ_FLAG_HIDDEN);
    S.scroll_lbl = lv_label_create(S.term_view);
    lv_obj_set_style_text_font(S.scroll_lbl, zos_font.small, 0);
    lv_obj_set_style_bg_opa(S.scroll_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(S.scroll_lbl, lv_color_hex(0xf59e0b), 0);
    lv_obj_set_style_text_color(S.scroll_lbl, lv_color_hex(0x111111), 0);
    lv_obj_set_style_pad_hor(S.scroll_lbl, 4, 0);
    lv_obj_align(S.scroll_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(S.scroll_lbl, LV_OBJ_FLAG_HIDDEN);

    S.key_view = lv_obj_create(scr);
    lv_obj_remove_style_all(S.key_view);
    lv_obj_set_size(S.key_view, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_set_style_bg_opa(S.key_view, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(S.key_view, lv_color_hex(ZOS_COLOR_BG), 0);
    lv_obj_remove_flag(S.key_view, LV_OBJ_FLAG_SCROLLABLE);
    zos_statusbar_create(S.key_view, "SSH \xc2\xb7 My key");
    S.qr = lv_qrcode_create(S.key_view);
    lv_qrcode_set_size(S.qr, 128);
    lv_qrcode_set_dark_color(S.qr, lv_color_black());
    lv_qrcode_set_light_color(S.qr, lv_color_white());
    lv_obj_set_style_border_width(S.qr, 3, 0);
    lv_obj_set_style_border_color(S.qr, lv_color_white(), 0);
    lv_obj_set_pos(S.qr, 6, ZOS_STATUSBAR_H + 4);
    S.key_title = lv_label_create(S.key_view);
    lv_obj_set_style_text_font(S.key_title, zos_font.title, 0);
    lv_obj_set_pos(S.key_title, 146, ZOS_STATUSBAR_H + 4);
    S.key_text = lv_label_create(S.key_view);
    lv_obj_set_style_text_font(S.key_text, zos_font.small, 0);
    lv_obj_set_style_text_color(S.key_text, lv_color_hex(ZOS_COLOR_MUTED), 0);
    lv_obj_set_width(S.key_text, 168);
    lv_label_set_long_mode(S.key_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(S.key_text, 146, ZOS_STATUSBAR_H + 26);

    S.sink = lv_obj_create(scr);
    lv_obj_remove_style_all(S.sink);
    lv_obj_set_size(S.sink, 1, 1);
    lv_obj_add_event_cb(S.sink, sink_key, LV_EVENT_KEY, NULL);

    S.io = lv_timer_create(io_tick, 10, NULL);

    set_view(V_HOSTS);
    if (pending_host[0]) {   /* opened from the Tailscale app */
        host_t h = {0};
        snprintf(h.name, sizeof h.name, "%s", pending_label);
        snprintf(h.host, sizeof h.host, "%s", pending_host);
        pending_host[0] = 0;
        open_form(&h, -1);
    }
}

static void ssh_close(void)
{
    zos_task_cancel(&owner_tag);
    if (S.alive) {
        kill(S.pid, SIGHUP);
        waitpid(S.pid, NULL, 0);
        close(S.fd);
    }
    lv_timer_delete(S.io);
    if (S.term) term_destroy(S.term);
    memset(&S, 0, sizeof S);
}

const zos_app_t zos_app_ssh = {
    .id = "ssh", .name = "SSH", .tagline = "Terminal for your servers",
    .icon = &zos_icon_terminal, .color = 0x0f9f6e, .open = ssh_open, .close = ssh_close,
};
