#include <string.h>
#include "pb.h"

static bool rd_varint(pb_reader_t *r, uint64_t *out)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return false;
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

bool pb_next(pb_reader_t *r, pb_field_t *f)
{
    if (r->error || r->p >= r->end) return false;
    uint64_t key;
    if (!rd_varint(r, &key)) goto bad;
    f->field = (uint32_t)(key >> 3);
    f->wire = key & 7;
    f->data = NULL;
    f->len = 0;
    f->u = 0;
    switch (f->wire) {
    case PB_VARINT:
        if (!rd_varint(r, &f->u)) goto bad;
        return true;
    case PB_I64:
        if (r->end - r->p < 8) goto bad;
        for (int i = 7; i >= 0; i--) f->u = (f->u << 8) | r->p[i];
        r->p += 8;
        return true;
    case PB_I32:
        if (r->end - r->p < 4) goto bad;
        f->u = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 |
               (uint32_t)r->p[3] << 24;
        r->p += 4;
        return true;
    case PB_LEN: {
        uint64_t n;
        if (!rd_varint(r, &n) || n > (uint64_t)(r->end - r->p)) goto bad;
        f->data = r->p;
        f->len = (size_t)n;
        r->p += n;
        return true;
    }
    }
bad:
    r->error = true;
    return false;
}

float pb_float(const pb_field_t *f)
{
    uint32_t u = (uint32_t)f->u;
    float x;
    memcpy(&x, &u, 4);
    return x;
}

void pb_str(const pb_field_t *f, char *out, size_t cap)
{
    if (!cap) return;
    size_t n = f->len < cap - 1 ? f->len : cap - 1;
    if (f->data) memcpy(out, f->data, n);
    out[n] = 0;
}

static void put(pb_writer_t *w, const void *d, size_t n)
{
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, d, n);
    w->len += n;
}

static void put_raw_varint(pb_writer_t *w, uint64_t v)
{
    uint8_t b[10];
    int n = 0;
    do {
        b[n] = v & 0x7f;
        v >>= 7;
        if (v) b[n] |= 0x80;
        n++;
    } while (v);
    put(w, b, (size_t)n);
}

void pb_put_varint(pb_writer_t *w, uint32_t field, uint64_t v)
{
    put_raw_varint(w, (uint64_t)field << 3 | PB_VARINT);
    put_raw_varint(w, v);
}

void pb_put_fixed32(pb_writer_t *w, uint32_t field, uint32_t v)
{
    put_raw_varint(w, (uint64_t)field << 3 | PB_I32);
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(w, b, 4);
}

void pb_put_bytes(pb_writer_t *w, uint32_t field, const void *data, size_t len)
{
    put_raw_varint(w, (uint64_t)field << 3 | PB_LEN);
    put_raw_varint(w, len);
    put(w, data, len);
}

size_t pb_begin(pb_writer_t *w, uint32_t field)
{
    put_raw_varint(w, (uint64_t)field << 3 | PB_LEN);
    size_t mark = w->len;
    uint8_t two[2] = {0x80, 0x00};   /* placeholder: 2-byte varint */
    put(w, two, 2);
    return mark;
}

void pb_end(pb_writer_t *w, size_t mark)
{
    if (w->overflow) return;
    size_t n = w->len - mark - 2;
    if (n >= 1 << 14) {
        w->overflow = true;
        return;
    }
    /* Always a 2-byte varint (valid even for small n: 0x80|lo, hi). */
    w->buf[mark] = (uint8_t)(0x80 | (n & 0x7f));
    w->buf[mark + 1] = (uint8_t)(n >> 7);
}
