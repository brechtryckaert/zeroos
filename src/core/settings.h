/* Persistent user settings: a key=value file in the state directory
 * (settings.conf). Values are read once and written on every change. */
#ifndef ZOS_SETTINGS_H
#define ZOS_SETTINGS_H

#include <stdbool.h>

int zos_setting_int(const char *key, int def);
void zos_setting_set_int(const char *key, int v);
const char *zos_setting_str(const char *key, const char *def);
void zos_setting_set_str(const char *key, const char *v);

#endif
