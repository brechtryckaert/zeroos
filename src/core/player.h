/* Media playback through mpv (a child process, controlled over its JSON
 * IPC socket). One player at a time.
 *
 * On the device, video goes straight to the panel (DRM) while ZeroOS stops
 * drawing; keys still arrive here and become mpv commands. In the simulator
 * mpv opens its own window. Audio-only playback keeps the ZeroOS UI up. */
#ifndef ZOS_PLAYER_H
#define ZOS_PLAYER_H

#include <stdbool.h>

typedef struct {
    bool running;
    bool video;
    bool paused;
    bool buffering;
    double pos, dur;          /* seconds; dur 0 if unknown */
    int volume;
    char title[160];
} zos_player_state_t;

typedef struct {
    void (*on_change)(void *ud);           /* position, pause, ... (UI thread) */
    void (*on_end)(bool reached_end, double last_pos, void *ud);
} zos_player_cbs_t;

bool zos_player_start(const char *url, double start_s, bool video, const char *title,
                      zos_player_cbs_t cbs, void *ud);
void zos_player_toggle_pause(void);
void zos_player_seek(double rel_s);
void zos_player_volume(int delta);
void zos_player_stop(void);
const zos_player_state_t *zos_player_state(void);

#endif
