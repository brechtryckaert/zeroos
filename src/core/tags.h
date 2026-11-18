/* Read song metadata (title, artist, album, track, embedded cover) from
 * MP3 (ID3v2.2-2.4, ID3v1), FLAC, Ogg Vorbis/Opus and MP4/M4A files.
 * Blocking file I/O: call from a worker thread. */
#ifndef ZOS_TAGS_H
#define ZOS_TAGS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char title[128], artist[96], album[96], album_artist[96];
    int track, disc, year;
    /* Embedded picture: where it starts in the file and how long it is (0 = none). */
    long pic_offset;
    size_t pic_len;
} zos_tags_t;

/* Returns false if the file could not be read; missing tags are left empty. */
bool zos_tags_read(const char *path, zos_tags_t *t);
/* Read the embedded picture bytes (malloc'd) found by zos_tags_read. */
void *zos_tags_picture(const char *path, const zos_tags_t *t, size_t *len);

#endif
