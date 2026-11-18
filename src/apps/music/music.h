/* Music library and background playback (shared by the Music app and the
 * status bar). The library is ~/Music, scanned recursively; tags are cached
 * in the state directory so later scans only read new or changed files. */
#ifndef ZOS_MUSIC_H
#define ZOS_MUSIC_H

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

typedef struct {
    char *path, *title, *artist, *album;
    int track, disc, year;
    int album_idx;
    long pic_off;
    size_t pic_len;
} mtrack_t;

typedef struct {
    char *name, *artist, *dir;
    int year;
    int *tracks;        /* track indexes, in disc/track order */
    int n;
    int art_track;      /* track with an embedded picture, or -1 */
    char *cover_file;   /* cover.jpg & co. in the folder, or NULL */
} malbum_t;

typedef struct {
    char *name;
    int *albums;
    int na, ntracks;
} martist_t;

typedef struct {
    mtrack_t *t;
    int nt;
    malbum_t *al;
    int nal;
    martist_t *ar;
    int nar;
    int *songs;         /* all tracks, by artist / album / disc / track */
} mlib_t;

/* Current library (empty until the first scan finishes). */
const mlib_t *music_lib(void);
bool music_scanning(void);
/* Scan in the background; `done` runs on the UI thread afterwards. */
void music_scan(void (*done)(void));

/* Album covers, decoded in the background and cached. Returns NULL until
 * ready; `ready` is called (UI thread) when one arrives. size: 26 or 84. */
const lv_image_dsc_t *music_cover(int album, int size);
void music_set_cover_ready_cb(void (*ready)(void));

/* ---- playback ---- */
typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } repeat_t;

/* Play `tracks` (library indexes) starting at entry `start`. */
void music_play(const int *tracks, int n, int start, bool shuffle);
bool music_active(void);              /* something queued (playing or paused) */
int music_current(void);              /* library index of the current track, or -1 */
const int *music_queue(int *n, int *pos);
void music_jump(int queue_pos);
void music_next(void);
void music_prev(void);
void music_stop(void);
bool music_shuffle(void);
void music_toggle_shuffle(void);
repeat_t music_repeat(void);
void music_cycle_repeat(void);
void music_volume(int delta);
/* Called on any playback change (UI thread). */
void music_set_change_cb(void (*cb)(void));

#endif
