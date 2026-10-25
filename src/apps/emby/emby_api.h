/* Emby REST client. Calls block: run them in zos_task workers. */
#ifndef ZOS_EMBY_API_H
#define ZOS_EMBY_API_H

#include <stdbool.h>
#include <stdint.h>

#define EMBY_TICKS 10000000LL   /* 100 ns ticks per second */

typedef struct {
    char server[256];      /* "http://host:8096", no trailing slash */
    char user_id[64];
    char user_name[64];
    char token[128];
    char device_id[40];
    char server_name[96];
    int quality;           /* index into emby_qualities */
} emby_cfg_t;

typedef struct {
    char id[48];
    char name[128];
    char type[24];         /* Movie, Series, Season, Episode, MusicAlbum, Audio, CollectionFolder, Folder, ... */
    char collection[24];   /* CollectionType of views: movies, tvshows, music, ... */
    char media_type[12];   /* Video, Audio */
    char series[96];
    char artist[96];
    char overview[700];
    char rating[16];
    char image_tag[48];
    bool is_folder;
    bool played;
    int year, index, parent_index, child_count;
    int64_t runtime_ticks, pos_ticks;
} emby_item_t;

typedef struct {
    const char *name;
    int max_w, max_h, video_kbps;   /* max_w 0 = direct play */
} emby_quality_t;

extern const emby_quality_t emby_qualities[];
extern const int emby_quality_count;

/* ---- config (UI thread) ---- */
bool emby_cfg_load(emby_cfg_t *c);
void emby_cfg_save(const emby_cfg_t *c);
void emby_cfg_forget(void);

/* ---- network (workers) ---- */
bool emby_login(emby_cfg_t *c, const char *server, const char *user, const char *pass, char *err, int errlen);
/* path like "/Users/{uid}/Items?ParentId=..." ({uid} is substituted). Items
 * are malloc'd; *total gets TotalRecordCount. Returns -1 on error (err set),
 * -2 when the token was rejected (sign in again). */
int emby_get_items(const emby_cfg_t *c, const char *path, emby_item_t **out, int *total, char *err, int errlen);
/* Download a poster into the cache; returns the local path (malloc'd) or NULL. */
char *emby_fetch_image(const emby_cfg_t *c, const emby_item_t *it, int max_w, int max_h);
/* Playback check-ins: kind is "", "/Progress" or "/Stopped". */
void emby_report(const emby_cfg_t *c, const char *kind, const char *item_id, const char *play_session,
                 int64_t pos_ticks, bool paused, const char *method, const char *event);
void emby_stop_encodings(const emby_cfg_t *c, const char *play_session);

/* ---- helpers ---- */
/* Stream URL for an item at the configured quality (malloc'd). */
char *emby_stream_url(const emby_cfg_t *c, const emby_item_t *it, const char *play_session, const char **method);
bool emby_is_playable(const emby_item_t *it);
bool emby_is_video(const emby_item_t *it);

#endif
