/* Meshtastic: talks to meshtasticd's client API on TCP 4403.
 *
 * Framing: 0x94 0xC3, 16-bit big-endian length, then a ToRadio / FromRadio
 * protobuf. Field numbers below come from meshtastic/protobufs (mesh.proto,
 * channel.proto, config.proto); only what the chat UI needs is decoded. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "mesh.h"
#include "pb.h"
#include "stream.h"

/* ToRadio */
#define TR_PACKET 1
#define TR_WANT_CONFIG_ID 3
#define TR_HEARTBEAT 7
/* FromRadio */
#define FR_PACKET 2
#define FR_MY_INFO 3
#define FR_NODE_INFO 4
#define FR_CONFIG 5
#define FR_CONFIG_COMPLETE_ID 7
#define FR_REBOOTED 8
#define FR_CHANNEL 10
/* MeshPacket */
#define MP_FROM 1
#define MP_TO 2
#define MP_CHANNEL 3
#define MP_DECODED 4
#define MP_ID 6
#define MP_RX_TIME 7
#define MP_RX_SNR 8
#define MP_HOP_LIMIT 9
#define MP_WANT_ACK 10
#define MP_HOP_START 15
/* Data */
#define DATA_PORTNUM 1
#define DATA_PAYLOAD 2
#define DATA_REQUEST_ID 6
/* NodeInfo */
#define NI_NUM 1
#define NI_USER 2
#define NI_SNR 4
#define NI_LAST_HEARD 5
#define NI_HOPS_AWAY 9
/* User */
#define USER_LONG_NAME 2
#define USER_SHORT_NAME 3
/* MyNodeInfo */
#define MYINFO_NODE_NUM 1
/* Channel / ChannelSettings */
#define CH_INDEX 1
#define CH_SETTINGS 2
#define CH_ROLE 3
#define CHS_NAME 3
/* Config / LoRaConfig */
#define CFG_LORA 6
#define LORA_USE_PRESET 1
#define LORA_MODEM_PRESET 2
#define LORA_REGION 7
/* Routing */
#define ROUTING_ERROR 3
#define ROUTING_MAX_RETRANSMIT 5   /* Routing.Error */
/* PortNum */
#define PORT_TEXT 1
#define PORT_NODEINFO 4
#define PORT_ROUTING 5

#define BROADCAST 0xFFFFFFFFu
#define MAX_FRAME 512

typedef struct {
    zstream_t *s;
    uint8_t rx[MAX_FRAME + 4];
    size_t rxlen;
    uint32_t my_num;
    uint32_t config_nonce;
    int preset, region;
    bool use_preset;
    lv_timer_t *hb, *status;
    uint32_t next_id;
    bool ch0_unnamed;    /* primary channel shown under the preset's name */
} mt_t;

static const char *PRESETS[] = {"LongFast", "LongSlow", "VeryLongSlow", "MediumSlow", "MediumFast",
                                "ShortSlow", "ShortFast", "LongModerate", "ShortTurbo", "LongTurbo",
                                "LiteFast", "LiteSlow", "NarrowFast", "NarrowSlow"};
static const char *REGIONS[] = {"UNSET", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW",
                                "RU", "IN", "NZ_865", "TH", "LORA_24", "UA_433", "UA_868"};

static void node_id(uint32_t num, char *out, size_t cap) { snprintf(out, cap, "!%08x", num); }

static const char *preset_name(int p)
{
    return p >= 0 && p < (int)(sizeof PRESETS / sizeof *PRESETS) ? PRESETS[p] : "Custom";
}

/* ---------------- sending ---------------- */

static bool send_toradio(mesh_t *m, const uint8_t *pb, size_t len)
{
    mt_t *t = m->bs;
    uint8_t hdr[4] = {0x94, 0xC3, (uint8_t)(len >> 8), (uint8_t)len};
    return zstream_write(t->s, hdr, 4) && zstream_write(t->s, pb, len);
}

static void want_config(mesh_t *m)
{
    mt_t *t = m->bs;
    uint8_t buf[16];
    pb_writer_t w = pb_writer(buf, sizeof buf);
    t->config_nonce = (uint32_t)(lv_rand(1, 0x7fffffff));
    pb_put_varint(&w, TR_WANT_CONFIG_ID, t->config_nonce);
    send_toradio(m, buf, w.len);
}

static void heartbeat(lv_timer_t *timer)
{
    mesh_t *m = lv_timer_get_user_data(timer);
    mt_t *t = m->bs;
    if (!zstream_connected(t->s)) return;
    uint8_t buf[16];
    pb_writer_t w = pb_writer(buf, sizeof buf);
    size_t hb = pb_begin(&w, TR_HEARTBEAT);
    pb_put_varint(&w, 1, lv_rand(1, 0xffff));
    pb_end(&w, hb);
    send_toradio(m, buf, w.len);
}

static bool mt_send(mesh_t *m, mesh_conv_t *c, const char *text)
{
    mt_t *t = m->bs;
    size_t tl = strlen(text);
    if (!t || !zstream_connected(t->s) || m->state != MESH_READY || tl == 0 || tl > 200) return false;

    uint32_t to = BROADCAST;
    if (!c->is_channel) {
        if (c->peer[0] != '!') return false;
        to = (uint32_t)strtoul(c->peer + 1, NULL, 16);
    }
    uint32_t id = t->next_id++;
    uint8_t buf[MAX_FRAME];
    pb_writer_t w = pb_writer(buf, sizeof buf);
    size_t pkt = pb_begin(&w, TR_PACKET);
    pb_put_fixed32(&w, MP_TO, to);
    if (c->is_channel && c->ch_idx) pb_put_varint(&w, MP_CHANNEL, (uint32_t)c->ch_idx);
    size_t data = pb_begin(&w, MP_DECODED);
    pb_put_varint(&w, DATA_PORTNUM, PORT_TEXT);
    pb_put_bytes(&w, DATA_PAYLOAD, text, tl);
    pb_end(&w, data);
    pb_put_fixed32(&w, MP_ID, id);
    pb_put_varint(&w, MP_WANT_ACK, 1);
    pb_end(&w, pkt);
    if (w.overflow || !send_toradio(m, buf, w.len)) return false;

    mesh_msg_t msg = {.text = (char *)text, .outgoing = true, .status = MSG_SENDING, .ref = id,
                      .snr = NAN, .hops = -1};
    mesh_add_msg(m, c, &msg);
    return true;
}

/* ---------------- receiving ---------------- */

static const char *name_for(mesh_t *m, uint32_t num, char *buf, size_t cap)
{
    char id[16];
    node_id(num, id, sizeof id);
    mesh_node_t *n = mesh_node_find(m, id);
    if (n && n->name[0]) return n->name;
    snprintf(buf, cap, "%s", id);
    return buf;
}

static void parse_user(mesh_t *m, mesh_node_t *n, pb_reader_t r)
{
    pb_field_t f;
    while (pb_next(&r, &f)) {
        if (f.field == USER_LONG_NAME && f.wire == PB_LEN) pb_str(&f, n->name, sizeof n->name);
        else if (f.field == USER_SHORT_NAME && f.wire == PB_LEN) pb_str(&f, n->short_name, sizeof n->short_name);
    }
    /* keep DM titles in step with renamed nodes */
    char key[MESH_KEY_LEN + 4];
    snprintf(key, sizeof key, "dm:%s", n->id);
    mesh_conv_t *c = mesh_conv_find(m, key);
    if (c && n->name[0]) snprintf(c->title, sizeof c->title, "%s", n->name);
    mesh_changed(m);
}

static void on_node_info(mesh_t *m, pb_reader_t r)
{
    mt_t *t = m->bs;
    pb_field_t f;
    uint32_t num = 0;
    pb_field_t user = {0};
    float snr = NAN;
    uint32_t last = 0;
    int hops = -1;
    while (pb_next(&r, &f)) {
        switch (f.field) {
        case NI_NUM: num = (uint32_t)f.u; break;
        case NI_USER: user = f; break;
        case NI_SNR: snr = pb_float(&f); break;
        case NI_LAST_HEARD: last = (uint32_t)f.u; break;
        case NI_HOPS_AWAY: hops = (int)f.u; break;
        }
    }
    if (!num) return;
    char id[16];
    node_id(num, id, sizeof id);
    mesh_node_t *n = mesh_node_upsert(m, id);
    n->num = num;
    n->is_self = num == t->my_num;
    if (user.data) parse_user(m, n, pb_sub(&user));
    if (!n->name[0]) snprintf(n->name, sizeof n->name, "%s", id);
    if (snr != 0.0f || !n->is_self) n->snr = snr;
    n->last_heard = last;
    n->hops = hops;
    if (n->is_self) {
        snprintf(m->self_name, sizeof m->self_name, "%s", n->name);
        snprintf(m->self_id, sizeof m->self_id, "%s", id);
    }
    mesh_changed(m);
}

static void on_channel(mesh_t *m, pb_reader_t r)
{
    mt_t *t = m->bs;
    pb_field_t f;
    int idx = 0, role = 0;
    char name[32] = "";
    while (pb_next(&r, &f)) {
        if (f.field == CH_INDEX) idx = (int)f.u;
        else if (f.field == CH_ROLE) role = (int)f.u;
        else if (f.field == CH_SETTINGS && f.wire == PB_LEN) {
            pb_reader_t s = pb_sub(&f);
            pb_field_t g;
            while (pb_next(&s, &g))
                if (g.field == CHS_NAME && g.wire == PB_LEN) pb_str(&g, name, sizeof name);
        }
    }
    if (role == 0) return;   /* DISABLED */
    /* An unnamed primary channel is shown under its modem preset's name, as the
     * phone apps do. meshtasticd sends channels before config, so on_config
     * renames it once the preset is known. */
    if (idx == 0) t->ch0_unnamed = !name[0];
    mesh_conv_channel(m, idx, name[0] ? name : (idx == 0 ? preset_name(t->preset) : NULL));
}

static void on_config(mesh_t *m, pb_reader_t r)
{
    mt_t *t = m->bs;
    pb_field_t f;
    while (pb_next(&r, &f)) {
        if (f.field != CFG_LORA || f.wire != PB_LEN) continue;
        pb_reader_t l = pb_sub(&f);
        pb_field_t g;
        t->preset = 0;
        t->region = 0;
        t->use_preset = false;
        while (pb_next(&l, &g)) {
            if (g.field == LORA_USE_PRESET) t->use_preset = g.u != 0;
            else if (g.field == LORA_MODEM_PRESET) t->preset = (int)g.u;
            else if (g.field == LORA_REGION) t->region = (int)g.u;
        }
        const char *reg = t->region < (int)(sizeof REGIONS / sizeof *REGIONS) ? REGIONS[t->region] : "region?";
        if (t->region == 0)   /* UNSET: meshtasticd won't transmit until a region is chosen */
            snprintf(m->radio_desc, sizeof m->radio_desc, "No region set!");
        else
            snprintf(m->radio_desc, sizeof m->radio_desc, "%s \xc2\xb7 %s", reg,
                     t->use_preset ? preset_name(t->preset) : "custom");
        mesh_conv_t *ch0 = mesh_conv_find(m, "ch:0");
        if (ch0 && t->ch0_unnamed)
            snprintf(ch0->title, sizeof ch0->title, "%s", t->use_preset ? preset_name(t->preset) : "Primary");
        mesh_changed(m);
    }
}

static void on_packet(mesh_t *m, pb_reader_t r)
{
    mt_t *t = m->bs;
    pb_field_t f;
    uint32_t from = 0, to = 0, channel = 0, rx_time = 0, hop_limit = 0, hop_start = 0;
    float snr = NAN;
    pb_field_t data = {0};
    while (pb_next(&r, &f)) {
        switch (f.field) {
        case MP_FROM: from = (uint32_t)f.u; break;
        case MP_TO: to = (uint32_t)f.u; break;
        case MP_CHANNEL: channel = (uint32_t)f.u; break;
        case MP_DECODED: data = f; break;
        case MP_RX_TIME: rx_time = (uint32_t)f.u; break;
        case MP_RX_SNR: snr = pb_float(&f); break;
        case MP_HOP_LIMIT: hop_limit = (uint32_t)f.u; break;
        case MP_HOP_START: hop_start = (uint32_t)f.u; break;
        }
    }
    if (!data.data) return;   /* still encrypted: not for a channel we have */
    uint32_t port = 0, request_id = 0;
    pb_field_t payload = {0};
    pb_reader_t d = pb_sub(&data);
    while (pb_next(&d, &f)) {
        if (f.field == DATA_PORTNUM) port = (uint32_t)f.u;
        else if (f.field == DATA_PAYLOAD) payload = f;
        else if (f.field == DATA_REQUEST_ID) request_id = (uint32_t)f.u;
    }

    if (port == PORT_ROUTING && request_id) {
        int err = 0;
        pb_reader_t rr = pb_sub(&payload);
        while (pb_next(&rr, &f))
            if (f.field == ROUTING_ERROR) err = (int)f.u;
        mesh_conv_t *conv = NULL;
        mesh_msg_t *msg = mesh_find_out_msg(m, request_id, &conv);
        if (msg) {
            /* An ack from ourselves is the "implicit ack": someone rebroadcast it.
             * On a channel, MAX_RETRANSMIT only means nobody relayed it within
             * earshot; it was still transmitted, so that isn't a failure. */
            msg_status_t st;
            if (!err) st = from == t->my_num ? MSG_SENT : MSG_DELIVERED;
            else if (err == ROUTING_MAX_RETRANSMIT && conv && conv->is_channel) st = MSG_SENT;
            else st = MSG_FAILED;
            mesh_msg_status(m, msg, st);
        }
        return;
    }

    char id[16];
    node_id(from, id, sizeof id);
    if (port == PORT_NODEINFO && payload.data) {
        mesh_node_t *n = mesh_node_upsert(m, id);
        n->num = from;
        parse_user(m, n, pb_sub(&payload));
        return;
    }
    if (port != PORT_TEXT || !payload.data) return;

    mesh_node_t *n = mesh_node_upsert(m, id);
    n->num = from;
    n->last_heard = rx_time ? rx_time : (uint32_t)time(NULL);
    if (!isnan(snr) && snr != 0.0f) n->snr = snr;
    int hops = hop_start >= hop_limit && hop_start ? (int)(hop_start - hop_limit) : -1;
    if (hops >= 0) n->hops = hops;

    char text[256], namebuf[MESH_KEY_LEN];
    pb_str(&payload, text, sizeof text);
    const char *from_name = name_for(m, from, namebuf, sizeof namebuf);
    mesh_conv_t *c;
    if (to == BROADCAST) {
        char key[16];
        snprintf(key, sizeof key, "ch:%u", channel);
        c = mesh_conv_find(m, key);
        if (!c) c = mesh_conv_channel(m, (int)channel, NULL);
    } else {
        c = mesh_conv_dm(m, id, from_name);
    }
    mesh_msg_t msg = {.ts = rx_time, .from = (char *)from_name, .text = text, .status = MSG_RECEIVED,
                      .snr = (snr == 0.0f ? NAN : snr), .hops = (int8_t)hops};
    mesh_add_msg(m, c, &msg);
}

static void on_fromradio(mesh_t *m, const uint8_t *p, size_t len)
{
    mt_t *t = m->bs;
    pb_reader_t r = pb_reader(p, len);
    pb_field_t f;
    while (pb_next(&r, &f)) {
        switch (f.field) {
        case FR_MY_INFO: {
            pb_reader_t s = pb_sub(&f);
            pb_field_t g;
            while (pb_next(&s, &g))
                if (g.field == MYINFO_NODE_NUM) t->my_num = (uint32_t)g.u;
            break;
        }
        case FR_NODE_INFO: on_node_info(m, pb_sub(&f)); break;
        case FR_CHANNEL: on_channel(m, pb_sub(&f)); break;
        case FR_CONFIG: on_config(m, pb_sub(&f)); break;
        case FR_PACKET: on_packet(m, pb_sub(&f)); break;
        case FR_CONFIG_COMPLETE_ID:
            if ((uint32_t)f.u == t->config_nonce) {
                int nodes = 0;
                for (int i = 0; i < m->nnode; i++) nodes += !m->nodes[i].is_self;
                mesh_set_state(m, MESH_READY, "Connected \xc2\xb7 %d node%s", nodes, nodes == 1 ? "" : "s");
            }
            break;
        case FR_REBOOTED:
            mesh_set_state(m, MESH_SYNCING, "Node restarted, syncing…");
            want_config(m);
            break;
        }
    }
}

/* Reassemble 0x94 0xC3 frames from the byte stream. Bytes outside frames
 * are debug log text from older daemons and are skipped. */
static void on_data(void *user, const uint8_t *data, size_t len)
{
    mesh_t *m = user;
    mt_t *t = m->bs;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (t->rxlen == 0) {
            if (c == 0x94) t->rx[t->rxlen++] = c;
            continue;
        }
        if (t->rxlen == 1) {
            if (c == 0xC3) t->rx[t->rxlen++] = c;
            else t->rxlen = c == 0x94 ? 1 : 0;
            continue;
        }
        t->rx[t->rxlen++] = c;
        if (t->rxlen >= 4) {
            size_t n = (size_t)t->rx[2] << 8 | t->rx[3];
            if (n > MAX_FRAME) {   /* corrupt: resync */
                t->rxlen = 0;
                continue;
            }
            if (t->rxlen == 4 + n) {
                on_fromradio(m, t->rx + 4, n);
                t->rxlen = 0;
                if (m->bs != t) return;   /* stopped from inside a handler */
            }
        }
    }
}

static void on_state(void *user, bool up)
{
    mesh_t *m = user;
    mt_t *t = m->bs;
    t->rxlen = 0;
    if (up) {
        mesh_set_state(m, MESH_SYNCING, "Connected, syncing nodes…");
        want_config(m);
    } else {
        mesh_set_state(m, MESH_CONNECTING, "Lost meshtasticd, reconnecting…");
        for (int i = 0; i < m->nconv; i++)
            for (int j = 0; j < m->convs[i].n; j++)
                if (m->convs[i].msgs[j].status == MSG_SENDING) mesh_msg_status(m, &m->convs[i].msgs[j], MSG_FAILED);
    }
}

static void status_tick(lv_timer_t *timer)
{
    mesh_t *m = lv_timer_get_user_data(timer);
    mt_t *t = m->bs;
    if (m->state == MESH_CONNECTING && zstream_error(t->s)[0]) {
        char s[96];
        snprintf(s, sizeof s, "Waiting for meshtasticd on %s (%s)", zstream_addr(t->s), zstream_error(t->s));
        if (strcmp(s, m->status)) mesh_set_state(m, MESH_CONNECTING, "%s", s);
    }
}

static void mt_start(mesh_t *m)
{
    mt_t *t = lv_zalloc(sizeof *t);
    m->bs = t;
    t->next_id = (uint32_t)lv_rand(1, 0x7fffffff);
    t->s = zstream_new(m->host, m->port, (zstream_cbs_t){on_data, on_state}, m);
    t->hb = lv_timer_create(heartbeat, 60000, m);
    t->status = lv_timer_create(status_tick, 500, m);
}

static void mt_stop(mesh_t *m)
{
    mt_t *t = m->bs;
    if (!t) return;
    zstream_free(t->s);
    lv_timer_delete(t->hb);
    lv_timer_delete(t->status);
    lv_free(t);
    m->bs = NULL;
    m->nnode = 0;
    m->radio_desc[0] = 0;
}

const mesh_backend_t mesh_backend_meshtastic = {
    .start = mt_start,
    .stop = mt_stop,
    .send = mt_send,
    .advert = NULL,
    .max_text = 200,
};
