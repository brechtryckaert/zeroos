/* The panel backlight (see backlight.h). ZOS_BACKLIGHT names the sysfs
 * directory (tests). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backlight.h"
#include "settings.h"

static bool read_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

static bool backlight_dir(char *out, size_t n)
{
    const char *e = getenv("ZOS_BACKLIGHT");
    if (e) { snprintf(out, n, "%s", e); return true; }
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return false;
    struct dirent *de;
    bool ok = false;
    while ((de = readdir(d)))
        if (de->d_name[0] != '.') { snprintf(out, n, "/sys/class/backlight/%s", de->d_name); ok = true; break; }
    closedir(d);
    return ok;
}

int zos_backlight_get(void)
{
    char dir[256], p[300], v[32];
    if (!backlight_dir(dir, sizeof dir)) return -1;
    snprintf(p, sizeof p, "%s/max_brightness", dir);
    if (!read_line(p, v, sizeof v)) return -1;
    int max = atoi(v);
    snprintf(p, sizeof p, "%s/brightness", dir);
    if (!read_line(p, v, sizeof v) || max <= 0) return -1;
    return (atoi(v) * 100 + max / 2) / max;
}

bool zos_backlight_set(int pct)
{
    char dir[256], p[300], v[32];
    if (!backlight_dir(dir, sizeof dir)) return false;
    snprintf(p, sizeof p, "%s/max_brightness", dir);
    if (!read_line(p, v, sizeof v)) return false;
    int max = atoi(v);
    if (pct < 5) pct = 5;          /* never fully dark: there'd be no way back */
    if (pct > 100) pct = 100;
    snprintf(p, sizeof p, "%s/brightness", dir);
    FILE *f = fopen(p, "w");
    if (!f) return false;
    fprintf(f, "%d\n", (pct * max + 50) / 100);
    fclose(f);
    zos_setting_set_int("brightness", pct);
    return true;
}

