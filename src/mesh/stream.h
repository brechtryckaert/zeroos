/* Non-blocking TCP client driven by an LVGL timer: connects, reconnects with
 * back-off, buffers writes, and hands received bytes to a callback. Nothing
 * here blocks the UI thread. */
#ifndef ZOS_STREAM_H
#define ZOS_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct zstream zstream_t;

typedef struct {
    void (*on_data)(void *user, const uint8_t *data, size_t len);
    void (*on_state)(void *user, bool connected);
} zstream_cbs_t;

zstream_t *zstream_new(const char *host, int port, zstream_cbs_t cbs, void *user);
void zstream_free(zstream_t *s);
bool zstream_connected(const zstream_t *s);
/* Queue bytes; returns false if not connected or the buffer is full. */
bool zstream_write(zstream_t *s, const void *data, size_t len);
/* Drop the connection now (it reconnects after the back-off). */
void zstream_reset(zstream_t *s);
/* "host:port", for messages */
const char *zstream_addr(const zstream_t *s);
/* Last error text, or "" */
const char *zstream_error(const zstream_t *s);

#endif
