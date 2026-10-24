/* Meshtastic and MeshCore apps: one UI over the mesh service.
 *
 *   Chats    channels first, then direct messages by recent activity
 *   Chat     message bubbles + a one-line composer that always has focus
 *   Nodes    who the node has heard; Enter starts a direct message
 *
 * The connection lives in the mesh service, not here, so closing the app
 * keeps the radio and notifications going. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../mesh/mesh.h"
#include "../../zos.h"

extern const lv_image_dsc_t zos_icon_meshtastic, zos_icon_meshcore;

typedef enum { V_CHATS, V_CHAT, V_NODES, V_TAKEOVER } view_t;

static struct {
    const zos_app_t *app;
    mesh_t *m;
    lv_obj_t *scr;
    lv_group_t *group;
    view_t view;
    uint32_t seen_version;
    lv_timer_t *refresh;

    lv_obj_t *chats, *chat_status_dot, *chat_status, *chat_radio, *chat_list, *chat_hint;
    int chat_sel, chat_rows;

    lv_obj_t *convv, *conv_title, *conv_sub, *msg_list, *compose, *counter;
    char conv_key[MESH_KEY_LEN];
    int rendered;                  /* messages already in msg_list */
    lv_obj_t **status_lbls;        /* per rendered message (outgoing only, else NULL) */
    int status_cap;

    lv_obj_t *nodesv, *node_title, *node_list;
    int node_sel, node_rows;

    lv_obj_t *takeover, *focus_sink;

    /* keys copied out of the model: model arrays move when they grow */
    char (*row_keys)[MESH_KEY_LEN];
    int row_cap;
    char (*node_ids)[MESH_KEY_LEN];
    int node_cap;
} U;

/* ======================= small helpers ======================= */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void ago(uint32_t ts, char *out, size_t cap)
{
    if (!ts || cap < 8) { snprintf(out, cap, "-"); return; }
    long d = (long)time(NULL) - (long)ts;
    int v;
    char unit;
    if (d < 60) { snprintf(out, cap, "now"); return; }
    if (d < 3600) v = (int)(d / 60), unit = 'm';
    else if (d < 86400) v = (int)(d / 3600), unit = 'h';
    else v = (int)(d / 86400 > 999 ? 999 : d / 86400), unit = 'd';
    snprintf(out, cap, "%d%c", v, unit);
}

static const char *hops_text(int hops)
{
    static char buf[4][24];
    static int i;
    char *b = buf[i++ & 3];
    if (hops <= 0) snprintf(b, 24, "direct");
    else snprintf(b, 24, "%d hop%s", hops > 99 ? 99 : hops, hops == 1 ? "" : "s");
    return b;
}

static void clock_of(uint32_t ts, char *out, size_t cap)
{
    time_t t = ts;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* A stable colour per sender name, so channel chatter is easy to follow. */
static uint32_t name_color(const char *s)
{
    static const uint32_t pal[] = {0xf59e0b, 0x34d399, 0x60a5fa, 0xf472b6, 0xa78bfa, 0xfb923c, 0x2dd4bf};
    uint32_t h = 5381;
    for (; s && *s; s++) h = h * 33 + (uint8_t)*s;
    return pal[h % (sizeof pal / sizeof *pal)];
}

static uint32_t state_color(mesh_state_t st)
{
    return st == MESH_READY ? 0x22c55e : st == MESH_SYNCING ? 0xf59e0b : st == MESH_OFF ? 0x6b7280 : 0xef4444;
}

static void focus(lv_obj_t *o)
{
    lv_group_remove_all_objs(U.group);
    lv_group_add_obj(U.group, o);
    lv_group_focus_obj(o);
}

static void set_view(view_t v);

/* ======================= Chats ======================= */

static void chats_select(int i)
{
    int n = (int)lv_obj_get_child_count(U.chat_list);
    if (!n) return;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    lv_obj_t *old = lv_obj_get_child(U.chat_list, U.chat_sel);
    if (old) lv_obj_remove_state(old, LV_STATE_CHECKED);
    U.chat_sel = i;
    lv_obj_t *row = lv_obj_get_child(U.chat_list, i);
    lv_obj_add_state(row, LV_STATE_CHECKED);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

static void set_status_line(const char *text)
{
    lv_label_set_text(U.chat_radio, U.m->radio_desc);
    lv_obj_update_layout(U.chat_radio);
    int32_t room = ZOS_HOR_RES - 21 - 8 - 10 - (U.m->radio_desc[0] ? lv_obj_get_width(U.chat_radio) : 0);
    zos_label_one_line(U.chat_status, room);
    lv_label_set_text(U.chat_status, text);
}

static void chats_rebuild(void)
{
    mesh_t *m = U.m;
    lv_obj_set_style_bg_color(U.chat_status_dot, lv_color_hex(state_color(m->state)), 0);
    set_status_line(m->status);

    char sel_key[MESH_KEY_LEN] = "";
    if (U.row_keys && U.chat_sel < (int)lv_obj_get_child_count(U.chat_list))
        snprintf(sel_key, sizeof sel_key, "%s", U.row_keys[U.chat_sel]);
    lv_obj_clean(U.chat_list);

    mesh_conv_t **convs;
    int n = mesh_sorted_convs(m, &convs);
    if (n > U.row_cap) {
        U.row_cap = n + 16;
        U.row_keys = lv_realloc(U.row_keys, sizeof *U.row_keys * U.row_cap);
    }
    static lv_style_t st_row, st_sel;
    static bool inited;
    if (!inited) {
        inited = true;
        lv_style_init(&st_row);
        lv_style_set_width(&st_row, lv_pct(100));
        lv_style_set_height(&st_row, 31);
        lv_style_set_pad_hor(&st_row, 8);
        lv_style_set_radius(&st_row, 6);
        lv_style_init(&st_sel);
        lv_style_set_bg_opa(&st_sel, LV_OPA_COVER);
        lv_style_set_bg_color(&st_sel, lv_color_hex(ZOS_COLOR_SURFACE));
        lv_style_set_border_side(&st_sel, LV_BORDER_SIDE_LEFT);
        lv_style_set_border_width(&st_sel, 3);
        lv_style_set_border_color(&st_sel, lv_color_hex(U.app->color));
    }
    lv_style_set_border_color(&st_sel, lv_color_hex(U.app->color));

    int new_sel = 0;
    for (int i = 0; i < n; i++) {
        mesh_conv_t *c = convs[i];
        lv_obj_t *row = box(U.chat_list);
        lv_obj_add_style(row, &st_row, 0);
        lv_obj_add_style(row, &st_sel, LV_STATE_CHECKED);
        snprintf(U.row_keys[i], MESH_KEY_LEN, "%s", c->key);
        if (!strcmp(c->key, sel_key)) new_sel = i;

        char title[64];
        snprintf(title, sizeof title, "%s%s", c->is_channel ? "# " : "", c->title);
        lv_obj_t *t = label(row, zos_font.body, c->known || c->is_channel ? ZOS_COLOR_TEXT : ZOS_COLOR_MUTED, title);
        zos_label_one_line(t, 230);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 1);

        char prev[96] = "No messages yet";
        if (c->n) {
            mesh_msg_t *last = &c->msgs[c->n - 1];
            snprintf(prev, sizeof prev, "%s%s%s", last->outgoing ? "You" : (last->from ? last->from : ""),
                     ": ", last->text);
        }
        lv_obj_t *p = label(row, zos_font.small, ZOS_COLOR_MUTED, prev);
        zos_label_one_line(p, 250);
        lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, 0, -1);

        if (c->unread) {
            char b[16];
            snprintf(b, sizeof b, "%d", c->unread > 99 ? 99 : c->unread);
            lv_obj_t *badge = label(row, zos_font.small, 0x111111, b);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(U.app->color), 0);
            lv_obj_set_style_radius(badge, 8, 0);
            lv_obj_set_style_pad_hor(badge, 5, 0);
            lv_obj_set_style_min_width(badge, 16, 0);
            lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 0, 2);
        } else if (c->last_ts) {
            char a[16];
            ago(c->last_ts, a, sizeof a);
            lv_obj_t *al = label(row, zos_font.small, ZOS_COLOR_MUTED, a);
            lv_obj_align(al, LV_ALIGN_TOP_RIGHT, 0, 3);
        }
    }
    U.chat_rows = n;
    if (!n) {
        lv_obj_t *e = label(U.chat_list, zos_font.small, ZOS_COLOR_MUTED,
                            m->state == MESH_READY ? "No channels or chats yet." : "");
        lv_obj_set_style_pad_all(e, 8, 0);
    }
    U.chat_sel = 0;
    if (n) chats_select(new_sel);
}

static void open_conv_key(const char *key);

static void chats_key(uint32_t k)
{
    if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT) chats_select(U.chat_sel + 1);
    else if (k == LV_KEY_UP || k == LV_KEY_LEFT) chats_select(U.chat_sel - 1);
    else if (k == LV_KEY_ENTER || k == ' ') {
        if (U.chat_sel < U.chat_rows) {
            char key[MESH_KEY_LEN];
            snprintf(key, sizeof key, "%s", U.row_keys[U.chat_sel]);
            open_conv_key(key);
        }
    } else if (k == 'n' || k == LV_KEY_NEXT) set_view(V_NODES);
    else if (k == 'a' && U.m->be->advert && U.m->state == MESH_READY) {
        U.m->be->advert(U.m);
        set_status_line("Advert sent");
    } else if (k == LV_KEY_ESC || k == 'q' || k == '`' || k == LV_KEY_BACKSPACE) zos_app_exit();
}

/* ======================= Chat ======================= */

static mesh_conv_t *cur_conv(void) { return mesh_conv_find(U.m, U.conv_key); }

static const char *status_text(const mesh_msg_t *msg)
{
    switch (msg->status) {
    case MSG_SENDING: return "\xe2\x80\xa6";            /* … */
    case MSG_SENT: return "\xe2\x9c\x93";               /* ✓ */
    case MSG_DELIVERED: return "\xe2\x9c\x93\xe2\x9c\x93"; /* ✓✓ */
    case MSG_FAILED: return "failed";
    default: return "";
    }
}

static void fill_meta(lv_obj_t *meta, const mesh_conv_t *c, const mesh_msg_t *msg)
{
    char t[8], s[64];
    clock_of(msg->ts, t, sizeof t);
    if (msg->outgoing) {
        snprintf(s, sizeof s, "%s  %s", t, status_text(msg));
        lv_obj_set_style_text_color(meta, lv_color_hex(msg->status == MSG_FAILED ? 0xfca5a5 : 0x3b2a06), 0);
    } else if (!isnan(msg->snr) && msg->hops >= 0) {
        snprintf(s, sizeof s, "%s \xc2\xb7 %.1f dB \xc2\xb7 %s", t, msg->snr, hops_text(msg->hops));
    } else if (!isnan(msg->snr)) {
        snprintf(s, sizeof s, "%s \xc2\xb7 %.1f dB", t, msg->snr);
    } else {
        snprintf(s, sizeof s, "%s", t);
    }
    LV_UNUSED(c);
    lv_label_set_text(meta, s);
}

static void append_msg(const mesh_conv_t *c, const mesh_msg_t *msg, int idx)
{
    lv_obj_t *row = box(U.msg_list);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);

    lv_obj_t *b = box(row);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, 236, 0);
    lv_obj_set_style_pad_hor(b, 7, 0);
    lv_obj_set_style_pad_ver(b, 3, 0);
    lv_obj_set_style_radius(b, 9, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, 0, 0);
    if (msg->outgoing) {
        lv_obj_set_style_bg_color(b, lv_color_hex(msg->status == MSG_FAILED ? 0x7f1d1d : U.app->color), 0);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        lv_obj_set_style_bg_color(b, lv_color_hex(0x252b34), 0);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    if (!msg->outgoing && c->is_channel && msg->from) {
        lv_obj_t *who = label(b, zos_font.small, name_color(msg->from), msg->from);
        lv_label_set_long_mode(who, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(who, 222, 0);
    }
    lv_obj_t *txt = label(b, zos_font.body, msg->outgoing ? 0x111111 : ZOS_COLOR_TEXT, msg->text);
    lv_obj_set_style_max_width(txt, 222, 0);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *meta = label(b, zos_font.small, ZOS_COLOR_MUTED, "");
    fill_meta(meta, c, msg);
    if (msg->outgoing) lv_obj_set_style_align(meta, LV_ALIGN_RIGHT_MID, 0);

    if (idx >= U.status_cap) {
        U.status_cap = idx + 64;
        U.status_lbls = lv_realloc(U.status_lbls, sizeof *U.status_lbls * U.status_cap);
    }
    U.status_lbls[idx] = msg->outgoing ? meta : NULL;
}

static void conv_refresh(bool scroll)
{
    mesh_conv_t *c = cur_conv();
    if (!c) return;
    if (c->n < U.rendered) {            /* trimmed history: rebuild */
        lv_obj_clean(U.msg_list);
        U.rendered = 0;
    }
    bool added = c->n > U.rendered;
    for (int i = U.rendered; i < c->n; i++) append_msg(c, &c->msgs[i], i);
    U.rendered = c->n;
    for (int i = 0; i < c->n; i++)
        if (U.status_lbls[i]) {
            fill_meta(U.status_lbls[i], c, &c->msgs[i]);
            lv_obj_t *bubble = lv_obj_get_parent(U.status_lbls[i]);
            lv_obj_set_style_bg_color(bubble, lv_color_hex(c->msgs[i].status == MSG_FAILED ? 0x7f1d1d : U.app->color), 0);
        }
    if (added || scroll) {
        lv_obj_update_layout(U.msg_list);
        lv_obj_scroll_to_y(U.msg_list, LV_COORD_MAX, LV_ANIM_OFF);
    }

    /* subtitle: link quality for DMs, node count for channels */
    char sub[64] = "";
    if (c->is_channel) {
        int nodes = 0;
        for (int i = 0; i < U.m->nnode; i++) nodes += !U.m->nodes[i].is_self;
        snprintf(sub, sizeof sub, "channel \xc2\xb7 %d node%s", nodes, nodes == 1 ? "" : "s");
    } else {
        mesh_node_t *nd = mesh_node_find(U.m, c->peer);
        if (nd && !isnan(nd->snr) && nd->hops >= 0) snprintf(sub, sizeof sub, "%.1f dB \xc2\xb7 %s", nd->snr, hops_text(nd->hops));
        else if (nd && nd->hops >= 0) snprintf(sub, sizeof sub, "%s", hops_text(nd->hops));
        else snprintf(sub, sizeof sub, "direct message");
    }
    lv_label_set_text(U.conv_sub, sub);
    mesh_set_viewing(U.m, c);
}

static void open_conv_key(const char *key)
{
    snprintf(U.conv_key, sizeof U.conv_key, "%s", key);
    mesh_conv_t *c = cur_conv();
    if (!c) return;
    char title[64];
    snprintf(title, sizeof title, "%s%s", c->is_channel ? "# " : "", c->title);
    lv_label_set_text(U.conv_title, title);
    lv_obj_clean(U.msg_list);
    U.rendered = 0;
    lv_textarea_set_text(U.compose, "");
    lv_textarea_set_max_length(U.compose, (uint32_t)U.m->be->max_text);
    set_view(V_CHAT);
    conv_refresh(true);
}

static void update_counter(void)
{
    const char *t = lv_textarea_get_text(U.compose);
    int left = U.m->be->max_text - (int)strlen(t);
    if (left < 40) {
        char s[16];
        snprintf(s, sizeof s, "%d", left);
        lv_label_set_text(U.counter, s);
        show(U.counter, true);
    } else {
        show(U.counter, false);
    }
}

static void compose_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        update_counter();
    } else if (code == LV_EVENT_READY) {
        const char *t = lv_textarea_get_text(U.compose);
        mesh_conv_t *c = cur_conv();
        if (!c || !*t) return;
        if (U.m->state != MESH_READY) {
            lv_label_set_text(U.conv_sub, "Not connected \xe2\x80\x94 message not sent");
            return;
        }
        char text[256];
        snprintf(text, sizeof text, "%s", t);
        if (U.m->be->send(U.m, c, text)) {
            lv_textarea_set_text(U.compose, "");
            conv_refresh(true);
        } else {
            lv_label_set_text(U.conv_sub, "Could not send");
        }
    } else if (code == LV_EVENT_KEY) {
        uint32_t k = lv_event_get_key(e);
        if (k == LV_KEY_ESC) {
            mesh_set_viewing(NULL, NULL);
            set_view(V_CHATS);
        } else if (k == LV_KEY_UP) {
            lv_obj_scroll_by_bounded(U.msg_list, 0, 36, LV_ANIM_ON);
        } else if (k == LV_KEY_DOWN) {
            lv_obj_scroll_by_bounded(U.msg_list, 0, -36, LV_ANIM_ON);
        }
    }
}

/* ======================= Nodes ======================= */

static void nodes_select(int i)
{
    int n = (int)lv_obj_get_child_count(U.node_list);
    if (!n) return;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    lv_obj_t *old = lv_obj_get_child(U.node_list, U.node_sel);
    if (old) lv_obj_remove_state(old, LV_STATE_CHECKED);
    U.node_sel = i;
    lv_obj_t *row = lv_obj_get_child(U.node_list, i);
    lv_obj_add_state(row, LV_STATE_CHECKED);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

static const char *kind_name(int k)
{
    return k == 2 ? "repeater" : k == 3 ? "room" : k == 4 ? "sensor" : "";
}

static void nodes_rebuild(void)
{
    mesh_node_t **nodes;
    int n = mesh_sorted_nodes(U.m, &nodes);
    char t[48];
    snprintf(t, sizeof t, "%s \xc2\xb7 %d heard", U.m->kind == MESH_MESHCORE ? "Contacts" : "Nodes", n);
    lv_label_set_text(U.node_title, t);
    int keep = U.node_sel;
    lv_obj_clean(U.node_list);
    if (n > U.node_cap) {
        U.node_cap = n + 16;
        U.node_ids = lv_realloc(U.node_ids, sizeof *U.node_ids * U.node_cap);
    }
    U.node_rows = n;
    for (int i = 0; i < n; i++) {
        mesh_node_t *nd = nodes[i];
        lv_obj_t *row = box(U.node_list);
        lv_obj_set_size(row, lv_pct(100), 28);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(ZOS_COLOR_SURFACE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
        snprintf(U.node_ids[i], MESH_KEY_LEN, "%s", nd->id);

        lv_obj_t *name = label(row, zos_font.body, ZOS_COLOR_TEXT, nd->name);
        zos_label_one_line(name, 170);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

        char sub[64];
        const char *kn = kind_name(nd->kind);
        snprintf(sub, sizeof sub, "%s%s%s", nd->short_name[0] ? nd->short_name : nd->id, *kn ? " \xc2\xb7 " : "", kn);
        lv_obj_t *s = label(row, zos_font.small, ZOS_COLOR_MUTED, sub);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        char q[64], a[16];
        ago(nd->last_heard, a, sizeof a);
        if (!isnan(nd->snr) && nd->hops >= 0) snprintf(q, sizeof q, "%.1f dB \xc2\xb7 %s \xc2\xb7 %s", nd->snr, hops_text(nd->hops), a);
        else if (nd->hops >= 0) snprintf(q, sizeof q, "%s \xc2\xb7 %s", hops_text(nd->hops), a);
        else snprintf(q, sizeof q, "%s", a);
        lv_obj_t *ql = label(row, zos_font.small, ZOS_COLOR_MUTED, q);
        lv_obj_align(ql, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (!n) {
        lv_obj_t *e = label(U.node_list, zos_font.small, ZOS_COLOR_MUTED,
                            U.m->has_advert ? "Nobody yet. Press A in Chats to send an advert."
                                            : "No nodes heard yet.");
        lv_obj_set_style_pad_all(e, 8, 0);
    }
    U.node_sel = 0;
    if (n) nodes_select(keep);
}

static void nodes_key(uint32_t k)
{
    if (k == LV_KEY_DOWN || k == LV_KEY_RIGHT) nodes_select(U.node_sel + 1);
    else if (k == LV_KEY_UP || k == LV_KEY_LEFT) nodes_select(U.node_sel - 1);
    else if (k == LV_KEY_ENTER || k == ' ') {
        if (U.node_sel >= U.node_rows) return;
        mesh_node_t *nd = mesh_node_find(U.m, U.node_ids[U.node_sel]);
        if (!nd) return;
        mesh_conv_t *c = mesh_conv_dm(U.m, nd->id, nd->name);
        char key[MESH_KEY_LEN];
        snprintf(key, sizeof key, "%s", c->key);
        open_conv_key(key);
    } else if (k == LV_KEY_ESC || k == 'q' || k == '`' || k == LV_KEY_BACKSPACE || k == 'n' || k == LV_KEY_NEXT)
        set_view(V_CHATS);
}

/* ======================= takeover prompt ======================= */

static void takeover_key(uint32_t k)
{
    if (k == LV_KEY_ENTER || k == ' ' || k == 'y') {
        mesh_activate(U.m->kind);
        set_view(V_CHATS);
    } else if (k == LV_KEY_ESC || k == 'q' || k == '`' || k == 'n') {
        zos_app_exit();
    }
}

/* ======================= plumbing ======================= */

static void sink_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (U.view) {
    case V_CHATS: chats_key(k); break;
    case V_NODES: nodes_key(k); break;
    case V_TAKEOVER: takeover_key(k); break;
    default: break;
    }
}

static void set_view(view_t v)
{
    U.view = v;
    show(U.chats, v == V_CHATS);
    show(U.convv, v == V_CHAT);
    show(U.nodesv, v == V_NODES);
    show(U.takeover, v == V_TAKEOVER);
    if (v == V_CHAT) {
        focus(U.compose);
    } else {
        focus(U.focus_sink);
        if (v == V_CHATS) chats_rebuild();
        if (v == V_NODES) nodes_rebuild();
    }
    U.seen_version = U.m->version;
}

static void refresh_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (U.m->version == U.seen_version) return;
    U.seen_version = U.m->version;
    if (U.view == V_CHATS) chats_rebuild();
    else if (U.view == V_NODES) nodes_rebuild();
    else if (U.view == V_CHAT) conv_refresh(false);
}

static lv_obj_t *page(lv_obj_t *scr)
{
    lv_obj_t *p = box(scr);
    lv_obj_set_size(p, ZOS_HOR_RES, ZOS_VER_RES);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *list(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_size(l, ZOS_HOR_RES, h);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_obj_set_style_pad_row(l, 1, 0);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
    return l;
}

static lv_obj_t *hint(lv_obj_t *parent, const char *text)
{
    lv_obj_t *h = label(parent, &lv_font_montserrat_10, 0x5b6472, text);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -2);
    return h;
}

static void build(lv_obj_t *scr)
{
    /* ---- chats ---- */
    U.chats = page(scr);
    zos_statusbar_create(U.chats, U.m->name);
    lv_obj_t *strip = box(U.chats);
    lv_obj_set_size(strip, ZOS_HOR_RES, 16);
    lv_obj_set_pos(strip, 0, ZOS_STATUSBAR_H);
    U.chat_status_dot = box(strip);
    lv_obj_set_size(U.chat_status_dot, 7, 7);
    lv_obj_set_style_radius(U.chat_status_dot, 4, 0);
    lv_obj_set_style_bg_opa(U.chat_status_dot, LV_OPA_COVER, 0);
    lv_obj_align(U.chat_status_dot, LV_ALIGN_LEFT_MID, 9, 0);
    U.chat_status = label(strip, zos_font.small, ZOS_COLOR_TEXT, "");
    zos_label_one_line(U.chat_status, 180);
    lv_obj_align(U.chat_status, LV_ALIGN_LEFT_MID, 21, 0);
    U.chat_radio = label(strip, zos_font.small, ZOS_COLOR_MUTED, "");
    lv_obj_align(U.chat_radio, LV_ALIGN_RIGHT_MID, -8, 0);
    U.chat_list = list(U.chats, ZOS_STATUSBAR_H + 18, ZOS_VER_RES - ZOS_STATUSBAR_H - 18 - 13);
    U.chat_hint = hint(U.chats, U.m->has_advert
        ? LV_SYMBOL_NEW_LINE " open    N contacts    A advert    Esc back"
        : LV_SYMBOL_NEW_LINE " open    N nodes    Esc back");

    /* ---- chat ---- */
    U.convv = page(scr);
    lv_obj_t *head = box(U.convv);
    lv_obj_set_size(head, ZOS_HOR_RES, ZOS_STATUSBAR_H);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(ZOS_COLOR_SURFACE), 0);
    U.conv_title = label(head, zos_font.title, ZOS_COLOR_TEXT, "");
    lv_obj_set_style_text_font(U.conv_title, zos_font.body, 0);
    zos_label_one_line(U.conv_title, 170);
    lv_obj_align(U.conv_title, LV_ALIGN_LEFT_MID, 7, 0);
    U.conv_sub = label(head, zos_font.small, ZOS_COLOR_MUTED, "");
    lv_obj_align(U.conv_sub, LV_ALIGN_RIGHT_MID, -7, 0);

    U.msg_list = lv_obj_create(U.convv);
    lv_obj_remove_style_all(U.msg_list);
    lv_obj_set_pos(U.msg_list, 0, ZOS_STATUSBAR_H);
    lv_obj_set_size(U.msg_list, ZOS_HOR_RES, ZOS_VER_RES - ZOS_STATUSBAR_H - 27);
    lv_obj_set_style_pad_hor(U.msg_list, 6, 0);
    lv_obj_set_style_pad_ver(U.msg_list, 4, 0);
    lv_obj_set_style_pad_row(U.msg_list, 4, 0);
    lv_obj_set_flex_flow(U.msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(U.msg_list, LV_SCROLLBAR_MODE_OFF);

    U.compose = lv_textarea_create(U.convv);
    lv_textarea_set_one_line(U.compose, true);
    lv_textarea_set_placeholder_text(U.compose, "Type a message, Enter to send");
    lv_obj_set_size(U.compose, ZOS_HOR_RES - 8, 24);
    lv_obj_align(U.compose, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_font(U.compose, zos_font.body, 0);
    lv_obj_set_style_pad_ver(U.compose, 3, 0);
    lv_obj_set_style_pad_hor(U.compose, 8, 0);
    lv_obj_set_style_radius(U.compose, 12, 0);
    lv_obj_set_style_bg_color(U.compose, lv_color_hex(ZOS_COLOR_SURFACE), 0);
    lv_obj_set_style_border_width(U.compose, 1, 0);
    lv_obj_set_style_border_color(U.compose, lv_color_hex(0x323944), 0);
    lv_obj_set_style_border_color(U.compose, lv_color_hex(U.app->color), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(U.compose, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(U.compose, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_text_color(U.compose, lv_color_hex(0x6b7280), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(U.compose, compose_event, LV_EVENT_ALL, NULL);
    U.counter = label(U.convv, zos_font.small, 0xf59e0b, "");
    lv_obj_align(U.counter, LV_ALIGN_BOTTOM_RIGHT, -12, -7);
    show(U.counter, false);

    /* ---- nodes ---- */
    U.nodesv = page(scr);
    lv_obj_t *nb = zos_statusbar_create(U.nodesv, "");
    U.node_title = lv_obj_get_child(nb, 0);
    U.node_list = list(U.nodesv, ZOS_STATUSBAR_H + 2, ZOS_VER_RES - ZOS_STATUSBAR_H - 2 - 13);
    hint(U.nodesv, LV_SYMBOL_NEW_LINE " message    Esc back");

    /* ---- takeover ---- */
    U.takeover = page(scr);
    zos_statusbar_create(U.takeover, U.m->name);
    mesh_t *other = mesh_active();
    char msg[200];
    snprintf(msg, sizeof msg,
             "%s is using the LoRa cap.\n\nSwitch the radio to %s?\n%s stops until you switch back.",
             other ? other->name : "Another app", U.m->name, other ? other->name : "It");
    lv_obj_t *ml = label(U.takeover, zos_font.body, ZOS_COLOR_TEXT, msg);
    lv_obj_set_width(ml, 290);
    lv_label_set_long_mode(ml, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ml, LV_ALIGN_CENTER, 0, -4);
    hint(U.takeover, LV_SYMBOL_NEW_LINE " switch    Esc cancel");

    U.focus_sink = box(scr);
    lv_obj_set_size(U.focus_sink, 1, 1);
    lv_obj_add_event_cb(U.focus_sink, sink_key, LV_EVENT_KEY, NULL);
}

static void open_common(const zos_app_t *app, mesh_kind_t kind, lv_obj_t *scr, lv_group_t *g)
{
    memset(&U, 0, sizeof U);
    U.app = app;
    U.m = mesh_get(kind);
    U.scr = scr;
    U.group = g;
    build(scr);
    U.refresh = lv_timer_create(refresh_cb, 100, NULL);

    mesh_t *act = mesh_active();
    if (act && act != U.m) {
        set_view(V_TAKEOVER);
    } else {
        if (!act) mesh_activate(kind);   /* radio free: just take it */
        set_view(V_CHATS);
    }
}

static void close_common(void)
{
    mesh_set_viewing(NULL, NULL);
    if (U.refresh) lv_timer_delete(U.refresh);
    lv_free(U.status_lbls);
    lv_free(U.row_keys);
    lv_free(U.node_ids);
    memset(&U, 0, sizeof U);
}

extern const zos_app_t zos_app_meshtastic, zos_app_meshcore;

static void mt_open(lv_obj_t *scr, lv_group_t *g) { open_common(&zos_app_meshtastic, MESH_MESHTASTIC, scr, g); }
static void mc_open(lv_obj_t *scr, lv_group_t *g) { open_common(&zos_app_meshcore, MESH_MESHCORE, scr, g); }

static int unread_of(mesh_kind_t k)
{
    mesh_t *m = mesh_get(k);
    int n = 0;
    for (int i = 0; i < m->nconv; i++) n += m->convs[i].unread;
    return n;
}
static int mt_badge(void) { return unread_of(MESH_MESHTASTIC); }
static int mc_badge(void) { return unread_of(MESH_MESHCORE); }

const zos_app_t zos_app_meshtastic = {
    .id = "meshtastic", .name = "Meshtastic", .tagline = "LoRa mesh chat via meshtasticd",
    .icon = &zos_icon_meshtastic, .color = 0x22b573, .open = mt_open, .close = close_common,
    .badge = mt_badge,
};

const zos_app_t zos_app_meshcore = {
    .id = "meshcore", .name = "MeshCore", .tagline = "LoRa mesh chat via a MeshCore companion",
    .icon = &zos_icon_meshcore, .color = 0x3b82f6, .open = mc_open, .close = close_common,
    .badge = mc_badge,
};
