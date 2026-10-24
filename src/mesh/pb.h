/* Just enough protobuf to speak the Meshtastic client API: a field reader
 * and a writer for varint, fixed32, float and length-delimited fields.
 * Field numbers live in mt_backend.c, next to the .proto names. */
#ifndef ZOS_PB_H
#define ZOS_PB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PB_VARINT = 0, PB_I64 = 1, PB_LEN = 2, PB_I32 = 5 };

typedef struct {
    const uint8_t *p, *end;
    bool error;
} pb_reader_t;

typedef struct {
    uint32_t field;
    uint8_t wire;
    uint64_t u;              /* VARINT, I32, I64 */
    const uint8_t *data;     /* LEN */
    size_t len;
} pb_field_t;

static inline pb_reader_t pb_reader(const uint8_t *data, size_t len)
{
    return (pb_reader_t){data, data + len, false};
}

bool pb_next(pb_reader_t *r, pb_field_t *f);
static inline pb_reader_t pb_sub(const pb_field_t *f) { return pb_reader(f->data, f->len); }
float pb_float(const pb_field_t *f);
/* Copy a LEN field into a NUL-terminated buffer (truncated if needed). */
void pb_str(const pb_field_t *f, char *out, size_t cap);

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool overflow;
} pb_writer_t;

static inline pb_writer_t pb_writer(uint8_t *buf, size_t cap) { return (pb_writer_t){buf, cap, 0, false}; }
void pb_put_varint(pb_writer_t *w, uint32_t field, uint64_t v);
void pb_put_fixed32(pb_writer_t *w, uint32_t field, uint32_t v);
void pb_put_bytes(pb_writer_t *w, uint32_t field, const void *data, size_t len);
/* Nested message: start returns a marker, end patches in the length. The
 * length prefix is reserved as 2 bytes, enough for any API frame (< 512). */
size_t pb_begin(pb_writer_t *w, uint32_t field);
void pb_end(pb_writer_t *w, size_t mark);

#endif
