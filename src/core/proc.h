/* Run a command and capture its output (for worker threads: it blocks).
 * Returns the exit status, or -1 if it could not run or timed out. */
#ifndef ZOS_PROC_H
#define ZOS_PROC_H

#include <stddef.h>

int zos_run(const char *const argv[], char *out, size_t cap, int timeout_ms);
/* Run detached, ignoring output (e.g. "systemctl reboot"). */
void zos_run_bg(const char *const argv[]);

#endif
