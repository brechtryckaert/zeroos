#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "tags.h"

#define MAX_TAG_BYTES (16u << 20)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint32_t syncsafe(const uint8_t *p) { return (uint32_t)(p[0] & 0x7f) << 21 | (uint32_t)(p[1] & 0x7f) << 14 | (uint32_t)(p[2] & 0x7f) << 7 | (p[3] & 0x7f); }

static void put_utf8(char **o, char *end, uint32_t cp)
{
    char *d = *o;
    if (cp < 0x80) { if (d + 1 < end) *d++ = (char)cp; }
    else if (cp < 0x800) { if (d + 2 < end) { *d++ = (char)(0xc0 | cp >> 6); *d++ = (char)(0x80 | (cp & 0x3f)); } }
    else if (cp < 0x10000) { if (d + 3 < end) { *d++ = (char)(0xe0 | cp >> 12); *d++ = (char)(0x80 | (cp >> 6 & 0x3f)); *d++ = (char)(0x80 | (cp & 0x3f)); } }
    else if (d + 4 < end) { *d++ = (char)(0xf0 | cp >> 18); *d++ = (char)(0x80 | (cp >> 12 & 0x3f)); *d++ = (char)(0x80 | (cp >> 6 & 0x3f)); *d++ = (char)(0x80 | (cp & 0x3f)); }
    *o = d;
}

/* Decode an ID3 text field (encoding byte already split off) into UTF-8. */
static void id3_text(int enc, const uint8_t *s, size_t n, char *out, size_t cap)
{
    char *o = out, *end = out + cap;
    if (enc == 0) {                         /* ISO-8859-1 */
        for (size_t i = 0; i < n && s[i]; i++) put_utf8(&o, end, s[i]);
    } else if (enc == 3) {                  /* UTF-8 */
        for (size_t i = 0; i < n && s[i] && o + 1 < end; i++) *o++ = (char)s[i];
    } else {                                /* UTF-16 with BOM (1) or big endian (2) */
        bool le = false;
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (s[0] == 0xff && s[1] == 0xfe) { le = true; i = 2; }
            else if (s[0] == 0xfe && s[1] == 0xff) i = 2;
        }
        for (; i + 1 < n; i += 2) {
            uint32_t u = le ? (uint32_t)(s[i] | s[i + 1] << 8) : (uint32_t)(s[i] << 8 | s[i + 1]);
            if (!u) break;
            if (u >= 0xd800 && u < 0xdc00 && i + 3 < n) {
                uint32_t lo = le ? (uint32_t)(s[i + 2] | s[i + 3] << 8) : (uint32_t)(s[i + 2] << 8 | s[i + 3]);
                u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                i += 2;
            }
            put_utf8(&o, end, u);
        }
    }
    *o = 0;
    /* ID3v2.4 allows several values separated by NUL; we keep the first. */
    for (char *p = out + strlen(out) - 1; p >= out && (*p == ' ' || *p == '\n'); p--) *p = 0;
}

static void num_pair(const char *s, int *a)
{
    if (s && *s) *a = atoi(s);
}

static void set_field(zos_tags_t *t, const char *key, const char *val)
{
    if (!*val) return;
    if (!strcasecmp(key, "TITLE")) snprintf(t->title, sizeof t->title, "%s", val);
    else if (!strcasecmp(key, "ARTIST")) { if (!t->artist[0]) snprintf(t->artist, sizeof t->artist, "%s", val); }
    else if (!strcasecmp(key, "ALBUM")) snprintf(t->album, sizeof t->album, "%s", val);
    else if (!strcasecmp(key, "ALBUMARTIST") || !strcasecmp(key, "ALBUM ARTIST")) snprintf(t->album_artist, sizeof t->album_artist, "%s", val);
    else if (!strcasecmp(key, "TRACKNUMBER")) num_pair(val, &t->track);
    else if (!strcasecmp(key, "DISCNUMBER")) num_pair(val, &t->disc);
    else if (!strcasecmp(key, "DATE") || !strcasecmp(key, "YEAR")) num_pair(val, &t->year);
}

/* ---------------- ID3v2 ---------------- */

static void id3v2(FILE *f, zos_tags_t *t)
{
    uint8_t h[10];
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return;
    int ver = h[3];
    uint32_t size = syncsafe(h + 6);
    if (size > MAX_TAG_BYTES || ver < 2 || ver > 4) return;
    uint8_t *buf = malloc(size);
    if (!buf || fread(buf, 1, size, f) != size) { free(buf); return; }
    bool unsync = h[5] & 0x80;
    size_t pos = 0;
    if (ver >= 3 && (h[5] & 0x40) && size >= 4) {             /* extended header */
        uint32_t es = ver == 4 ? syncsafe(buf) : be32(buf) + 4;
        pos = es;
    }
    int idlen = ver == 2 ? 3 : 4, hdrlen = ver == 2 ? 6 : 10;
    while (pos + (size_t)hdrlen <= size) {
        const uint8_t *fh = buf + pos;
        if (!fh[0]) break;                                       /* padding */
        uint32_t fsz = ver == 2 ? be24(fh + 3) : ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        if (fsz == 0 || pos + hdrlen + fsz > size) break;
        const uint8_t *d = fh + hdrlen;
        char id[5] = {0};
        memcpy(id, fh, (size_t)idlen);
        uint16_t flags = ver >= 3 ? (uint16_t)(fh[8] << 8 | fh[9]) : 0;
        bool skip = ver == 4 ? (flags & 0x000c) : ver == 3 ? (flags & 0x00c0) : false;   /* compressed/encrypted */
        if (!skip && (id[0] == 'T') && fsz > 1) {
            char v[200];
            id3_text(d[0], d + 1, fsz - 1, v, sizeof v);
            if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) snprintf(t->title, sizeof t->title, "%s", v);
            else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) snprintf(t->artist, sizeof t->artist, "%s", v);
            else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) snprintf(t->album_artist, sizeof t->album_artist, "%s", v);
            else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) snprintf(t->album, sizeof t->album, "%s", v);
            else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) num_pair(v, &t->track);
            else if (!strcmp(id, "TPOS") || !strcmp(id, "TPA")) num_pair(v, &t->disc);
            else if (!strcmp(id, "TYER") || !strcmp(id, "TDRC") || !strcmp(id, "TYE")) num_pair(v, &t->year);
        } else if (!skip && !unsync && !t->pic_len && (!strcmp(id, "APIC") || !strcmp(id, "PIC"))) {
            int enc = d[0];
            size_t i = 1;
            if (ver == 2) i += 3;                                /* image format "JPG" */
            else { while (i < fsz && d[i]) i++; i++; }           /* MIME type */
            int ptype = i < fsz ? d[i] : 0;
            i++;
            if (enc == 1 || enc == 2) { while (i + 1 < fsz && (d[i] || d[i + 1])) i += 2; i += 2; }
            else { while (i < fsz && d[i]) i++; i++; }           /* description */
            if (i < fsz && (ptype == 3 || ptype == 0 || !t->pic_len)) {
                t->pic_offset = 10 + (long)(pos + hdrlen + i);
                t->pic_len = fsz - i;
            }
        }
        pos += hdrlen + fsz;
    }
    free(buf);
}

static void id3v1(FILE *f, zos_tags_t *t)
{
    uint8_t b[128];
    if (fseek(f, -128, SEEK_END) || fread(b, 1, 128, f) != 128 || memcmp(b, "TAG", 3)) return;
    char v[64];
    if (!t->title[0]) { id3_text(0, b + 3, 30, v, sizeof v); snprintf(t->title, sizeof t->title, "%s", v); }
    if (!t->artist[0]) { id3_text(0, b + 33, 30, v, sizeof v); snprintf(t->artist, sizeof t->artist, "%s", v); }
    if (!t->album[0]) { id3_text(0, b + 63, 30, v, sizeof v); snprintf(t->album, sizeof t->album, "%s", v); }
    if (!t->year) { char y[5] = {0}; memcpy(y, b + 93, 4); t->year = atoi(y); }
    if (!t->track && b[125] == 0 && b[126]) t->track = b[126];
}

/* ---------------- Vorbis comments (FLAC, Ogg) ---------------- */

static void vorbis_comments(const uint8_t *p, size_t n, zos_tags_t *t)
{
    if (n < 8) return;
    uint32_t vl = le32(p);
    size_t i = 4 + vl;
    if (i + 4 > n) return;
    uint32_t cnt = le32(p + i);
    i += 4;
    for (uint32_t c = 0; c < cnt && i + 4 <= n; c++) {
        uint32_t l = le32(p + i);
        i += 4;
        if (i + l > n) break;
        char kv[256];
        size_t cl = l < sizeof kv - 1 ? l : sizeof kv - 1;
        memcpy(kv, p + i, cl);
        kv[cl] = 0;
        char *eq = strchr(kv, '=');
        if (eq) { *eq = 0; set_field(t, kv, eq + 1); }
        i += l;
    }
}

static void flac(FILE *f, zos_tags_t *t)
{
    uint8_t h[4];
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t bh[4];
        long at = ftell(f);
        if (fread(bh, 1, 4, f) != 4) return;
        int type = bh[0] & 0x7f;
        uint32_t len = be24(bh + 1);
        if (type == 4 && len < MAX_TAG_BYTES) {
            uint8_t *b = malloc(len);
            if (b && fread(b, 1, len, f) == len) vorbis_comments(b, len, t);
            free(b);
        } else if (type == 6 && !t->pic_len && len > 32) {
            uint8_t b[8];
            /* type(4) mime_len(4) mime desc_len(4) desc w h depth colors data_len(4) data */
            if (fread(b, 1, 8, f) == 8) {
                uint32_t ml = be32(b + 4);
                fseek(f, ml, SEEK_CUR);
                if (fread(b, 1, 4, f) == 4) {
                    uint32_t dl = be32(b);
                    fseek(f, dl + 16, SEEK_CUR);
                    if (fread(b, 1, 4, f) == 4) {
                        t->pic_len = be32(b);
                        t->pic_offset = ftell(f);
                    }
                }
            }
        }
        if (bh[0] & 0x80) return;
        fseek(f, at + 4 + (long)len, SEEK_SET);
    }
}

static void ogg(FILE *f, zos_tags_t *t)
{
    uint8_t buf[65536];
    fseek(f, 0, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf, f);
    if (n < 4 || memcmp(buf, "OggS", 4)) return;
    /* The comment packet follows "\x03vorbis" or "OpusTags"; usually inside the
     * first few pages. Page headers inside the packet are rare enough to ignore. */
    for (size_t i = 0; i + 8 < n; i++) {
        if (!memcmp(buf + i, "\x03vorbis", 7)) { vorbis_comments(buf + i + 7, n - i - 7, t); return; }
        if (!memcmp(buf + i, "OpusTags", 8)) { vorbis_comments(buf + i + 8, n - i - 8, t); return; }
    }
}

/* ---------------- MP4 / M4A ---------------- */

static void mp4_walk(FILE *f, long start, long end, int depth, zos_tags_t *t)
{
    long pos = start;
    while (pos + 8 <= end && depth < 8) {
        uint8_t h[8];
        fseek(f, pos, SEEK_SET);
        if (fread(h, 1, 8, f) != 8) return;
        uint64_t sz = be32(h);
        long hdr = 8;
        if (sz == 1) {
            uint8_t x[8];
            if (fread(x, 1, 8, f) != 8) return;
            sz = (uint64_t)be32(x) << 32 | be32(x + 4);
            hdr = 16;
        } else if (sz == 0) {
            sz = (uint64_t)(end - pos);
        }
        if (sz < (uint64_t)hdr || pos + (long)sz > end) return;
        char type[5] = {0};
        memcpy(type, h + 4, 4);
        long body = pos + hdr, bend = pos + (long)sz;
        if (!strcmp(type, "moov") || !strcmp(type, "udta") || !strcmp(type, "ilst")) {
            mp4_walk(f, body, bend, depth + 1, t);
        } else if (!strcmp(type, "meta")) {
            mp4_walk(f, body + 4, bend, depth + 1, t);           /* version+flags */
        } else if (depth >= 3) {                                 /* an ilst item: data atom inside */
            uint8_t d[16];
            fseek(f, body, SEEK_SET);
            if (fread(d, 1, 16, f) == 16 && !memcmp(d + 4, "data", 4)) {
                uint32_t dl = be32(d);
                long dstart = body + 16;
                long dlen = (long)dl - 16;
                if (dlen > 0 && dstart + dlen <= bend) {
                    if (!memcmp(type, "covr", 4)) {
                        if (!t->pic_len) { t->pic_offset = dstart; t->pic_len = (size_t)dlen; }
                    } else if (!memcmp(type, "trkn", 4) || !memcmp(type, "disk", 4)) {
                        uint8_t nb[4];
                        if (dlen >= 4 && fread(nb, 1, 4, f) == 4) {
                            int v = nb[2] << 8 | nb[3];
                            if (type[0] == 't') t->track = v; else t->disc = v;
                        }
                    } else {
                        char v[200];
                        size_t l = (size_t)dlen < sizeof v - 1 ? (size_t)dlen : sizeof v - 1;
                        if (fread(v, 1, l, f) == l) {
                            v[l] = 0;
                            if (!memcmp(type, "\xa9nam", 4)) set_field(t, "TITLE", v);
                            else if (!memcmp(type, "\xa9" "ART", 4)) set_field(t, "ARTIST", v);
                            else if (!memcmp(type, "aART", 4)) set_field(t, "ALBUMARTIST", v);
                            else if (!memcmp(type, "\xa9" "alb", 4)) set_field(t, "ALBUM", v);
                            else if (!memcmp(type, "\xa9" "day", 4)) set_field(t, "DATE", v);
                        }
                    }
                }
            }
        }
        pos = bend;
    }
}

static void mp4(FILE *f, zos_tags_t *t)
{
    uint8_t h[8];
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, 8, f) != 8 || memcmp(h + 4, "ftyp", 4)) return;
    fseek(f, 0, SEEK_END);
    mp4_walk(f, 0, ftell(f), 0, t);
}

bool zos_tags_read(const char *path, zos_tags_t *t)
{
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t m[12] = {0};
    size_t n = fread(m, 1, sizeof m, f);
    if (n >= 3 && !memcmp(m, "ID3", 3)) { id3v2(f, t); id3v1(f, t); }
    else if (n >= 4 && !memcmp(m, "fLaC", 4)) flac(f, t);
    else if (n >= 4 && !memcmp(m, "OggS", 4)) ogg(f, t);
    else if (n >= 8 && !memcmp(m + 4, "ftyp", 4)) mp4(f, t);
    else id3v1(f, t);
    fclose(f);
    return true;
}

void *zos_tags_picture(const char *path, const zos_tags_t *t, size_t *len)
{
    *len = 0;
    if (!t->pic_len || t->pic_len > MAX_TAG_BYTES) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    void *b = malloc(t->pic_len);
    if (b && (fseek(f, t->pic_offset, SEEK_SET) || fread(b, 1, t->pic_len, f) != t->pic_len)) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) *len = t->pic_len;
    return b;
}
