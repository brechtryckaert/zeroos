#include <math.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include "../zos.h"
#include "mesh.h"

extern char **environ;

#define MAX_MSGS_PER_CONV 200

void (*mesh_notify_hook)(mesh_t *m, mesh_conv_t *c, const mesh_msg_t *msg);

static mesh_t meshes[MESH_COUNT];
static mesh_t *active;
static bool radio_ctl;
static mesh_t *viewing_mesh;
static char viewing_key[MESH_KEY_LEN];
static bool loading;   /* replaying history: no notifications, no re-logging */

/* ======================= persistence ======================= */

static void log_path(mesh_t *m, char *out, size_t cap)
{
    snprintf(out, cap, "%s/mesh-%s.log", zos_cfg.state_dir,
             m->kind == MESH_MESHTASTIC ? "meshtastic" : "meshcore");
}

static void put_escaped(FILE *f, const char *s)
{
    for (; s && *s; s++) {
        if (*s == '\t') fputs("\\t", f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\\') fputs("\\\\", f);
        else fputc(*s, f);
    }
}

static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            *w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static void log_msg(mesh_t *m, mesh_conv_t *c, const mesh_msg_t *msg)
{
    if (loading) return;
    char p[512];
    log_path(m, p, sizeof p);
    FILE *f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "M\t%s\t", c->key);
    put_escaped(f, c->title);
    fprintf(f, "\t%u\t%d\t%d\t%u\t", msg->ts, msg->outgoing, msg->status, msg->ref);
    put_escaped(f, msg->from);
    fputc('\t', f);
    put_escaped(f, msg->text);
    fputc('\n', f);
    fclose(f);
}

static void log_status(mesh_t *m, mesh_conv_t *c, const mesh_msg_t *msg)
{
    if (loading) return;
    char p[512];
    log_path(m, p, sizeof p);
    FILE *f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "S\t%s\t%u\t%d\n", c->key, msg->ref, msg->status);
    fclose(f);
}

static void load_history(mesh_t *m)
{
    char p[512];
    log_path(m, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    loading = true;
    char *line = NULL;
    size_t cap = 0;
    long nlines = 0;
    while (getline(&line, &cap, f) > 0) {
        nlines++;
        line[strcspn(line, "\n")] = 0;
        char *fld[9] = {0};
        int nf = 0;
        for (char *s = line, *tab; nf < 9; s = tab + 1) {
            fld[nf++] = s;
            tab = strchr(s, '\t');
            if (!tab) break;
            *tab = 0;
        }
        if (fld[0][0] == 'M' && nf == 9) {
            for (int i = 1; i < 9; i++) unescape(fld[i]);
            mesh_conv_t *c = mesh_conv_find(m, fld[1]);
            if (!c) {
                if (strncmp(fld[1], "ch:", 3) == 0)
                    c = mesh_conv_channel(m, atoi(fld[1] + 3), fld[2]), c->known = false;
                else
                    c = mesh_conv_dm(m, fld[1] + 3, fld[2]);
            }
            mesh_msg_t t = {.ts = (uint32_t)strtoul(fld[3], NULL, 10), .outgoing = atoi(fld[4]),
                            .status = (uint8_t)atoi(fld[5]), .ref = (uint32_t)strtoul(fld[6], NULL, 10),
                            .from = fld[7][0] ? fld[7] : NULL, .text = fld[8], .snr = NAN, .hops = -1};
            if (t.status == MSG_SENDING) t.status = MSG_SENT;   /* outcome unknown after restart */
            mesh_add_msg(m, c, &t);
        } else if (fld[0][0] == 'S' && nf == 4) {
            mesh_conv_t *c = mesh_conv_find(m, fld[1]);
            uint32_t ref = (uint32_t)strtoul(fld[2], NULL, 10);
            for (int i = 0; c && i < c->n; i++)
                if (c->msgs[i].outgoing && c->msgs[i].ref == ref) c->msgs[i].status = (uint8_t)atoi(fld[3]);
        }
    }
    free(line);
    fclose(f);
    for (int i = 0; i < m->nconv; i++) m->convs[i].unread = 0;
    loading = false;

    /* Compact: rewrite with what we kept once the log gets long. */
    if (nlines > 4000) {
        char tmp[520];
        snprintf(tmp, sizeof tmp, "%s.tmp", p);
        rename(p, tmp);
        for (int i = 0; i < m->nconv; i++)
            for (int j = 0; j < m->convs[i].n; j++) log_msg(m, &m->convs[i], &m->convs[i].msgs[j]);
        remove(tmp);
    }
}

/* ======================= model ======================= */

void mesh_changed(mesh_t *m) { m->version++; }

void mesh_set_state(mesh_t *m, mesh_state_t st, const char *fmt, ...)
{
    m->state = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->status, sizeof m->status, fmt, ap);
    va_end(ap);
    mesh_changed(m);
}

mesh_conv_t *mesh_conv_find(mesh_t *m, const char *key)
{
    for (int i = 0; i < m->nconv; i++)
        if (strcmp(m->convs[i].key, key) == 0) return &m->convs[i];
    return NULL;
}

static mesh_conv_t *conv_add(mesh_t *m, const char *key)
{
    if (m->nconv == m->capconv) {
        m->capconv = m->capconv ? m->capconv * 2 : 16;
        m->convs = lv_realloc(m->convs, sizeof *m->convs * m->capconv);
    }
    mesh_conv_t *c = &m->convs[m->nconv++];
    memset(c, 0, sizeof *c);
    snprintf(c->key, sizeof c->key, "%s", key);
    return c;
}

mesh_conv_t *mesh_conv_channel(mesh_t *m, int idx, const char *title)
{
    char key[MESH_KEY_LEN];
    snprintf(key, sizeof key, "ch:%d", idx);
    mesh_conv_t *c = mesh_conv_find(m, key);
    if (!c) {
        c = conv_add(m, key);
        c->is_channel = true;
        c->ch_idx = idx;
    }
    if (title && *title) snprintf(c->title, sizeof c->title, "%s", title);
    else if (!c->title[0]) snprintf(c->title, sizeof c->title, "Channel %d", idx);
    c->known = true;
    mesh_changed(m);
    return c;
}

mesh_conv_t *mesh_conv_dm(mesh_t *m, const char *peer_id, const char *title)
{
    char key[MESH_KEY_LEN];
    snprintf(key, sizeof key, "dm:%s", peer_id);
    mesh_conv_t *c = mesh_conv_find(m, key);
    if (!c) {
        c = conv_add(m, key);
        snprintf(c->peer, sizeof c->peer, "%s", peer_id);
        c->known = true;
    }
    if (title && *title) snprintf(c->title, sizeof c->title, "%s", title);
    else if (!c->title[0]) snprintf(c->title, sizeof c->title, "%s", peer_id);
    mesh_changed(m);
    return c;
}

mesh_node_t *mesh_node_find(mesh_t *m, const char *id)
{
    for (int i = 0; i < m->nnode; i++)
        if (strcmp(m->nodes[i].id, id) == 0) return &m->nodes[i];
    return NULL;
}

mesh_node_t *mesh_node_upsert(mesh_t *m, const char *id)
{
    mesh_node_t *n = mesh_node_find(m, id);
    if (n) return n;
    if (m->nnode == m->capnode) {
        m->capnode = m->capnode ? m->capnode * 2 : 32;
        m->nodes = lv_realloc(m->nodes, sizeof *m->nodes * m->capnode);
    }
    n = &m->nodes[m->nnode++];
    memset(n, 0, sizeof *n);
    snprintf(n->id, sizeof n->id, "%s", id);
    n->snr = NAN;
    n->hops = -1;
    mesh_changed(m);
    return n;
}

mesh_msg_t *mesh_add_msg(mesh_t *m, mesh_conv_t *c, const mesh_msg_t *t)
{
    if (c->n == MAX_MSGS_PER_CONV) {   /* drop the oldest */
        lv_free(c->msgs[0].from);
        lv_free(c->msgs[0].text);
        memmove(c->msgs, c->msgs + 1, sizeof *c->msgs * (c->n - 1));
        c->n--;
    }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->msgs = lv_realloc(c->msgs, sizeof *c->msgs * c->cap);
    }
    mesh_msg_t *msg = &c->msgs[c->n++];
    *msg = *t;
    if (!msg->ts) msg->ts = (uint32_t)time(NULL);
    msg->from = t->from ? lv_strdup(t->from) : NULL;
    msg->text = lv_strdup(t->text ? t->text : "");
    if (msg->ts > c->last_ts) c->last_ts = msg->ts;
    log_msg(m, c, msg);
    if (!msg->outgoing && !loading) {
        bool on_screen = viewing_mesh == m && strcmp(viewing_key, c->key) == 0;
        if (!on_screen) {
            c->unread++;
            if (mesh_notify_hook) mesh_notify_hook(m, c, msg);
        }
    }
    mesh_changed(m);
    return msg;
}

mesh_msg_t *mesh_find_out_msg(mesh_t *m, uint32_t ref, mesh_conv_t **conv_out)
{
    if (!ref) return NULL;
    for (int i = 0; i < m->nconv; i++)
        for (int j = m->convs[i].n - 1; j >= 0; j--)
            if (m->convs[i].msgs[j].outgoing && m->convs[i].msgs[j].ref == ref) {
                if (conv_out) *conv_out = &m->convs[i];
                return &m->convs[i].msgs[j];
            }
    return NULL;
}

void mesh_msg_status(mesh_t *m, mesh_msg_t *msg, msg_status_t st)
{
    if (msg->status == MSG_DELIVERED && st != MSG_DELIVERED) return;   /* never downgrade */
    msg->status = (uint8_t)st;
    for (int i = 0; i < m->nconv; i++)
        if (msg >= m->convs[i].msgs && msg < m->convs[i].msgs + m->convs[i].n) {
            log_status(m, &m->convs[i], msg);
            break;
        }
    mesh_changed(m);
}

void mesh_mark_read(mesh_t *m, mesh_conv_t *c)
{
    if (c && c->unread) {
        c->unread = 0;
        mesh_changed(m);
    }
}

int mesh_total_unread(void)
{
    int n = 0;
    for (int k = 0; k < MESH_COUNT; k++)
        for (int i = 0; i < meshes[k].nconv; i++) n += meshes[k].convs[i].unread;
    return n;
}

void mesh_set_viewing(mesh_t *m, mesh_conv_t *c)
{
    viewing_mesh = m;
    snprintf(viewing_key, sizeof viewing_key, "%s", c ? c->key : "");
    if (m && c) mesh_mark_read(m, c);
}

/* ---- sorted views ---- */

static mesh_conv_t **conv_view;
static int conv_view_cap;

static int conv_cmp(const void *a, const void *b)
{
    const mesh_conv_t *x = *(mesh_conv_t *const *)a, *y = *(mesh_conv_t *const *)b;
    if (x->is_channel != y->is_channel) return x->is_channel ? -1 : 1;
    if (x->is_channel) return x->ch_idx - y->ch_idx;
    if (x->last_ts != y->last_ts) return x->last_ts > y->last_ts ? -1 : 1;
    return strcmp(x->title, y->title);
}

int mesh_sorted_convs(mesh_t *m, mesh_conv_t ***out)
{
    if (conv_view_cap < m->nconv + 1) {
        conv_view_cap = m->nconv + 16;
        conv_view = lv_realloc(conv_view, sizeof *conv_view * conv_view_cap);
    }
    int n = 0;
    for (int i = 0; i < m->nconv; i++)
        if (m->convs[i].known || m->convs[i].n) conv_view[n++] = &m->convs[i];
    qsort(conv_view, n, sizeof *conv_view, conv_cmp);
    *out = conv_view;
    return n;
}

static mesh_node_t **node_view;
static int node_view_cap;

static int node_cmp(const void *a, const void *b)
{
    const mesh_node_t *x = *(mesh_node_t *const *)a, *y = *(mesh_node_t *const *)b;
    if (x->last_heard != y->last_heard) return x->last_heard > y->last_heard ? -1 : 1;
    return strcmp(x->name, y->name);
}

int mesh_sorted_nodes(mesh_t *m, mesh_node_t ***out)
{
    if (node_view_cap < m->nnode + 1) {
        node_view_cap = m->nnode + 16;
        node_view = lv_realloc(node_view, sizeof *node_view * node_view_cap);
    }
    int n = 0;
    for (int i = 0; i < m->nnode; i++)
        if (!m->nodes[i].is_self) node_view[n++] = &m->nodes[i];
    qsort(node_view, n, sizeof *node_view, node_cmp);
    *out = node_view;
    return n;
}

/* ======================= radio ownership ======================= */

static const char *unit_for(mesh_kind_t k)
{
    const char *e = getenv(k == MESH_MESHTASTIC ? "ZOS_MESHTASTIC_UNIT" : "ZOS_MESHCORE_UNIT");
    if (e && *e) return e;
    return k == MESH_MESHTASTIC ? "meshtasticd.service" : "openhop-repeater.service";
}

static void reap(lv_timer_t *t)
{
    LV_UNUSED(t);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

/* Stop the other daemon, start ours. systemd's Conflicts= (installed by
 * tools/install-mesh.sh) would do the stop on its own; doing it explicitly
 * also works without the drop-ins. Runs in the background. */
static void switch_daemons(mesh_kind_t k)
{
    if (!radio_ctl) return;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "systemctl stop %s; systemctl start --no-block %s",
             unit_for(k == MESH_MESHTASTIC ? MESH_MESHCORE : MESH_MESHTASTIC), unit_for(k));
    char *argv[] = {"/bin/sh", "-c", cmd, NULL};
    pid_t pid;
    if (posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ) != 0)
        LV_LOG_WARN("could not run: %s", cmd);
}

static void save_active(void)
{
    char p[512];
    snprintf(p, sizeof p, "%s/radio.conf", zos_cfg.state_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "active=%s\n", !active ? "off" : active->kind == MESH_MESHTASTIC ? "meshtastic" : "meshcore");
    fclose(f);
}

static void parse_addr(mesh_t *m, const char *env, const char *dflt_host, int dflt_port)
{
    const char *v = getenv(env);
    snprintf(m->host, sizeof m->host, "%s", dflt_host);
    m->port = dflt_port;
    if (!v || !*v) return;
    const char *colon = strrchr(v, ':');
    if (colon) {
        snprintf(m->host, sizeof m->host, "%.*s", (int)(colon - v), v);
        m->port = atoi(colon + 1);
    } else {
        snprintf(m->host, sizeof m->host, "%s", v);
    }
}

void mesh_activate(mesh_kind_t k)
{
    mesh_t *m = &meshes[k];
    if (active == m) return;
    if (active) {
        active->be->stop(active);
        mesh_set_state(active, MESH_OFF, "Radio is in use by %s", m->name);
    }
    active = m;
    save_active();
    switch_daemons(k);
    mesh_set_state(m, MESH_CONNECTING, "Starting %s…", m->daemon);
    m->be->start(m);
}

void mesh_deactivate(void)
{
    if (!active) return;
    active->be->stop(active);
    mesh_set_state(active, MESH_OFF, "Radio off");
    active = NULL;
    save_active();
}

mesh_t *mesh_get(mesh_kind_t k) { return &meshes[k]; }
mesh_t *mesh_active(void) { return active; }

void mesh_service_init(bool control_radio)
{
    radio_ctl = control_radio;
    lv_timer_create(reap, 1000, NULL);

    mesh_t *mt = &meshes[MESH_MESHTASTIC];
    mt->kind = MESH_MESHTASTIC;
    mt->name = "Meshtastic";
    mt->daemon = "meshtasticd";
    mt->be = &mesh_backend_meshtastic;
    parse_addr(mt, "ZOS_MESHTASTIC_ADDR", "127.0.0.1", 4403);

    mesh_t *mc = &meshes[MESH_MESHCORE];
    mc->kind = MESH_MESHCORE;
    mc->name = "MeshCore";
    mc->daemon = "openhop-repeater";
    mc->be = &mesh_backend_meshcore;
    mc->has_advert = true;
    parse_addr(mc, "ZOS_MESHCORE_ADDR", "127.0.0.1", 5000);

    for (int k = 0; k < MESH_COUNT; k++) {
        snprintf(meshes[k].status, sizeof meshes[k].status, "Radio is off");
        load_history(&meshes[k]);
    }

    /* Resume whichever stack owned the radio last time. */
    char p[512], line[64];
    snprintf(p, sizeof p, "%s/radio.conf", zos_cfg.state_dir);
    FILE *f = fopen(p, "r");
    if (f) {
        if (fgets(line, sizeof line, f)) {
            if (strncmp(line, "active=meshtastic", 17) == 0) mesh_activate(MESH_MESHTASTIC);
            else if (strncmp(line, "active=meshcore", 15) == 0) mesh_activate(MESH_MESHCORE);
        }
        fclose(f);
    }
}
