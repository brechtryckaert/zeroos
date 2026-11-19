/* Persistent settings: key=value files, read once and written on every
 * change.
 *
 * The device's settings (brightness, HDMI, software update, …) live in the
 * system state folder; everything else belongs to the logged-in user and
 * lives in their state folder. Before anyone logs in (or without accounts)
 * all keys go to the device's file. */
#ifndef ZOS_SETTINGS_H
#define ZOS_SETTINGS_H

#include <stdbool.h>

int zos_setting_int(const char *key, int def);
void zos_setting_set_int(const char *key, int v);
const char *zos_setting_str(const char *key, const char *def);
void zos_setting_set_str(const char *key, const char *v);

/* Users (users.c): from now on, user keys live in `dir`/settings.conf. */
void zos_settings_use_user_dir(const char *dir);
/* Move the user keys of the device's file into `dir`/settings.conf (the
 * first account takes over the settings made before accounts existed). */
void zos_settings_move_user_keys(const char *dir);

#endif
