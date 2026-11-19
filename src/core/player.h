/* Media playback through mpv (a child process, controlled over its JSON
 * IPC socket). One player at a time.
 *
 * Video goes to the panel (mpv takes it over while ZeroOS stops drawing) or,
 * when a monitor is connected, to HDMI, with the panel left free for a
 * remote-control screen. Keys still arrive here and become mpv commands.
 * In the simulator mpv opens its own window. Audio-only playback keeps the
 * ZeroOS UI up and can run in the background (Music). */
#ifndef ZOS_PLAYER_H
#define ZOS_PLAYER_H

#include <stdbool.h>

typedef enum { ZOS_OUT_AUTO, ZOS_OUT_PANEL, ZOS_OUT_HDMI } zos_out_t;

#define ZOS_PLAYER_MAX_TRACKS 24

typedef struct {
    int id;                 /* mpv track id (per type) */
    char type[8];           /* "video", "audio", "sub" */
    char lang[12];
    char title[64];
    char codec[16];
    bool selected, external, def, forced;
} zos_track_t;

typedef struct {
    bool running;
    bool video;
    bool on_hdmi;
    bool paused;
    bool buffering;
    double pos, dur;          /* seconds; dur 0 if unknown */
    int volume;
    int playlist_pos, playlist_count;
    char title[160];
    zos_track_t tracks[ZOS_PLAYER_MAX_TRACKS];
    int ntracks;
} zos_player_state_t;

typedef struct {
    void (*on_change)(void *ud);           /* position, pause, ... (UI thread) */
    void (*on_end)(bool reached_end, double last_pos, void *ud);
} zos_player_cbs_t;

typedef struct {
    double start;             /* seconds into the (first) file */
    bool video;
    const char *title;        /* overrides the media title */
    zos_out_t out;            /* video only */
    const char *playlist;     /* path of a playlist file instead of a URL */
    int playlist_start;       /* entry to start at */
    const char *loop;         /* NULL, "file" or "playlist" */
} zos_player_opts_t;

bool zos_player_play(const char *url, const zos_player_opts_t *o, zos_player_cbs_t cbs, void *ud);
/* Older form: one URL, auto output. */
bool zos_player_start(const char *url, double start_s, bool video, const char *title,
                      zos_player_cbs_t cbs, void *ud);
void zos_player_toggle_pause(void);
void zos_player_toggle_mute(void);
/* Show text on the video (mpv's OSD) for ms milliseconds. */
void zos_player_osd(const char *text, int ms);
/* Called by hdmi.c when a monitor is plugged in or out. */
void zos_player_hdmi_changed(void);
void zos_player_seek(double rel_s);
void zos_player_seek_abs(double s);
/* Change the volume by delta and save it as the media volume. */
void zos_player_volume(int delta);
/* "playlist-next", "playlist-prev" ... a raw mpv command with no arguments. */
void zos_player_command(const char *cmd);
/* Set a property; value is JSON ("true", "3", "\"no\""). */
void zos_player_set(const char *prop, const char *json_value);
void zos_player_stop(void);
const zos_player_state_t *zos_player_state(void);
/* Where a video would play right now. */
zos_out_t zos_player_video_target(void);

#endif
