/* Software updates: ZeroOS's side of /usr/local/sbin/zeroos-update.
 *
 * The helper does the work (it finds the newest GitHub release, downloads
 * its update bundle, installs it and restarts ZeroOS). This module asks it,
 * keeps the last answer, follows a running install through its status
 * file, and after a restart says how the update went. */
#ifndef ZOS_UPDATE_H
#define ZOS_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct {
    time_t checked;          /* 0: never asked */
    bool update;             /* a newer release has an update bundle */
    char installed[64];      /* /etc/zeroos-release ZEROOS_VERSION */
    char installed_tag[48];  /* the release it came from, if known */
    char latest[48];
    char published[16];      /* YYYY-MM-DD */
    long size;               /* bundle bytes */
    char error[200];
    char notes[4096];        /* the release notes (markdown) */
} zos_update_info_t;

typedef struct {
    char state[16];          /* idle starting checking downloading unpacking installing done uptodate error */
    int pct;
    char msg[200];
    char version[48];
    time_t time;
} zos_update_status_t;

/* At startup: report the result of an update that just happened, confirm
 * that the new version runs, and check for updates now and then. */
void zos_update_init(void);

bool zos_update_available(void);          /* the helper is installed */
const zos_update_info_t *zos_update_info(void);
bool zos_update_checking(void);
void zos_update_check(void);              /* in the background, asked by the user */
unsigned zos_update_generation(void);     /* bumps whenever the info changes */

bool zos_update_install(char *err, size_t n);
bool zos_update_status(zos_update_status_t *st);
bool zos_update_busy(void);               /* an install is running */
/* The version a rollback would bring back, or false if there is none. */
bool zos_update_rollback_version(char *out, size_t n);
bool zos_update_rollback(char *err, size_t n);

#endif
