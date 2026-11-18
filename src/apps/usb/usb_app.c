/* USB Drive: connect the CardputerZero to a Mac or PC over USB-C and it
 * shows up as a drive (ZEROOS) with the Music, Videos, Pictures and Books
 * folders, to copy files on and off.
 *
 * The work is done by a root helper, /usr/local/sbin/zeroos-usb, run with
 * sudo: it switches the USB controller between host and device mode (a
 * restart), and shares the drive with the computer as a USB mass-storage
 * gadget, unmounting it here while the computer has it.
 * ZOS_USB_HELPER replaces the helper (tests and the simulator). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/player.h"
#include "../../core/proc.h"
#include "../../core/settings.h"
#include "../../core/task.h"
#include "../../core/ui.h"
#include "../../zos.h"
#include "../music/music.h"

extern const lv_image_dsc_t zos_icon_usb;
extern const zos_app_t zos_app_usb;

typedef enum { ST_LOADING, ST_NO_DRIVE, ST_HOST, ST_READY, ST_ACTIVE, ST_ERROR } state_t;

typedef struct {
    char udc[32], config_mode[16], runtime_mode[16], storage[16], usb_state[32], lun_file[256], error[200];
    bool mounted, active;
    long long size_kb, free_kb, sys_free_kb;
} status_t;

static const int sizes_gb[] = {2, 4, 8, 16, 32, 64, 128};

static struct {
    lv_obj_t *scr, *sink, *tile, *title, *body, *detail, *hint;
    status_t st;
    state_t state;
    bool busy, have, confirm, reboot_ready;
    int size_idx;
    char msg[200];
    lv_timer_t *poll;
} U;

static const char owner_tag;
static void render(void);

/* ---------------- helper ---------------- */

static const char *helper(void)
{
    const char *h = getenv("ZOS_USB_HELPER");
    return h ? h : NULL;
}

static int run_helper(const char *a1, const char *a2, char *out, size_t cap)
{
    const char *h = helper();
    if (h) {
        const char *argv[] = {h, a1, a2, NULL};
        return zos_run(argv, out, cap, 60000);
    }
    const char *argv[] = {"sudo", "-n", "/usr/local/sbin/zeroos-usb", a1, a2, NULL};
    return zos_run(argv, out, cap, 60000);
}

static void parse_status(const char *out, status_t *s)
{
    memset(s, 0, sizeof *s);
    char *copy = strdup(out), *save = NULL;
    for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = l, *v = eq + 1;
#define STR(name) if (!strcmp(k, #name)) snprintf(s->name, sizeof s->name, "%s", v)
        STR(udc); STR(config_mode); STR(runtime_mode); STR(storage); STR(usb_state); STR(lun_file); STR(error);
#undef STR
        if (!strcmp(k, "mounted")) s->mounted = atoi(v);
        if (!strcmp(k, "active")) s->active = atoi(v);
        if (!strcmp(k, "size_kb")) s->size_kb = atoll(v);
        if (!strcmp(k, "free_kb")) s->free_kb = atoll(v);
        if (!strcmp(k, "sys_free_kb")) s->sys_free_kb = atoll(v);
    }
    free(copy);
}

typedef struct {
    char a1[24], a2[24];
    int rc;
    char out[2048];
    status_t st;
} job_t;

static void job_work(void *a)
{
    job_t *j = a;
    if (j->a1[0]) j->rc = run_helper(j->a1, j->a2[0] ? j->a2 : NULL, j->out, sizeof j->out);
    char s[2048] = "";
    run_helper("status", NULL, s, sizeof s);
    parse_status(s, &j->st);
}

static void job_done(void *a)
{
    job_t *j = a;
    bool was_active = U.st.active;
    U.st = j->st;
    U.have = true;
    U.busy = false;
    if (j->a1[0]) {
        char *err = strstr(j->out, "error=");
        if (!strcmp(j->a1, "mode")) {
            if (j->rc == 10 || strstr(j->out, "restart=1")) U.reboot_ready = true;
            else if (j->rc != 0) snprintf(U.msg, sizeof U.msg, "%s", err ? err + 6 : "Could not change the USB mode");
        } else if (j->rc != 0) {
            snprintf(U.msg, sizeof U.msg, "%s", err ? err + 6 : j->out[0] ? j->out : "Something went wrong");
            char *nl = strchr(U.msg, '\n');
            if (nl) *nl = 0;
        } else {
            U.msg[0] = 0;
        }
        if (!strcmp(j->a1, "stop") && j->rc == 0) {
            zos_notify(0x0ea5e9, "USB drive", "Back on the CardputerZero");
            music_scan(NULL);   /* pick up what was copied on */
        }
    }
    if (was_active && !U.st.active) U.confirm = false;
    render();
}

static void job_release(void *a) { free(a); }

static void run(const char *a1, const char *a2)
{
    if (U.busy) return;
    U.busy = true;
    job_t *j = calloc(1, sizeof *j);
    if (a1) snprintf(j->a1, sizeof j->a1, "%s", a1);
    if (a2) snprintf(j->a2, sizeof j->a2, "%s", a2);
    zos_task_run(&owner_tag, job_work, job_done, job_release, j);
}

/* ---------------- states ---------------- */

static state_t derive(void)
{
    if (!U.have) return ST_LOADING;
    if (U.st.active) return ST_ACTIVE;
    if (!strcmp(U.st.storage, "none") || !U.st.storage[0]) return ST_NO_DRIVE;
    if (strcmp(U.st.runtime_mode, "device")) return ST_HOST;
    return ST_READY;
}

static void fmt_gb(long long kb, char *out, size_t n)
{
    if (kb >= 1024LL * 1024 * 10) snprintf(out, n, "%.0f GB", kb / 1048576.0);
    else snprintf(out, n, "%.1f GB", kb / 1048576.0);
}

static int max_size_idx(void)
{
    long long free_gb = U.st.sys_free_kb / 1048576 - 2;   /* keep 2 GB for the system */
    int m = -1;
    for (int i = 0; i < (int)(sizeof sizes_gb / sizeof sizes_gb[0]); i++)
        if (sizes_gb[i] <= free_gb) m = i;
    return m;
}

static void render(void)
{
    if (!U.scr) return;
    U.state = derive();
    char body[400] = "", detail[300] = "", hint[200] = "";
    uint32_t tile = 0x0ea5e9;
    const char *title = "USB drive";
    switch (U.state) {
    case ST_LOADING:
        snprintf(body, sizeof body, "Checking\xe2\x80\xa6");
        snprintf(hint, sizeof hint, "Esc back");
        break;
    case ST_NO_DRIVE: {
        int m = max_size_idx();
        if (U.size_idx > m) U.size_idx = m;
        if (m < 0) {
            snprintf(body, sizeof body, "There's no room on the SD card for a drive area (it needs 4 GB free).");
            snprintf(hint, sizeof hint, "Esc back");
        } else {
            char fr[24];
            fmt_gb(U.st.sys_free_kb, fr, sizeof fr);
            title = "Set up the USB drive";
            snprintf(body, sizeof body, "Drive size: %d GB of the SD card, for Music, Videos, Pictures and Books.",
                     sizes_gb[U.size_idx]);
            snprintf(detail, sizeof detail, "%s free on the card. Files already in those folders move onto it.", fr);
            snprintf(hint, sizeof hint, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " size   " LV_SYMBOL_NEW_LINE " create   Esc back");
        }
        break;
    }
    case ST_HOST:
        tile = 0x52525b;
        if (U.reboot_ready || !strcmp(U.st.config_mode, "device")) {
            title = "Restart to switch";
            snprintf(body, sizeof body, "USB-C switches to drive mode after a restart. ZeroOS opens this screen again.");
            snprintf(hint, sizeof hint, LV_SYMBOL_NEW_LINE " restart now   H stay in host mode   Esc back");
        } else {
            title = "USB-C is in host mode";
            snprintf(body, sizeof body, "To be a drive, the USB controller has to switch to device mode. That takes a restart.");
            snprintf(detail, sizeof detail, "While in device mode the USB-A port and the USB hub are off (H here switches back).");
            snprintf(hint, sizeof hint, LV_SYMBOL_NEW_LINE " switch and restart   Esc back");
        }
        break;
    case ST_READY: {
        char sz[24], fr[24];
        fmt_gb(U.st.size_kb, sz, sizeof sz);
        fmt_gb(U.st.free_kb, fr, sizeof fr);
        title = "Ready";
        snprintf(body, sizeof body, "Connect the CardputerZero to your computer with a USB-C cable, then press Enter.");
        snprintf(detail, sizeof detail, "Drive ZEROOS: %s, %s free. Music and video stop while it's shared.",
                 U.st.mounted ? sz : "?", U.st.mounted ? fr : "?");
        snprintf(hint, sizeof hint, LV_SYMBOL_NEW_LINE " share   H back to host mode (USB-A)   Esc back");
        if (!strcmp(U.st.config_mode, "host")) snprintf(hint, sizeof hint, LV_SYMBOL_NEW_LINE " share   (host mode after the next restart)   Esc back");
        break;
    }
    case ST_ACTIVE: {
        bool configured = !strcmp(U.st.usb_state, "configured");
        bool ejected = U.st.lun_file[0] == 0;
        tile = ejected ? 0x22c55e : configured ? 0x0ea5e9 : 0xf5a524;
        if (ejected) {
            title = "Ejected";
            snprintf(body, sizeof body, "The computer has let go of the drive.");
            snprintf(hint, sizeof hint, LV_SYMBOL_NEW_LINE " / Esc finish");
        } else if (configured) {
            title = "Connected";
            snprintf(body, sizeof body, "Copy files to ZEROOS on your computer. When you're done, eject it there, then press Esc.");
            snprintf(hint, sizeof hint, "Esc finish");
        } else {
            title = "Waiting for a computer";
            snprintf(body, sizeof body, "Plug the USB-C cable into your Mac or PC. If nothing happens, try the other USB-C port.");
            snprintf(hint, sizeof hint, "Esc stop");
        }
        if (U.confirm)
            snprintf(detail, sizeof detail, "The computer still has the drive. Eject it there first, or press Esc again to take it back anyway.");
        break;
    }
    case ST_ERROR:
        break;
    }
    if (U.msg[0]) snprintf(detail, sizeof detail, "%s", U.msg);
    if (U.busy && U.state != ST_LOADING) snprintf(hint, sizeof hint, "Working...");
    lv_obj_set_style_bg_color(U.tile, lv_color_hex(tile), 0);
    lv_label_set_text(U.title, title);
    lv_label_set_text(U.body, body);
    lv_label_set_text(U.detail, detail);
    lv_obj_set_style_text_color(U.detail, lv_color_hex(U.msg[0] ? 0xf87171 : ZOS_COLOR_MUTED), 0);
    lv_label_set_text(U.hint, hint);
}

/* ---------------- keys ---------------- */

static void reboot(void)
{
    zos_setting_set_str("start_app", "usb");   /* come back here after the restart */
    zos_notify(0x52525b, "Restarting", "Back in a minute");
    const char *argv[] = {"systemctl", "reboot", NULL};
    zos_run_bg(argv);
}

static void stop_media(void)
{
    if (music_active()) music_stop();
    if (zos_player_state()->running) zos_player_stop();
}

static void sink_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    bool esc = k == LV_KEY_ESC || k == LV_KEY_BACKSPACE || k == '`';
    if (U.busy) return;
    switch (U.state) {
    case ST_NO_DRIVE:
        if (k == LV_KEY_LEFT && U.size_idx > 0) U.size_idx--;
        else if (k == LV_KEY_RIGHT && U.size_idx < max_size_idx()) U.size_idx++;
        else if (k == LV_KEY_ENTER && max_size_idx() >= 0) {
            char mb[16];
            snprintf(mb, sizeof mb, "%d", sizes_gb[U.size_idx] * 1024);
            stop_media();
            run("create-file", mb);
        } else if (esc) { zos_app_exit(); return; }
        break;
    case ST_HOST:
        if (k == LV_KEY_ENTER) {
            if (U.reboot_ready || !strcmp(U.st.config_mode, "device")) reboot();
            else run("mode", "device");
        } else if (k == 'h' || k == 'H') {
            U.reboot_ready = false;
            run("mode", "host");
        } else if (esc) { zos_app_exit(); return; }
        break;
    case ST_READY:
        if (k == LV_KEY_ENTER || k == ' ') {
            stop_media();
            run("start", NULL);
        } else if (k == 'h' || k == 'H') {
            run("mode", "host");
        } else if (esc) { zos_app_exit(); return; }
        break;
    case ST_ACTIVE: {
        bool held = U.st.lun_file[0] && !strcmp(U.st.usb_state, "configured");
        if (esc || k == LV_KEY_ENTER) {
            if (held && !U.confirm && esc) U.confirm = true;
            else if (held && k == LV_KEY_ENTER) break;
            else { U.confirm = false; run("stop", NULL); }
        }
        break;
    }
    default:
        if (esc) { zos_app_exit(); return; }
    }
    render();
}

static void poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!U.busy) run(NULL, NULL);
}

/* ---------------- build ---------------- */

static void usb_open(lv_obj_t *scr, lv_group_t *g)
{
    memset(&U, 0, sizeof U);
    U.scr = scr;
    U.size_idx = 2;   /* 8 GB */
    zos_statusbar_create(scr, "USB drive");
    U.tile = zos_ui_box(scr);
    lv_obj_set_size(U.tile, 52, 52);
    lv_obj_set_style_radius(U.tile, 14, 0);
    lv_obj_set_style_bg_opa(U.tile, LV_OPA_COVER, 0);
    lv_obj_set_pos(U.tile, 14, ZOS_STATUSBAR_H + 10);
    lv_obj_t *ic = lv_image_create(U.tile);
    lv_image_set_src(ic, &zos_icon_usb);
    lv_obj_set_style_image_recolor(ic, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
    lv_obj_center(ic);
    int32_t x = 80, w = ZOS_HOR_RES - x - 10;
    U.title = zos_ui_label(scr, zos_font.title, ZOS_COLOR_TEXT);
    zos_label_one_line(U.title, w);
    lv_obj_set_pos(U.title, x, ZOS_STATUSBAR_H + 8);
    U.body = zos_ui_label(scr, zos_font.small, ZOS_COLOR_TEXT);
    lv_obj_set_width(U.body, w);
    lv_label_set_long_mode(U.body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(U.body, x, ZOS_STATUSBAR_H + 30);
    U.detail = zos_ui_label(scr, zos_font.small, ZOS_COLOR_MUTED);
    lv_obj_set_width(U.detail, ZOS_HOR_RES - 28);
    lv_label_set_long_mode(U.detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(U.detail, 14, ZOS_STATUSBAR_H + 92);
    U.hint = zos_ui_hint(scr, "");
    U.sink = zos_ui_box(scr);
    lv_obj_set_size(U.sink, 1, 1);
    lv_obj_add_event_cb(U.sink, sink_key, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g, U.sink);
    lv_group_focus_obj(U.sink);
    render();
    run(NULL, NULL);
    U.poll = lv_timer_create(poll_cb, 1000, NULL);
}

static void usb_close(void)
{
    lv_timer_delete(U.poll);
    zos_task_cancel(&owner_tag);
    /* Never leave the drive shared with nobody watching it. */
    if (U.st.active) {
        char out[256];
        run_helper("stop", NULL, out, sizeof out);
    }
    memset(&U, 0, sizeof U);
}

const zos_app_t zos_app_usb = {
    .id = "usb", .name = "USB Drive", .tagline = "Copy files from a Mac or PC",
    .icon = &zos_icon_usb, .color = 0x0ea5e9, .open = usb_open, .close = usb_close,
};
