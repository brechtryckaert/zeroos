/* The panel backlight (sysfs), shared by Settings and the brightness keys. */
#ifndef ZOS_BACKLIGHT_H
#define ZOS_BACKLIGHT_H

#include <stdbool.h>

int zos_backlight_get(void);          /* percent, or -1 without a backlight */
bool zos_backlight_set(int pct);      /* 5..100, saved as the "brightness" setting */

#endif
