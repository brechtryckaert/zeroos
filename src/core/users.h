/* User accounts ("profiles"): who is using ZeroOS.
 *
 * ZeroOS runs as one Linux account; each ZeroOS user gets
 *   <media root>/Users/<Name>/{Books,Music,Videos,Pictures}   their media
 *   <state>/users/<id>/                                     their app data
 *                                                           and settings
 * and logs in at boot with a PIN, a password or nothing. The list lives in
 * <state>/users.json (hashed secrets, crypt SHA-512).
 *
 * Logging in points zos_cfg's folders at the user's; logging out restarts
 * ZeroOS, so nothing of one user is left in memory for the next. */
#ifndef ZOS_USERS_H
#define ZOS_USERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { ZOS_LOCK_NONE, ZOS_LOCK_PIN, ZOS_LOCK_PASSWORD } zos_lock_t;

#define ZOS_USER_NAME_MAX 21   /* 20 characters */

typedef struct {
    char id[12];               /* "u1", "u2", … (never reused) */
    char name[ZOS_USER_NAME_MAX];
    uint32_t color;
    bool admin;
    zos_lock_t lock;
    char hash[128];            /* crypt() of the PIN or password */
} zos_user_t;

/* Load the list (main.c, after zos_cfg is set). */
void zos_users_init(void);
int zos_users_count(void);
const zos_user_t *zos_users_get(int i);
const zos_user_t *zos_users_find(const char *id);
/* The logged-in user, or NULL (no accounts yet, or before login). */
const zos_user_t *zos_user_current(void);
/* Admins manage users and install updates. True when nobody is logged in
 * and there are no accounts (development runs, tests). */
bool zos_user_is_admin(void);

/* PIN / password check, with a growing wait after 5 wrong tries.
 * Returns false and fills `err` when it is wrong or too soon. */
bool zos_users_check(const zos_user_t *u, const char *secret, char *err, size_t n);
/* Log in: point zos_cfg and the settings at this user's folders. */
void zos_users_login(const zos_user_t *u);

/* Secrets: "" = no lock, 4-8 digits = PIN, anything else (4+ chars) =
 * password. Validates; returns the kind or -1 with `err` filled. */
int zos_users_classify_secret(const char *secret, const char *repeat, char *err, size_t n);

/* The first account takes over everything already on the device (media
 * folders, app data, settings). Later ones start empty. Returns the new
 * user or NULL with `err`. */
const zos_user_t *zos_users_add(const char *name, const char *secret, const char *repeat, bool admin,
                                char *err, size_t n);
bool zos_users_remove(const char *id, char *err, size_t n);
bool zos_users_rename(const char *id, const char *name, char *err, size_t n);
bool zos_users_set_secret(const char *id, const char *secret, const char *repeat, char *err, size_t n);
bool zos_users_set_admin(const char *id, bool admin, char *err, size_t n);

/* Where a user's media lives (<media root>/Users/<Name>). */
void zos_users_media_dir(const zos_user_t *u, char *out, size_t n);
/* The user's SSH key folder, or NULL when nobody is logged in. */
const char *zos_user_ssh_dir(void);

/* ZeroOS restarts for USB drive mode and software updates: keep the
 * current user logged in across that one restart. */
void zos_users_keep_login(int seconds);
/* At startup: the user to log in without asking (a kept login), or NULL. */
const zos_user_t *zos_users_resume(void);

#endif
