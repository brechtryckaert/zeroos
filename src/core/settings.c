#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../zos.h"
#include "settings.h"

#define MAX_KEYS 64

static struct { char key[40]; char val[200]; } kv[MAX_KEYS];
static int n;
static bool loaded;

static void path_of(char *out, size_t cap) { snprintf(out, cap, "%s/settings.conf", zos_cfg.state_dir); }

static void load(void)
{
    if (loaded) return;
    loaded = true;
    char p[512];
    path_of(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[300];
    while (n < MAX_KEYS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        snprintf(kv[n].key, sizeof kv[n].key, "%s", line);
        snprintf(kv[n].val, sizeof kv[n].val, "%s", eq + 1);
        n++;
    }
    fclose(f);
}

static void save(void)
{
    char p[512], tmp[520];
    path_of(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s=%s\n", kv[i].key, kv[i].val);
    fclose(f);
    rename(tmp, p);
}

static int find(const char *key)
{
    load();
    for (int i = 0; i < n; i++)
        if (!strcmp(kv[i].key, key)) return i;
    return -1;
}

const char *zos_setting_str(const char *key, const char *def)
{
    int i = find(key);
    return i < 0 ? def : kv[i].val;
}

int zos_setting_int(const char *key, int def)
{
    const char *v = zos_setting_str(key, NULL);
    return v ? atoi(v) : def;
}

void zos_setting_set_str(const char *key, const char *v)
{
    int i = find(key);
    if (i < 0) {
        if (n >= MAX_KEYS) return;
        i = n++;
        snprintf(kv[i].key, sizeof kv[i].key, "%s", key);
    }
    snprintf(kv[i].val, sizeof kv[i].val, "%s", v);
    save();
}

void zos_setting_set_int(const char *key, int v)
{
    char b[24];
    snprintf(b, sizeof b, "%d", v);
    zos_setting_set_str(key, b);
}
