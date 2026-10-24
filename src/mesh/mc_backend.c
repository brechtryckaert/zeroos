/* MeshCore: talks the companion protocol (the one the phone apps use) to the
 * openHop/pyMC repeater's companion on TCP 5000.
 *
 * Framing: '<' + 16-bit LE length + command (app -> radio),
 *          '>' + 16-bit LE length + reply/push (radio -> app).
 * Commands are strictly one at a time; codes >= 0x80 are unsolicited
 * pushes. Layouts follow MeshCore's docs/companion_protocol.md and
 * examples/companion_radio/MyMesh.cpp. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "mesh.h"
#include "stream.h"

enum {
    CMD_APP_START = 1, CMD_SEND_TXT_MSG = 2, CMD_SEND_CHANNEL_TXT_MSG = 3, CMD_GET_CONTACTS = 4,
    CMD_SET_DEVICE_TIME = 6, CMD_SEND_SELF_ADVERT = 7, CMD_SYNC_NEXT_MESSAGE = 10,
    CMD_DEVICE_QUERY = 22, CMD_GET_CHANNEL = 31,
};
enum {
    RESP_OK = 0, RESP_ERR = 1, RESP_CONTACTS_START = 2, RESP_CONTACT = 3, RESP_END_OF_CONTACTS = 4,
    RESP_SELF_INFO = 5, RESP_SENT = 6, RESP_CONTACT_MSG = 7, RESP_CHANNEL_MSG = 8,
    RESP_NO_MORE_MESSAGES = 10, RESP_DEVICE_INFO = 13, RESP_CONTACT_MSG_V3 = 16,
    RESP_CHANNEL_MSG_V3 = 17, RESP_CHANNEL_INFO = 18,
    PUSH_ADVERT = 0x80, PUSH_PATH_UPDATED = 0x81, PUSH_SEND_CONFIRMED = 0x82, PUSH_MSG_WAITING = 0x83,
    PUSH_NEW_ADVERT = 0x8A,
};

#define CONTACT_FRAME_LEN 148
#define MAX_FRAME 300
#define QLEN 48
#define CMD_TIMEOUT_MS 6000

typedef struct {
    uint8_t buf[200];
    uint16_t len;
    uint32_t local_ref;   /* outgoing message this command sends, or 0 */
} cmd_t;

typedef struct {
    zstream_t *s;
    uint8_t rx[MAX_FRAME + 3];
    size_t rxlen;
    cmd_t q[QLEN];
    int qhead, qcount;
    bool inflight;
    uint32_t sent_at;
    int max_channels;
    bool setup_done;
    bool sync_queued, contacts_queued;
    struct { uint32_t ack, local_ref; } acks[16];
    int next_ack;
    uint32_t next_ref;
    lv_timer_t *tick;
    uint32_t last_sync, last_contacts;
} mc_t;

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void hex_id(const uint8_t *pub, char *out)
{
    for (int i = 0; i < 6; i++) sprintf(out + 2 * i, "%02x", pub[i]);
}

/* ---------------- command queue ---------------- */

static void pump(mesh_t *m);

static bool enqueue(mesh_t *m, const uint8_t *cmd, size_t len, uint32_t local_ref)
{
    mc_t *t = m->bs;
    if (t->qcount == QLEN || len > sizeof t->q[0].buf) return false;
    cmd_t *c = &t->q[(t->qhead + t->qcount++) % QLEN];
    memcpy(c->buf, cmd, len);
    c->len = (uint16_t)len;
    c->local_ref = local_ref;
    pump(m);
    return true;
}

static cmd_t *current(mc_t *t) { return t->inflight ? &t->q[t->qhead] : NULL; }

static void finish(mesh_t *m)
{
    mc_t *t = m->bs;
    if (!t->inflight) return;
    uint8_t op = t->q[t->qhead].buf[0];
    if (op == CMD_SYNC_NEXT_MESSAGE) t->sync_queued = false;
    if (op == CMD_GET_CONTACTS) t->contacts_queued = false;
    t->inflight = false;
    t->qhead = (t->qhead + 1) % QLEN;
    t->qcount--;
    pump(m);
}

static void pump(mesh_t *m)
{
    mc_t *t = m->bs;
    if (t->inflight || !t->qcount || !zstream_connected(t->s)) return;
    cmd_t *c = &t->q[t->qhead];
    uint8_t hdr[3] = {'<', (uint8_t)c->len, (uint8_t)(c->len >> 8)};
    if (zstream_write(t->s, hdr, 3) && zstream_write(t->s, c->buf, c->len)) {
        t->inflight = true;
        t->sent_at = lv_tick_get();
    }
}

static void queue_simple(mesh_t *m, uint8_t op)
{
    enqueue(m, &op, 1, 0);
}

static void queue_sync(mesh_t *m)
{
    mc_t *t = m->bs;
    if (t->sync_queued) return;
    t->sync_queued = true;
    t->last_sync = lv_tick_get();
    queue_simple(m, CMD_SYNC_NEXT_MESSAGE);
}

static void queue_contacts(mesh_t *m)
{
    mc_t *t = m->bs;
    if (t->contacts_queued) return;
    t->contacts_queued = true;
    t->last_contacts = lv_tick_get();
    queue_simple(m, CMD_GET_CONTACTS);
}

/* ---------------- parsing ---------------- */

static void on_contact(mesh_t *m, const uint8_t *f, size_t len)
{
    if (len < CONTACT_FRAME_LEN) return;
    char id[16];
    hex_id(f + 1, id);
    mesh_node_t *n = mesh_node_upsert(m, id);
    memcpy(n->pub, f + 1, 32);
    n->kind = f[33];
    uint8_t path_len = f[35];
    n->hops = path_len == 0xFF ? -1 : (path_len & 0x3F);
    snprintf(n->name, sizeof n->name, "%.32s", (const char *)f + 100);
    n->last_heard = le32(f + 132);
    if (!n->name[0]) snprintf(n->name, sizeof n->name, "%s", id);
    char key[MESH_KEY_LEN];
    snprintf(key, sizeof key, "dm:%s", id);
    mesh_conv_t *c = mesh_conv_find(m, key);
    if (c) snprintf(c->title, sizeof c->title, "%s", n->name);
    mesh_changed(m);
}

static void on_self_info(mesh_t *m, const uint8_t *f, size_t len)
{
    if (len < 58) return;
    char id[16];
    hex_id(f + 4, id);
    snprintf(m->self_id, sizeof m->self_id, "%s", id);
    size_t nlen = len - 58 < sizeof m->self_name - 1 ? len - 58 : sizeof m->self_name - 1;
    memcpy(m->self_name, f + 58, nlen);
    m->self_name[nlen] = 0;
    double mhz = le32(f + 48) / 1000.0, bw = le32(f + 52) / 1000.0;
    snprintf(m->radio_desc, sizeof m->radio_desc, "%.3f MHz \xc2\xb7 SF%d \xc2\xb7 BW%g", mhz, f[56], bw);
    mesh_changed(m);
}

static void add_incoming(mesh_t *m, bool channel, int ch_idx, const uint8_t *prefix, uint8_t path_len,
                         uint8_t txt_type, uint32_t ts, const uint8_t *text, size_t tlen, float snr)
{
    if (txt_type != 0 && txt_type != 2) return;   /* CLI replies etc. are not chat */
    char body[200];
    size_t n = tlen < sizeof body - 1 ? tlen : sizeof body - 1;
    memcpy(body, text, n);
    body[n] = 0;
    int hops = path_len == 0xFF ? -1 : (path_len & 0x3F);
    mesh_msg_t msg = {.ts = ts, .status = MSG_RECEIVED, .snr = snr, .hops = (int8_t)hops, .text = body};
    mesh_conv_t *c;
    if (channel) {
        char key[16];
        snprintf(key, sizeof key, "ch:%d", ch_idx);
        c = mesh_conv_find(m, key);
        if (!c) c = mesh_conv_channel(m, ch_idx, NULL);
        /* Channel messages carry the sender in the text: "Name: message". */
        char *sep = strstr(body, ": ");
        if (sep && sep - body < 32) {
            *sep = 0;
            msg.from = body;
            msg.text = sep + 2;
        } else {
            msg.from = "?";
        }
    } else {
        char id[16];
        hex_id(prefix, id);
        mesh_node_t *nd = mesh_node_find(m, id);
        const char *name = nd && nd->name[0] ? nd->name : id;
        c = mesh_conv_dm(m, id, name);
        msg.from = (char *)name;
        if (nd && hops >= 0) nd->hops = hops;
        if (nd) {
            nd->last_heard = (uint32_t)time(NULL);
            if (!isnan(snr)) nd->snr = snr;
        }
    }
    mesh_add_msg(m, c, &msg);
}

static float snr_of(uint8_t b) { return (float)(int8_t)b / 4.0f; }

static void handle_frame(mesh_t *m, const uint8_t *f, size_t len)
{
    mc_t *t = m->bs;
    if (!len) return;
    uint8_t code = f[0];

    /* ---- pushes ---- */
    if (code >= 0x80) {
        switch (code) {
        case PUSH_MSG_WAITING: queue_sync(m); break;
        case PUSH_NEW_ADVERT: on_contact(m, f, len); break;
        case PUSH_ADVERT:
        case PUSH_PATH_UPDATED:
            queue_contacts(m);   /* cheap: refresh names, hops, last heard */
            break;
        case PUSH_SEND_CONFIRMED:
            if (len >= 5) {
                uint32_t ack = le32(f + 1);
                for (int i = 0; i < 16; i++)
                    if (t->acks[i].ack == ack && t->acks[i].local_ref) {
                        mesh_msg_t *msg = mesh_find_out_msg(m, t->acks[i].local_ref, NULL);
                        if (msg) mesh_msg_status(m, msg, MSG_DELIVERED);
                        t->acks[i].ack = 0;
                    }
            }
            break;
        }
        return;
    }

    /* ---- replies to the in-flight command ---- */
    cmd_t *c = current(t);
    uint8_t op = c ? c->buf[0] : 0;
    switch (code) {
    case RESP_DEVICE_INFO:
        t->max_channels = len > 3 && f[1] >= 3 ? f[3] : 8;
        if (t->max_channels > 8) t->max_channels = 8;   /* the UI lists up to 8 */
        finish(m);
        return;
    case RESP_SELF_INFO:
        on_self_info(m, f, len);
        finish(m);
        return;
    case RESP_CONTACTS_START:
        return;   /* more frames follow; stay in flight */
    case RESP_CONTACT:
        on_contact(m, f, len);
        return;
    case RESP_END_OF_CONTACTS: {
        int nodes = 0;
        for (int i = 0; i < m->nnode; i++) nodes += !m->nodes[i].is_self;
        if (m->state == MESH_READY || t->setup_done)
            mesh_set_state(m, MESH_READY, "Connected \xc2\xb7 %d contact%s", nodes, nodes == 1 ? "" : "s");
        finish(m);
        return;
    }
    case RESP_CHANNEL_INFO:
        if (len >= 34) {
            char name[33];
            snprintf(name, sizeof name, "%.32s", (const char *)f + 2);
            if (name[0]) mesh_conv_channel(m, f[1], name);
        }
        finish(m);
        return;
    case RESP_CONTACT_MSG_V3:
        if (len >= 16) {
            size_t off = f[11] == 2 ? 20 : 16;
            if (len >= off) add_incoming(m, false, 0, f + 4, f[10], f[11], le32(f + 12), f + off, len - off, snr_of(f[1]));
        }
        finish(m);
        queue_sync(m);
        return;
    case RESP_CONTACT_MSG:
        if (len >= 13) {
            size_t off = f[8] == 2 ? 17 : 13;
            if (len >= off) add_incoming(m, false, 0, f + 1, f[7], f[8], le32(f + 9), f + off, len - off, NAN);
        }
        finish(m);
        queue_sync(m);
        return;
    case RESP_CHANNEL_MSG_V3:
        if (len >= 11) add_incoming(m, true, f[4], NULL, f[5], f[6], le32(f + 7), f + 11, len - 11, snr_of(f[1]));
        finish(m);
        queue_sync(m);
        return;
    case RESP_CHANNEL_MSG:
        if (len >= 8) add_incoming(m, true, f[1], NULL, f[2], f[3], le32(f + 4), f + 8, len - 8, NAN);
        finish(m);
        queue_sync(m);
        return;
    case RESP_NO_MORE_MESSAGES:
        finish(m);
        return;
    case RESP_SENT:
    case RESP_OK:
    case RESP_ERR: {
        uint32_t ref = c ? c->local_ref : 0;
        mesh_msg_t *msg = ref ? mesh_find_out_msg(m, ref, NULL) : NULL;
        if (msg) {
            if (code == RESP_ERR) {
                mesh_msg_status(m, msg, MSG_FAILED);
            } else {
                mesh_msg_status(m, msg, MSG_SENT);
                if (code == RESP_SENT && len >= 6 && op == CMD_SEND_TXT_MSG) {
                    t->acks[t->next_ack].ack = le32(f + 2);
                    t->acks[t->next_ack].local_ref = ref;
                    t->next_ack = (t->next_ack + 1) % 16;
                }
            }
        }
        if (op == CMD_GET_CHANNEL && code == RESP_ERR) { /* slot doesn't exist */ }
        finish(m);
        return;
    }
    default:
        finish(m);   /* a reply we don't use (e.g. CURR_TIME) still ends the command */
        return;
    }
}

static void on_data(void *user, const uint8_t *data, size_t len)
{
    mesh_t *m = user;
    mc_t *t = m->bs;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (t->rxlen == 0 && c != '>') continue;   /* resync on frame start */
        t->rx[t->rxlen++] = c;
        if (t->rxlen >= 3) {
            size_t n = (size_t)t->rx[1] | (size_t)t->rx[2] << 8;
            if (n > MAX_FRAME) {
                t->rxlen = 0;
                continue;
            }
            if (t->rxlen == 3 + n) {
                t->rxlen = 0;
                handle_frame(m, t->rx + 3, n);
                if (m->bs != t) return;
            }
        }
    }
}

/* ---------------- lifecycle ---------------- */

static void start_session(mesh_t *m)
{
    mc_t *t = m->bs;
    t->qhead = t->qcount = 0;
    t->inflight = t->sync_queued = t->contacts_queued = false;
    t->max_channels = 8;

    uint8_t q[2] = {CMD_DEVICE_QUERY, 3};   /* app protocol v3: SNR in message frames */
    enqueue(m, q, 2, 0);
    uint8_t start[14] = {CMD_APP_START, 0, 0, 0, 0, 0, 0, 0, 'Z', 'e', 'r', 'o', 'O', 'S'};
    enqueue(m, start, sizeof start, 0);
    uint8_t tm[5] = {CMD_SET_DEVICE_TIME};
    put32(tm + 1, (uint32_t)time(NULL));
    enqueue(m, tm, 5, 0);
    queue_contacts(m);
    /* channel slots are queried once DEVICE_INFO has told us how many exist;
     * tick() notices setup_done == false and queues them */
}

static void tick(lv_timer_t *timer)
{
    mesh_t *m = lv_timer_get_user_data(timer);
    mc_t *t = m->bs;
    uint32_t now = lv_tick_get();

    if (m->state == MESH_CONNECTING && zstream_error(t->s)[0]) {
        char s[96];
        snprintf(s, sizeof s, "Waiting for the MeshCore companion on %s (%s)", zstream_addr(t->s),
                 zstream_error(t->s));
        if (strcmp(s, m->status)) mesh_set_state(m, MESH_CONNECTING, "%s", s);
    }
    if (!zstream_connected(t->s)) return;

    if (t->inflight && now - t->sent_at > CMD_TIMEOUT_MS) {
        cmd_t *c = current(t);
        mesh_msg_t *msg = c->local_ref ? mesh_find_out_msg(m, c->local_ref, NULL) : NULL;
        if (msg) mesh_msg_status(m, msg, MSG_FAILED);
        finish(m);
    }
    /* After the first three setup commands have been answered, list channels. */
    if (!t->setup_done && !t->inflight && t->qcount <= 1) {
        t->setup_done = true;
        for (int i = 0; i < t->max_channels; i++) {
            uint8_t g[2] = {CMD_GET_CHANNEL, (uint8_t)i};
            enqueue(m, g, 2, 0);
        }
        queue_sync(m);
    }
    if (t->setup_done && !t->qcount && m->state != MESH_READY) {
        int nodes = 0;
        for (int i = 0; i < m->nnode; i++) nodes += !m->nodes[i].is_self;
        mesh_set_state(m, MESH_READY, "Connected \xc2\xb7 %d contact%s", nodes, nodes == 1 ? "" : "s");
    }
    /* The push 'tickle' can be missed; poll now and then as a safety net. */
    if (t->setup_done && now - t->last_sync > 30000) queue_sync(m);
    if (t->setup_done && now - t->last_contacts > 300000) queue_contacts(m);
}

static void on_state(void *user, bool up)
{
    mesh_t *m = user;
    mc_t *t = m->bs;
    t->rxlen = 0;
    t->setup_done = false;
    if (up) {
        mesh_set_state(m, MESH_SYNCING, "Connected, syncing contacts…");
        start_session(m);
    } else {
        mesh_set_state(m, MESH_CONNECTING, "Lost the companion, reconnecting…");
        t->qcount = 0;
        t->inflight = false;
        for (int i = 0; i < m->nconv; i++)
            for (int j = 0; j < m->convs[i].n; j++)
                if (m->convs[i].msgs[j].status == MSG_SENDING) mesh_msg_status(m, &m->convs[i].msgs[j], MSG_FAILED);
    }
}

static bool mc_send(mesh_t *m, mesh_conv_t *c, const char *text)
{
    mc_t *t = m->bs;
    size_t tl = strlen(text);
    if (!t || !zstream_connected(t->s) || m->state != MESH_READY || !tl || tl > 133) return false;
    uint8_t buf[200];
    size_t n;
    uint32_t ts = (uint32_t)time(NULL);
    if (c->is_channel) {
        buf[0] = CMD_SEND_CHANNEL_TXT_MSG;
        buf[1] = 0;   /* plain text */
        buf[2] = (uint8_t)c->ch_idx;
        put32(buf + 3, ts);
        memcpy(buf + 7, text, tl);
        n = 7 + tl;
    } else {
        mesh_node_t *nd = mesh_node_find(m, c->peer);
        if (!nd) return false;
        buf[0] = CMD_SEND_TXT_MSG;
        buf[1] = 0;   /* plain text */
        buf[2] = 0;   /* attempt */
        put32(buf + 3, ts);
        memcpy(buf + 7, nd->pub, 6);
        memcpy(buf + 13, text, tl);
        n = 13 + tl;
    }
    uint32_t ref = ++t->next_ref;
    mesh_msg_t msg = {.ts = ts, .text = (char *)text, .outgoing = true, .status = MSG_SENDING, .ref = ref,
                      .snr = NAN, .hops = -1};
    mesh_add_msg(m, c, &msg);
    if (!enqueue(m, buf, n, ref)) {
        mesh_msg_t *mm = mesh_find_out_msg(m, ref, NULL);
        if (mm) mesh_msg_status(m, mm, MSG_FAILED);
    }
    return true;
}

static void mc_advert(mesh_t *m)
{
    mc_t *t = m->bs;
    if (!t || !zstream_connected(t->s)) return;
    uint8_t a[2] = {CMD_SEND_SELF_ADVERT, 1};   /* 1 = flood */
    enqueue(m, a, 2, 0);
}

static void mc_start(mesh_t *m)
{
    mc_t *t = lv_zalloc(sizeof *t);
    m->bs = t;
    /* local refs only need to be unique within the history log */
    t->next_ref = (uint32_t)time(NULL) << 8;
    t->s = zstream_new(m->host, m->port, (zstream_cbs_t){on_data, on_state}, m);
    t->tick = lv_timer_create(tick, 100, m);
}

static void mc_stop(mesh_t *m)
{
    mc_t *t = m->bs;
    if (!t) return;
    zstream_free(t->s);
    lv_timer_delete(t->tick);
    lv_free(t);
    m->bs = NULL;
    m->nnode = 0;
    m->radio_desc[0] = 0;
}

const mesh_backend_t mesh_backend_meshcore = {
    .start = mc_start,
    .stop = mc_stop,
    .send = mc_send,
    .advert = mc_advert,
    .max_text = 133,
};
