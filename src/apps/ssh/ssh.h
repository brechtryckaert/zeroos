#ifndef ZOS_SSH_H
#define ZOS_SSH_H

#include "../../zos.h"

extern const zos_app_t zos_app_ssh;

/* Open the SSH app with a prefilled connection form for `host` (from the
 * Tailscale app's device list). `label` is shown as the connection name. */
void ssh_open_host(const char *label, const char *host);

#endif
