/* Mesh service: the model both mesh apps share.
 *
 * One mesh_t per stack (Meshtastic, MeshCore). The active one keeps its
 * daemon connection open for as long as ZeroOS runs, so messages arrive
 * (and notify) while you are in another app. Apps are only views.
 *
 * Only one stack can own the LoRa cap at a time: mesh_activate() switches
 * the radio daemon (systemd Conflicts= does the stopping) and the
 * connection. */
#ifndef ZOS_MESH_H
#define ZOS_MESH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { MESH_MESHTASTIC = 0, MESH_MESHCORE = 1, MESH_COUNT } mesh_kind_t;

typedef enum {
    MESH_OFF,          /* not the active stack */
    MESH_CONNECTING,   /* waiting for the daemon's API */
    MESH_SYNCING,      /* connected, downloading nodes/channels */
    MESH_READY,
} mesh_state_t;

typedef enum { MSG_SENDING, MSG_SENT, MSG_DELIVERED, MSG_FAILED, MSG_RECEIVED } msg_status_t;

typedef struct {
    uint32_t ts;           /* unix time */
    char *from;            /* sender display name (incoming), NULL for outgoing */
    char *text;
    bool outgoing;
    uint8_t status;        /* msg_status_t */
    uint32_t ref;          /* backend id used to match acks */
    float snr;             /* incoming only; NAN if unknown */
    int8_t hops;           /* incoming only; -1 if unknown */
} mesh_msg_t;

#define MESH_KEY_LEN 40

typedef struct {
    char key[MESH_KEY_LEN];   /* "ch:<idx>" or "dm:<node id>" */
    char title[48];
    bool is_channel;
    int ch_idx;
    char peer[MESH_KEY_LEN];  /* node id for DMs */
    mesh_msg_t *msgs;
    int n, cap;
    int unread;
    uint32_t last_ts;
    bool known;               /* channel exists on the node (vs. from history only) */
} mesh_conv_t;

typedef struct {
    char id[MESH_KEY_LEN];    /* "!8e21a4c7" (Meshtastic) or pubkey hex prefix (MeshCore) */
    char name[48];
    char short_name[8];
    float snr;                /* NAN if unknown */
    int hops;                 /* -1 if unknown */
    uint32_t last_heard;      /* unix time, 0 if unknown */
    bool is_self;
    uint8_t kind;             /* MeshCore: 1 chat, 2 repeater, 3 room, 4 sensor; 0 otherwise */
    uint32_t num;             /* Meshtastic node number */
    uint8_t pub[32];          /* MeshCore public key */
} mesh_node_t;

struct mesh_backend;

typedef struct mesh {
    mesh_kind_t kind;
    const char *name;         /* "Meshtastic" */
    const char *daemon;       /* "meshtasticd" */
    mesh_state_t state;
    char status[96];          /* human-readable connection status */
    char radio_desc[48];      /* "EU_868 · LongFast" */
    char self_name[48];
    char self_id[MESH_KEY_LEN];
    mesh_conv_t *convs;
    int nconv, capconv;
    mesh_node_t *nodes;
    int nnode, capnode;
    uint32_t version;         /* bumped on every change; views poll it */
    const struct mesh_backend *be;
    void *bs;                 /* backend state */
    char host[64];
    int port;
    bool has_advert;          /* backend supports "send advert" */
} mesh_t;

typedef struct mesh_backend {
    void (*start)(mesh_t *m);
    void (*stop)(mesh_t *m);
    /* Queue `text` for `conv`; adds the outgoing message itself. */
    bool (*send)(mesh_t *m, mesh_conv_t *conv, const char *text);
    void (*advert)(mesh_t *m);
    int max_text;             /* bytes */
} mesh_backend_t;

/* ---- service ---- */
void mesh_service_init(bool control_radio);
mesh_t *mesh_get(mesh_kind_t k);
mesh_t *mesh_active(void);            /* NULL if none */
/* Make `k` own the radio (starts its daemon, stops the other). */
void mesh_activate(mesh_kind_t k);
void mesh_deactivate(void);
int mesh_total_unread(void);
/* The conversation currently on screen (no unread/notification for it). */
void mesh_set_viewing(mesh_t *m, mesh_conv_t *c);

/* ---- model helpers for backends ---- */
void mesh_changed(mesh_t *m);
void mesh_set_state(mesh_t *m, mesh_state_t st, const char *fmt, ...);
mesh_conv_t *mesh_conv_find(mesh_t *m, const char *key);
mesh_conv_t *mesh_conv_channel(mesh_t *m, int idx, const char *title);
mesh_conv_t *mesh_conv_dm(mesh_t *m, const char *peer_id, const char *title);
mesh_node_t *mesh_node_find(mesh_t *m, const char *id);
mesh_node_t *mesh_node_upsert(mesh_t *m, const char *id);
/* Add a message; incoming ones bump unread and may raise a notification. */
mesh_msg_t *mesh_add_msg(mesh_t *m, mesh_conv_t *c, const mesh_msg_t *tmpl);
mesh_msg_t *mesh_find_out_msg(mesh_t *m, uint32_t ref, mesh_conv_t **conv_out);
void mesh_msg_status(mesh_t *m, mesh_msg_t *msg, msg_status_t st);
/* Sorted views (pointers valid until the next change) */
int mesh_sorted_convs(mesh_t *m, mesh_conv_t ***out);
int mesh_sorted_nodes(mesh_t *m, mesh_node_t ***out);
void mesh_mark_read(mesh_t *m, mesh_conv_t *c);

extern const mesh_backend_t mesh_backend_meshtastic;
extern const mesh_backend_t mesh_backend_meshcore;

/* Notification hook (set by the shell): called for incoming messages that
 * are not on screen. */
extern void (*mesh_notify_hook)(mesh_t *m, mesh_conv_t *c, const mesh_msg_t *msg);

#endif
