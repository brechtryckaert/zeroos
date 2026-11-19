/* Keys that work everywhere (see hotkeys.h).
 *
 *   volume / mute   what's playing (video or music, the saved media volume),
 *                   otherwise the system volume (ALSA, like Settings)
 *   brightness      the panel backlight
 *   play / pause    the video or music that's playing
 *   next / previous music here; the Video app takes them itself (next
 *                   episode), so they go through to it while a video plays
 *
 * Feedback goes where it can be seen: mpv's on-screen display while a video
 * fills the panel, a ZeroOS banner otherwise. */
#include "hotkeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../apps/music/music.h"
#include "../zos.h"
#include "backlight.h"
#include "player.h"
#include "proc.h"
#include "settings.h"
#include "task.h"

static const char owner_tag;

static bool video_playing(void)
{
    const zos_player_state_t *st = zos_player_state();
    return st->running && st->video;
}

/* While a video fills the panel ZeroOS draws nothing: say it through mpv. */
static void feedback(const char *title, const char *text)
{
    if (video_playing()) {
        char t[96];
        snprintf(t, sizeof t, "%s", text);
        zos_player_osd(t, 1200);
        if (zos_on_device() && !zos_player_state()->on_hdmi) return;
    }
    zos_notify(0x52525b, title, text);
}

/* ---------------- system volume (ALSA) ---------------- */

typedef struct {
    int delta;        /* +-5, or 0 = mute toggle */
    int vol;          /* after */
    bool muted, ok;
} sysvol_t;

static char mixer[48];

static void find_mixer(void)
{
    if (mixer[0]) return;
    const char *env = getenv("ZOS_MIXER");
    if (env) { snprintf(mixer, sizeof mixer, "%s", env); return; }
    char out[2048] = "";
    const char *argv[] = {"amixer", "scontrols", NULL};
    zos_run(argv, out, sizeof out, 5000);
    static const char *pref[] = {"Master", "PCM", "Speaker", "Headphone", "DAC", "Digital", "Playback"};
    for (size_t i = 0; i < sizeof pref / sizeof pref[0] && !mixer[0]; i++) {
        char needle[64];
        snprintf(needle, sizeof needle, "'%s'", pref[i]);
        if (strstr(out, needle)) snprintf(mixer, sizeof mixer, "%s", pref[i]);
    }
    if (!mixer[0]) {
        char *q = strchr(out, '\'');
        char *e = q ? strchr(q + 1, '\'') : NULL;
        if (q && e) snprintf(mixer, sizeof mixer, "%.*s", (int)(e - q - 1), q + 1);
    }
}

static void sysvol_work(void *a)
{
    sysvol_t *s = a;
    find_mixer();
    if (!mixer[0]) return;
    char v[16], out[1024] = "";
    if (s->delta) snprintf(v, sizeof v, "%d%%%c", abs(s->delta), s->delta > 0 ? '+' : '-');
    else snprintf(v, sizeof v, "toggle");
    const char *set[] = {"amixer", "-M", "sset", mixer, v, NULL};
    if (zos_run(set, out, sizeof out, 5000) != 0) return;
    char *p = strchr(out, '[');
    while (p && !strchr(p, '%')) p = strchr(p + 1, '[');
    s->vol = p ? atoi(p + 1) : -1;
    s->muted = strstr(out, "[off]") != NULL;
    s->ok = true;
}

static void sysvol_done(void *a)
{
    sysvol_t *s = a;
    char t[64];
    if (!s->ok) snprintf(t, sizeof t, "No volume control found");
    else if (s->muted) snprintf(t, sizeof t, "Muted");
    else snprintf(t, sizeof t, "%d%%", s->vol);
    zos_notify(0x52525b, "Volume", t);
}

static void sysvol_release(void *a) { free(a); }

static void system_volume(int delta)
{
    sysvol_t *s = calloc(1, sizeof *s);
    s->delta = delta;
    zos_task_run(&owner_tag, sysvol_work, sysvol_done, sysvol_release, s);
}

/* ---------------- keys ---------------- */

bool zos_hotkey(uint32_t k)
{
    const zos_player_state_t *st = zos_player_state();
    char t[64];
    switch (k) {
    case ZOS_KEY_VOL_UP: case ZOS_KEY_VOL_DOWN: {
        int d = k == ZOS_KEY_VOL_UP ? 5 : -5;
        if (st->running) {
            zos_player_volume(d);          /* mpv shows a bar for videos */
            if (!video_playing()) {
                snprintf(t, sizeof t, "%d%%", zos_setting_int("volume", 80));
                zos_notify(0x52525b, "Volume", t);
            }
        } else {
            system_volume(d);
        }
        return true;
    }
    case ZOS_KEY_MUTE:
        if (st->running) {
            zos_player_toggle_mute();
            if (!video_playing()) zos_notify(0x52525b, "Volume", "Mute on / off");
        } else {
            system_volume(0);
        }
        return true;
    case ZOS_KEY_BRIGHT_UP: case ZOS_KEY_BRIGHT_DOWN: {
        int b = zos_backlight_get();
        if (b < 0) { zos_notify(0x52525b, "Brightness", "No backlight control found"); return true; }
        zos_backlight_set(b + (k == ZOS_KEY_BRIGHT_UP ? 10 : -10));
        snprintf(t, sizeof t, "Brightness %d%%", zos_backlight_get());
        feedback("Display", t);
        return true;
    }
    case ZOS_KEY_PLAYPAUSE:
        if (st->running) zos_player_toggle_pause();
        return true;
    case ZOS_KEY_NEXT: case ZOS_KEY_PREV:
        if (video_playing()) return false;          /* the Video app: next / previous episode */
        if (music_active()) {
            if (k == ZOS_KEY_NEXT) music_next();
            else music_prev();
            return true;
        }
        return false;
    default:
        return false;
    }
}
