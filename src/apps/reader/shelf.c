#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "lvgl.h"
#include "book.h"
#include "shelf.h"

static char *db_path, *conf_path;
static shelf_item_t *items;
static int n_items, cap_items;
static shelf_item_t **view;
static int n_view;
static reader_prefs_t prefs = {.em_idx = 2, .theme = 0};

reader_prefs_t *reader_prefs(void) { return &prefs; }

static char *dupstr(const char *s) { return lv_strdup(s ? s : ""); }

static void clean(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static shelf_item_t *find(const char *path)
{
    for (int i = 0; i < n_items; i++)
        if (strcmp(items[i].path, path) == 0) return &items[i];
    return NULL;
}

static shelf_item_t *add(const char *path)
{
    if (n_items == cap_items) {
        cap_items = cap_items ? cap_items * 2 : 32;
        items = lv_realloc(items, sizeof *items * cap_items);
        /* view pointers into the old array are stale now; rebuilt after scan */
    }
    shelf_item_t *it = &items[n_items++];
    memset(it, 0, sizeof *it);
    it->path = dupstr(path);
    it->title = dupstr("");
    it->author = dupstr("");
    return it;
}

void shelf_load(const char *state_dir)
{
    shelf_free();
    size_t n = strlen(state_dir) + 32;
    db_path = lv_malloc(n);
    conf_path = lv_malloc(n);
    snprintf(db_path, n, "%s/books.tsv", state_dir);
    snprintf(conf_path, n, "%s/reader.conf", state_dir);

    FILE *f = fopen(conf_path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "em_idx=%d", &prefs.em_idx);
            sscanf(line, "theme=%d", &prefs.theme);
        }
        fclose(f);
    }

    f = fopen(db_path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *fields[7] = {0};
        char *p = line;
        for (int i = 0; i < 7 && p; i++) {
            fields[i] = p;
            p = strchr(p, '\t');
            if (p) *p++ = 0;
        }
        if (!fields[6]) continue;   /* malformed / old line */
        shelf_item_t *it = add(fields[0]);
        it->mtime = atol(fields[1]);
        it->mark = strtoull(fields[2], NULL, 10);
        it->percent = atoi(fields[3]);
        it->last_opened = atol(fields[4]);
        lv_free(it->title);
        lv_free(it->author);
        it->title = dupstr(fields[5]);
        it->author = dupstr(fields[6]);
    }
    fclose(f);
}

void shelf_save(void)
{
    if (!db_path) return;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", db_path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        for (int i = 0; i < n_items; i++) {
            shelf_item_t *it = &items[i];
            fprintf(f, "%s\t%ld\t%llu\t%d\t%ld\t%s\t%s\n", it->path, it->mtime,
                    (unsigned long long)it->mark, it->percent, it->last_opened, it->title,
                    it->author);
        }
        fclose(f);
        rename(tmp, db_path);   /* atomic: a dying battery never leaves half a file */
    }
    f = fopen(conf_path, "w");
    if (f) {
        fprintf(f, "em_idx=%d\ntheme=%d\n", prefs.em_idx, prefs.theme);
        fclose(f);
    }
}

static bool is_book(const char *name)
{
    static const char *ext[] = {".epub", ".pdf", ".cbz", ".fb2", ".mobi", ".xps", ".xhtml", ".html"};
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (strcasecmp(dot, ext[i]) == 0) return true;
    return false;
}

static void title_from_filename(const char *path, char *out, size_t len)
{
    const char *base = strrchr(path, '/');
    snprintf(out, len, "%.200s", base ? base + 1 : path);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    for (char *p = out; *p; p++)
        if (*p == '_') *p = ' ';
}

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 3) scan_dir(path, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_book(e->d_name)) continue;

        shelf_item_t *it = find(path);
        if (!it) it = add(path);
        it->present = true;
        if (it->mtime != (long)st.st_mtime || !it->title[0]) {
            char title[256] = "", author[256] = "";
            book_peek(path, title, sizeof title, author, sizeof author);
            if (!title[0]) title_from_filename(path, title, sizeof title);
            clean(title);
            clean(author);
            lv_free(it->title);
            lv_free(it->author);
            it->title = dupstr(title);
            it->author = dupstr(author);
            if (it->mtime && it->mtime != (long)st.st_mtime) {
                it->mark = 0;          /* file changed: old bookmark may not fit */
                it->percent = 0;
            }
            it->mtime = (long)st.st_mtime;
        }
    }
    closedir(d);
}

static int cmp(const void *a, const void *b)
{
    const shelf_item_t *x = *(shelf_item_t *const *)a, *y = *(shelf_item_t *const *)b;
    if (x->last_opened != y->last_opened) return x->last_opened > y->last_opened ? -1 : 1;
    return strcasecmp(x->title, y->title);
}

void shelf_scan(const char *dir)
{
    for (int i = 0; i < n_items; i++) items[i].present = false;
    scan_dir(dir, 0);
    lv_free(view);
    view = lv_malloc(sizeof *view * (n_items ? n_items : 1));
    n_view = 0;
    for (int i = 0; i < n_items; i++)
        if (items[i].present) view[n_view++] = &items[i];
    qsort(view, n_view, sizeof *view, cmp);
    shelf_save();
}

int shelf_list(shelf_item_t ***out)
{
    *out = view;
    return n_view;
}

void shelf_free(void)
{
    for (int i = 0; i < n_items; i++) {
        lv_free(items[i].path);
        lv_free(items[i].title);
        lv_free(items[i].author);
    }
    lv_free(items);
    lv_free(view);
    lv_free(db_path);
    lv_free(conf_path);
    items = NULL;
    view = NULL;
    db_path = conf_path = NULL;
    n_items = cap_items = n_view = 0;
}
