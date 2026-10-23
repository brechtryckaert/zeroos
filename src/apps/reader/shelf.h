/* The reader's library: which books exist, their metadata and where you were.
 * Persisted as a small TSV in the state dir so metadata is only read once per
 * file (re-read when the file's mtime changes). */
#ifndef ZOS_SHELF_H
#define ZOS_SHELF_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char *path;
    char *title;
    char *author;
    long mtime;
    uint64_t mark;     /* book_bookmark(); 0 = start */
    int percent;       /* last known progress, for the library list */
    long last_opened;  /* unix time, 0 = never */
    bool present;      /* found in the books dir during the last scan */
} shelf_item_t;

typedef struct {
    int em_idx;
    int theme;
} reader_prefs_t;

void shelf_load(const char *state_dir);
void shelf_save(void);
/* Scan dir (recursively, a few levels) and refresh metadata where needed. */
void shelf_scan(const char *dir);
/* Present books, most recently opened first. Pointer valid until next scan. */
int shelf_list(shelf_item_t ***out);
void shelf_free(void);

reader_prefs_t *reader_prefs(void);

#endif
