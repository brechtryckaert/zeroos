#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../zos.h"
#include "settings.h"

#define MAX_KEYS 64

typedef struct {
    char path[600];
    struct { char key[40]; char val[200]; } kv[MAX_KEYS];
    int n;
    bool loaded;
} store_t;

static store_t sys, usr;
static bool have_user;

/* Keys that belong to the device, not to whoever is logged in. */
static const char *const system_keys[] = {
    "brightness", "hdmi_mirror", "video_hdmi", "hdmi_audio", "start_app",
    "update_auto", "update_last", "update_notified",
    "last_user", "resume_user", "resume_until", NULL,
};

static bool is_system(const char *key)
{
    for (int i = 0; system_keys[i]; i++)
        if (!strcmp(system_keys[i], key)) return true;
    return false;
}

static void load(store_t *s)
{
    if (s->loaded) return;
    s->loaded = true;
    if (s == &sys && !s->path[0]) {
        const char *d = zos_cfg.system_state_dir ? zos_cfg.system_state_dir : zos_cfg.state_dir;
        snprintf(s->path, sizeof s->path, "%s/settings.conf", d);
    }
    FILE *f = fopen(s->path, "r");
    if (!f) return;
    char line[300];
    while (s->n < MAX_KEYS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        snprintf(s->kv[s->n].key, sizeof s->kv[s->n].key, "%s", line);
        snprintf(s->kv[s->n].val, sizeof s->kv[s->n].val, "%s", eq + 1);
        s->n++;
    }
    fclose(f);
}

static void save(store_t *s)
{
    char tmp[620];
    snprintf(tmp, sizeof tmp, "%s.tmp", s->path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < s->n; i++) fprintf(f, "%s=%s\n", s->kv[i].key, s->kv[i].val);
    fclose(f);
    rename(tmp, s->path);
}

static store_t *store_for(const char *key)
{
    store_t *s = have_user && !is_system(key) ? &usr : &sys;
    load(s);
    return s;
}

static int find(store_t *s, const char *key)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->kv[i].key, key)) return i;
    return -1;
}

const char *zos_setting_str(const char *key, const char *def)
{
    store_t *s = store_for(key);
    int i = find(s, key);
    return i < 0 ? def : s->kv[i].val;
}

int zos_setting_int(const char *key, int def)
{
    const char *v = zos_setting_str(key, NULL);
    return v ? atoi(v) : def;
}

static void set_in(store_t *s, const char *key, const char *v)
{
    int i = find(s, key);
    if (i < 0) {
        if (s->n >= MAX_KEYS) return;
        i = s->n++;
        snprintf(s->kv[i].key, sizeof s->kv[i].key, "%s", key);
    }
    snprintf(s->kv[i].val, sizeof s->kv[i].val, "%s", v);
}

void zos_setting_set_str(const char *key, const char *v)
{
    store_t *s = store_for(key);
    set_in(s, key, v);
    save(s);
}

void zos_setting_set_int(const char *key, int v)
{
    char b[24];
    snprintf(b, sizeof b, "%d", v);
    zos_setting_set_str(key, b);
}

void zos_settings_use_user_dir(const char *dir)
{
    memset(&usr, 0, sizeof usr);
    have_user = dir != NULL;
    if (dir) snprintf(usr.path, sizeof usr.path, "%s/settings.conf", dir);
}

void zos_settings_move_user_keys(const char *dir)
{
    load(&sys);
    store_t *u = calloc(1, sizeof *u);
    snprintf(u->path, sizeof u->path, "%s/settings.conf", dir);
    load(u);
    int kept = 0;
    for (int i = 0; i < sys.n; i++) {
        if (is_system(sys.kv[i].key)) {
            sys.kv[kept++] = sys.kv[i];
        } else if (find(u, sys.kv[i].key) < 0) {
            set_in(u, sys.kv[i].key, sys.kv[i].val);
        }
    }
    sys.n = kept;
    save(u);
    save(&sys);
    free(u);
}
