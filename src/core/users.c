/* User accounts (see users.h). */
#define _GNU_SOURCE
#include "users.h"

#include <cjson/cJSON.h>
#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../zos.h"
#include "settings.h"

#define MAX_USERS 16

static zos_user_t users[MAX_USERS];
static int nusers;
static int next_id = 1;
static const zos_user_t *current;
static struct { int fails; time_t until; } tries[MAX_USERS];

/* What zos_cfg pointed at before anyone logged in: the device's folders. */
static char legacy_books[512], legacy_music[512], legacy_video[512], legacy_pics[512];
static char u_state[512], u_books[600], u_music[600], u_video[600], u_pics[600], u_ssh[600];

static const uint32_t palette[] = {0xf5a524, 0x3b82f6, 0x22c55e, 0xe5484d, 0xa855f7, 0x14b8a6, 0xec4899, 0xf97316};

static const char *sys_dir(void) { return zos_cfg.system_state_dir ? zos_cfg.system_state_dir : zos_cfg.state_dir; }
static const char *media_root(void) { return zos_cfg.media_root ? zos_cfg.media_root : getenv("HOME"); }

static void list_path(char *out, size_t n) { snprintf(out, n, "%s/users.json", sys_dir()); }
static void state_of(const zos_user_t *u, char *out, size_t n) { snprintf(out, n, "%s/users/%s", sys_dir(), u->id); }

void zos_users_media_dir(const zos_user_t *u, char *out, size_t n)
{
    snprintf(out, n, "%s/Users/%s", media_root(), u->name);
}

/* ---------------- load / save ---------------- */

static void save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next_id", next_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "users");
    for (int i = 0; i < nusers; i++) {
        zos_user_t *u = &users[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", u->id);
        cJSON_AddStringToObject(o, "name", u->name);
        cJSON_AddNumberToObject(o, "color", u->color);
        cJSON_AddBoolToObject(o, "admin", u->admin);
        cJSON_AddStringToObject(o, "lock", u->lock == ZOS_LOCK_PIN ? "pin" : u->lock == ZOS_LOCK_PASSWORD ? "password" : "none");
        cJSON_AddStringToObject(o, "hash", u->hash);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    char p[600], tmp[620];
    list_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        size_t len = strlen(txt);
        if (write(fd, txt, len) == (ssize_t)len) {
            fsync(fd);
            close(fd);
            rename(tmp, p);
        } else {
            close(fd);
        }
    }
    free(txt);
}

void zos_users_init(void)
{
    snprintf(legacy_books, sizeof legacy_books, "%s", zos_cfg.books_dir);
    snprintf(legacy_music, sizeof legacy_music, "%s", zos_cfg.music_dir);
    snprintf(legacy_video, sizeof legacy_video, "%s", zos_cfg.video_dir);
    snprintf(legacy_pics, sizeof legacy_pics, "%s", zos_cfg.pictures_dir);
    nusers = 0;
    char p[600];
    list_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = 0;
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { fprintf(stderr, "zeroos: %s is damaged\n", p); return; }
    cJSON *ni = cJSON_GetObjectItem(root, "next_id");
    if (cJSON_IsNumber(ni)) next_id = ni->valueint;
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(root, "users")) {
        if (nusers >= MAX_USERS) break;
        zos_user_t *u = &users[nusers];
        memset(u, 0, sizeof *u);
        cJSON *v;
        if (cJSON_IsString(v = cJSON_GetObjectItem(o, "id"))) snprintf(u->id, sizeof u->id, "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItem(o, "name"))) snprintf(u->name, sizeof u->name, "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItem(o, "hash"))) snprintf(u->hash, sizeof u->hash, "%s", v->valuestring);
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "color"))) u->color = (uint32_t)v->valuedouble;
        u->admin = cJSON_IsTrue(cJSON_GetObjectItem(o, "admin"));
        v = cJSON_GetObjectItem(o, "lock");
        const char *l = cJSON_IsString(v) ? v->valuestring : "none";
        u->lock = !strcmp(l, "pin") ? ZOS_LOCK_PIN : !strcmp(l, "password") ? ZOS_LOCK_PASSWORD : ZOS_LOCK_NONE;
        if (u->lock != ZOS_LOCK_NONE && !u->hash[0]) u->lock = ZOS_LOCK_NONE;
        if (u->id[0] && u->name[0]) nusers++;
    }
    cJSON_Delete(root);
}

int zos_users_count(void) { return nusers; }
const zos_user_t *zos_users_get(int i) { return i >= 0 && i < nusers ? &users[i] : NULL; }
const zos_user_t *zos_user_current(void) { return current; }
const char *zos_user_ssh_dir(void) { return current ? u_ssh : NULL; }

const zos_user_t *zos_users_find(const char *id)
{
    for (int i = 0; i < nusers; i++)
        if (!strcmp(users[i].id, id)) return &users[i];
    return NULL;
}

static int index_of(const char *id)
{
    for (int i = 0; i < nusers; i++)
        if (!strcmp(users[i].id, id)) return i;
    return -1;
}

bool zos_user_is_admin(void) { return current ? current->admin : nusers == 0; }

static int admin_count(void)
{
    int a = 0;
    for (int i = 0; i < nusers; i++) a += users[i].admin;
    return a;
}

/* ---------------- secrets ---------------- */

static bool make_hash(const char *secret, char *out, size_t n)
{
    unsigned char rnd[12];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) { if (fd >= 0) close(fd); return false; }
    close(fd);
    static const char abc[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    char salt[32] = "$6$";
    for (int i = 0; i < 12; i++) salt[3 + i] = abc[rnd[i] % 64];
    salt[15] = '$';
    salt[16] = 0;
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *h = crypt_r(secret, salt, &cd);
    if (!h || h[0] == '*') return false;
    snprintf(out, n, "%s", h);
    return true;
}

int zos_users_classify_secret(const char *secret, const char *repeat, char *err, size_t n)
{
    size_t len = strlen(secret);
    if (len == 0) return ZOS_LOCK_NONE;
    if (repeat && strcmp(secret, repeat) != 0) { snprintf(err, n, "The two entries don't match"); return -1; }
    bool digits = true;
    for (const char *p = secret; *p; p++) if (!isdigit((unsigned char)*p)) digits = false;
    if (digits) {
        if (len < 4 || len > 8) { snprintf(err, n, "A PIN has 4 to 8 digits"); return -1; }
        return ZOS_LOCK_PIN;
    }
    if (len < 4) { snprintf(err, n, "A password needs at least 4 characters"); return -1; }
    return ZOS_LOCK_PASSWORD;
}

bool zos_users_check(const zos_user_t *u, const char *secret, char *err, size_t n)
{
    int i = index_of(u->id);
    if (i < 0) { snprintf(err, n, "Unknown user"); return false; }
    if (u->lock == ZOS_LOCK_NONE) return true;
    time_t now = time(NULL);
    if (tries[i].until > now) {
        snprintf(err, n, "Too many tries: wait %d s", (int)(tries[i].until - now));
        return false;
    }
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    const char *h = crypt_r(secret, u->hash, &cd);
    if (h && !strcmp(h, u->hash)) {
        tries[i].fails = 0;
        return true;
    }
    tries[i].fails++;
    if (tries[i].fails >= 5) {
        int wait = 30 << (tries[i].fails - 5 > 5 ? 5 : tries[i].fails - 5);
        if (wait > 15 * 60) wait = 15 * 60;
        tries[i].until = now + wait;
        snprintf(err, n, "Wrong %s. Wait %d s", u->lock == ZOS_LOCK_PIN ? "PIN" : "password", wait);
    } else {
        snprintf(err, n, "Wrong %s", u->lock == ZOS_LOCK_PIN ? "PIN" : "password");
    }
    return false;
}

/* ---------------- files ---------------- */

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void copy_file(const char *from, const char *to, mode_t mode)
{
    FILE *a = fopen(from, "rb");
    if (!a) return;
    int fd = open(to, O_WRONLY | O_CREAT | O_EXCL, mode);
    FILE *b = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (b) {
        char buf[16384];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, r, b);
        fclose(b);
    } else if (fd >= 0) {
        close(fd);
    }
    fclose(a);
}

static int rm_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw)
{
    LV_UNUSED(sb); LV_UNUSED(flag); LV_UNUSED(ftw);
    return remove(p);
}

static void rm_rf(const char *p) { nftw(p, rm_cb, 16, FTW_DEPTH | FTW_PHYS); }

/* The welcome book (and whatever else ships in share/zeroos/books). */
static void copy_guide_books(const char *books)
{
    const char *src[] = {getenv("ZOS_GUIDE_BOOKS"), "/usr/local/share/zeroos/books", NULL};
    for (int i = 0; i < 2; i++) {
        if (!src[i] || !is_dir(src[i])) continue;
        DIR *d = opendir(src[i]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char a[1024], b[1024];
            snprintf(a, sizeof a, "%s/%s", src[i], e->d_name);
            snprintf(b, sizeof b, "%s/%s", books, e->d_name);
            copy_file(a, b, 0644);
        }
        if (d) closedir(d);
        return;
    }
}

static const char *const folders[] = {"Books", "Music", "Videos", "Pictures"};

static void make_folders(const zos_user_t *u)
{
    char m[600], p[700];
    zos_users_media_dir(u, m, sizeof m);
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", m, folders[i]);
        zos_mkdirs(p);
    }
    char st[600];
    state_of(u, st, sizeof st);
    zos_mkdirs(st);
}

/* A device folder from before accounts existed, if it is the real one
 * (its default place or on the media drive): not a folder a developer
 * pointed ZeroOS at, like the repository's assets/books. */
static bool legacy_real(const char *legacy, const char *name, char *real, size_t n)
{
    char r[PATH_MAX], a[PATH_MAX], b[PATH_MAX], p[700];
    if (!realpath(legacy, r) || !is_dir(r)) return false;
    snprintf(p, sizeof p, "%s/%s", media_root(), name);
    bool ok = realpath(p, a) && !strcmp(a, r);
    snprintf(p, sizeof p, "%s/%s", getenv("HOME") ? getenv("HOME") : "/", name);
    ok = ok || (realpath(p, b) && !strcmp(b, r));
    if (ok) snprintf(real, n, "%s", r);
    return ok;
}

/* The first account gets what is already on the device. */
static void adopt_device_data(const zos_user_t *u)
{
    char m[600], to[700], real[PATH_MAX];
    zos_users_media_dir(u, m, sizeof m);
    zos_mkdirs(m);
    const char *legacy[] = {legacy_books, legacy_music, legacy_video, legacy_pics};
    for (int i = 0; i < 4; i++) {
        snprintf(to, sizeof to, "%s/%s", m, folders[i]);
        if (is_dir(to) || !legacy_real(legacy[i], folders[i], real, sizeof real)) continue;
        if (rename(real, to) != 0)
            fprintf(stderr, "zeroos: could not move %s to %s: %s\n", real, to, strerror(errno));
    }
    /* App data: everything in the state folder that isn't the device's. */
    char st[600];
    state_of(u, st, sizeof st);
    zos_mkdirs(st);
    DIR *d = opendir(sys_dir());
    struct dirent *e;
    while (d && (e = readdir(d))) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || !strcmp(nm, "users") || !strncmp(nm, "users.json", 10) || !strcmp(nm, "settings.conf") ||
            !strcmp(nm, "radio.conf") || !strncmp(nm, "mesh-", 5) || !strncmp(nm, "mpv-", 4))
            continue;
        char a[1024], b[1024];
        snprintf(a, sizeof a, "%s/%s", sys_dir(), nm);
        snprintf(b, sizeof b, "%s/%s", st, nm);
        rename(a, b);
    }
    if (d) closedir(d);
    zos_settings_move_user_keys(st);
    /* The SSH key and known hosts the SSH app has been using. */
    const char *home = getenv("HOME");
    if (home) {
        char sd[700], a[800], b[800];
        snprintf(sd, sizeof sd, "%s/ssh", st);
        zos_mkdirs(sd);
        chmod(sd, 0700);
        static const char *const keys[] = {"id_ed25519", "id_ed25519.pub", "known_hosts"};
        for (int i = 0; i < 3; i++) {
            snprintf(a, sizeof a, "%s/.ssh/%s", home, keys[i]);
            snprintf(b, sizeof b, "%s/%s", sd, keys[i]);
            copy_file(a, b, i == 0 ? 0600 : 0644);
        }
    }
}

/* ---------------- accounts ---------------- */

static bool valid_name(const char *name, int except, char *err, size_t n)
{
    size_t len = strlen(name);
    if (len == 0) { snprintf(err, n, "Type a name"); return false; }
    if (len > ZOS_USER_NAME_MAX - 1) { snprintf(err, n, "Names have at most 20 characters"); return false; }
    if (name[0] == ' ' || name[0] == '.' || name[len - 1] == ' ' || name[len - 1] == '.') {
        snprintf(err, n, "A name can't start or end with a space or dot");
        return false;
    }
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) { snprintf(err, n, "Names can't contain / \\ : * ? \" < > |"); return false; }
    }
    for (int i = 0; i < nusers; i++)
        if (i != except && !strcasecmp(users[i].name, name)) { snprintf(err, n, "There is already a user called %s", users[i].name); return false; }
    return true;
}

const zos_user_t *zos_users_add(const char *name_in, const char *secret, const char *repeat, bool admin,
                                char *err, size_t n)
{
    char name[64];
    snprintf(name, sizeof name, "%s", name_in);
    if (nusers >= MAX_USERS) { snprintf(err, n, "There can be at most %d users", MAX_USERS); return NULL; }
    if (nusers > 0 && !zos_user_is_admin()) { snprintf(err, n, "Only an admin can add users"); return NULL; }
    if (!valid_name(name, -1, err, n)) return NULL;
    int kind = zos_users_classify_secret(secret, repeat, err, n);
    if (kind < 0) return NULL;
    zos_user_t u = {0};
    snprintf(u.id, sizeof u.id, "u%d", next_id);
    snprintf(u.name, sizeof u.name, "%s", name);
    u.color = palette[(next_id - 1) % (int)(sizeof palette / sizeof palette[0])];
    u.admin = admin || nusers == 0;
    u.lock = (zos_lock_t)kind;
    if (kind != ZOS_LOCK_NONE && !make_hash(secret, u.hash, sizeof u.hash)) {
        snprintf(err, n, "Could not store the %s", kind == ZOS_LOCK_PIN ? "PIN" : "password");
        return NULL;
    }
    char m[600];
    zos_users_media_dir(&u, m, sizeof m);
    bool existed = is_dir(m);
    bool first = nusers == 0;
    if (first) adopt_device_data(&u);
    make_folders(&u);
    if (!existed && !first) {
        char b[700];
        snprintf(b, sizeof b, "%s/Books", m);
        copy_guide_books(b);
    }
    next_id++;
    users[nusers++] = u;
    save();
    return &users[nusers - 1];
}

bool zos_users_remove(const char *id, char *err, size_t n)
{
    int i = index_of(id);
    if (i < 0) { snprintf(err, n, "Unknown user"); return false; }
    if (!zos_user_is_admin()) { snprintf(err, n, "Only an admin can remove users"); return false; }
    if (current && !strcmp(current->id, id)) { snprintf(err, n, "You can't remove yourself"); return false; }
    if (users[i].admin && admin_count() <= 1) { snprintf(err, n, "Keep at least one admin"); return false; }
    char st[600];
    state_of(&users[i], st, sizeof st);
    rm_rf(st);   /* their app data; their media folder stays on the drive */
    const char *cur_id = current ? current->id : NULL;
    char keep[12] = "";
    if (cur_id) snprintf(keep, sizeof keep, "%s", cur_id);
    memmove(&users[i], &users[i + 1], (size_t)(nusers - i - 1) * sizeof users[0]);
    memmove(&tries[i], &tries[i + 1], (size_t)(nusers - i - 1) * sizeof tries[0]);
    nusers--;
    if (keep[0]) current = zos_users_find(keep);
    save();
    return true;
}

bool zos_users_rename(const char *id, const char *name_in, char *err, size_t n)
{
    int i = index_of(id);
    if (i < 0) { snprintf(err, n, "Unknown user"); return false; }
    bool self = current && !strcmp(current->id, id);
    if (!self && !zos_user_is_admin()) { snprintf(err, n, "Only an admin can rename other users"); return false; }
    char name[64];
    snprintf(name, sizeof name, "%s", name_in);
    if (!strcmp(name, users[i].name)) return true;
    if (!valid_name(name, i, err, n)) return false;
    char from[600], to[600];
    zos_users_media_dir(&users[i], from, sizeof from);
    zos_user_t tmp = users[i];
    snprintf(tmp.name, sizeof tmp.name, "%s", name);
    zos_users_media_dir(&tmp, to, sizeof to);
    if (is_dir(from)) {
        /* exFAT: a rename that only changes case still works in place. */
        if (is_dir(to) && strcasecmp(from, to) != 0) { snprintf(err, n, "There is already a folder Users/%s", name); return false; }
        if (rename(from, to) != 0) { snprintf(err, n, "Could not rename the folder: %s", strerror(errno)); return false; }
    }
    snprintf(users[i].name, sizeof users[i].name, "%s", name);
    save();
    if (self) zos_users_login(&users[i]);   /* the folders moved */
    return true;
}

bool zos_users_set_secret(const char *id, const char *secret, const char *repeat, char *err, size_t n)
{
    int i = index_of(id);
    if (i < 0) { snprintf(err, n, "Unknown user"); return false; }
    bool self = current && !strcmp(current->id, id);
    if (!self && !zos_user_is_admin()) { snprintf(err, n, "Only an admin can change other users' locks"); return false; }
    int kind = zos_users_classify_secret(secret, repeat, err, n);
    if (kind < 0) return false;
    char h[128] = "";
    if (kind != ZOS_LOCK_NONE && !make_hash(secret, h, sizeof h)) { snprintf(err, n, "Could not store it"); return false; }
    users[i].lock = (zos_lock_t)kind;
    snprintf(users[i].hash, sizeof users[i].hash, "%s", h);
    tries[i].fails = 0;
    tries[i].until = 0;
    save();
    return true;
}

bool zos_users_set_admin(const char *id, bool admin, char *err, size_t n)
{
    int i = index_of(id);
    if (i < 0) { snprintf(err, n, "Unknown user"); return false; }
    if (!zos_user_is_admin()) { snprintf(err, n, "Only an admin can change this"); return false; }
    if (!admin && users[i].admin && admin_count() <= 1) { snprintf(err, n, "Keep at least one admin"); return false; }
    users[i].admin = admin;
    save();
    return true;
}

/* ---------------- login ---------------- */

void zos_users_login(const zos_user_t *u)
{
    current = u;
    char m[600];
    zos_users_media_dir(u, m, sizeof m);
    state_of(u, u_state, sizeof u_state);
    snprintf(u_books, sizeof u_books, "%s/Books", m);
    snprintf(u_music, sizeof u_music, "%s/Music", m);
    snprintf(u_video, sizeof u_video, "%s/Videos", m);
    snprintf(u_pics, sizeof u_pics, "%s/Pictures", m);
    snprintf(u_ssh, sizeof u_ssh, "%s/ssh", u_state);
    make_folders(u);
    zos_mkdirs(u_ssh);
    chmod(u_ssh, 0700);
    zos_cfg.state_dir = u_state;
    zos_cfg.books_dir = u_books;
    zos_cfg.music_dir = u_music;
    zos_cfg.video_dir = u_video;
    zos_cfg.pictures_dir = u_pics;
    zos_settings_use_user_dir(u_state);
    zos_setting_set_str("last_user", u->id);
}

void zos_users_keep_login(int seconds)
{
    if (!current) return;
    zos_setting_set_str("resume_user", current->id);
    zos_setting_set_int("resume_until", (int)(time(NULL) / 60 + (seconds + 59) / 60 + 1));
}

const zos_user_t *zos_users_resume(void)
{
    char id[16];
    snprintf(id, sizeof id, "%s", zos_setting_str("resume_user", ""));
    long until = (long)zos_setting_int("resume_until", 0) * 60;
    if (!id[0]) return NULL;
    zos_setting_set_str("resume_user", "");
    zos_setting_set_int("resume_until", 0);
    if (time(NULL) > until) return NULL;
    return zos_users_find(id);
}
