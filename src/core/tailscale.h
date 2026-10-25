/* Tailscale LocalAPI client (the unix socket the `tailscale` CLI uses).
 *
 * Only the blocking calls below run in zos_task workers; the "service"
 * part polls status in the background so the status bar can show a VPN
 * indicator and other apps (SSH) can list tailnet devices. */
#ifndef ZOS_TAILSCALE_H
#define ZOS_TAILSCALE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char id[48];        /* StableNodeID, used to pick an exit node */
    char host[64];
    char dns[128];      /* MagicDNS name without trailing dot */
    char os[16];
    char ip[48];        /* first Tailscale IP (IPv4 if any) */
    bool online, active, exit_node, exit_option;
    int64_t rx, tx;
    char relay[16];
    char last_seen[32]; /* ISO time, only when offline */
} ts_peer_t;

typedef struct {
    bool reachable;     /* tailscaled answered */
    char err[160];
    char state[24];     /* BackendState: NeedsLogin, Stopped, Starting, Running, NeedsMachineAuth */
    char auth_url[256];
    char version[48];
    char tailnet[96];
    char self_host[64], self_dns[128], self_ip[48];
    char health[3][200];
    int nhealth;
    ts_peer_t *peers;   /* online first, then by name */
    int npeers;
} ts_status_t;

/* ---- blocking, worker threads only ---- */
bool ts_fetch_status(ts_status_t *out);
void ts_status_free(ts_status_t *s);
bool ts_set_running(bool on, char *err, int errlen);
bool ts_login(char *err, int errlen);
bool ts_set_exit_node(const char *stable_id, char *err, int errlen);   /* "" clears */

/* ---- UI thread ---- */
void ts_service_init(void);
/* Last background snapshot (may be stale by ~15 s); NULL before the first. */
const ts_status_t *ts_snapshot(void);
/* Ask the background poller to refresh soon. */
void ts_poke(void);
bool ts_connected(void);

#endif
