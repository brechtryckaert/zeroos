#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "lvgl.h"
#include "stream.h"

#define WBUF_SIZE 8192

typedef enum { S_IDLE, S_CONNECTING, S_UP } sstate_t;

struct zstream {
    char host[64];
    int port;
    char addr[80];
    char err[96];
    zstream_cbs_t cbs;
    void *user;
    int fd;
    sstate_t st;
    uint32_t retry_at, backoff;
    uint8_t wbuf[WBUF_SIZE];
    size_t wlen;
    lv_timer_t *timer;
};

static void set_err(zstream_t *s, const char *what, int e)
{
    snprintf(s->err, sizeof s->err, "%s: %s", what, strerror(e));
}

static void drop(zstream_t *s)
{
    bool was_up = s->st == S_UP;
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    s->st = S_IDLE;
    s->wlen = 0;
    s->retry_at = lv_tick_get() + s->backoff;
    s->backoff = s->backoff < 8000 ? s->backoff * 2 : 10000;
    if (was_up && s->cbs.on_state) s->cbs.on_state(s->user, false);
}

static void start_connect(zstream_t *s)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
    char port[8];
    snprintf(port, sizeof port, "%d", s->port);
    int rc = getaddrinfo(s->host, port, &hints, &res);   /* numeric hosts: no DNS wait */
    if (rc != 0 || !res) {
        snprintf(s->err, sizeof s->err, "resolve: %s", gai_strerror(rc));
        drop(s);
        return;
    }
    s->fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (s->fd < 0) {
        set_err(s, "socket", errno);
        freeaddrinfo(res);
        drop(s);
        return;
    }
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    rc = connect(s->fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc == 0 || errno == EINPROGRESS) {
        s->st = S_CONNECTING;
        s->retry_at = lv_tick_get() + 4000;   /* connect timeout */
    } else {
        set_err(s, "connect", errno);
        drop(s);
    }
}

static void flush(zstream_t *s)
{
    while (s->wlen) {
        ssize_t n = send(s->fd, s->wbuf, s->wlen, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            set_err(s, "send", errno);
            drop(s);
            return;
        }
        memmove(s->wbuf, s->wbuf + n, s->wlen - (size_t)n);
        s->wlen -= (size_t)n;
    }
}

static void poll_cb(lv_timer_t *t)
{
    zstream_t *s = lv_timer_get_user_data(t);
    uint32_t now = lv_tick_get();
    switch (s->st) {
    case S_IDLE:
        if ((int32_t)(now - s->retry_at) >= 0) start_connect(s);
        break;
    case S_CONNECTING: {
        int err = 0;
        socklen_t el = sizeof err;
        struct sockaddr_storage peer;
        socklen_t pl = sizeof peer;
        if (getpeername(s->fd, (struct sockaddr *)&peer, &pl) == 0) {
            s->st = S_UP;
            s->backoff = 1000;
            s->err[0] = 0;
            if (s->cbs.on_state) s->cbs.on_state(s->user, true);
        } else if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err) {
            set_err(s, "connect", err);
            drop(s);
        } else if ((int32_t)(now - s->retry_at) >= 0) {
            snprintf(s->err, sizeof s->err, "connect: timed out");
            drop(s);
        }
        break;
    }
    case S_UP: {
        uint8_t buf[2048];
        for (int i = 0; i < 8 && s->st == S_UP; i++) {
            ssize_t n = recv(s->fd, buf, sizeof buf, 0);
            if (n > 0) {
                if (s->cbs.on_data) s->cbs.on_data(s->user, buf, (size_t)n);
                continue;
            }
            if (n == 0) {
                snprintf(s->err, sizeof s->err, "connection closed");
                drop(s);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                set_err(s, "recv", errno);
                drop(s);
            }
            break;
        }
        if (s->st == S_UP) flush(s);
        break;
    }
    }
}

zstream_t *zstream_new(const char *host, int port, zstream_cbs_t cbs, void *user)
{
    zstream_t *s = lv_zalloc(sizeof *s);
    snprintf(s->host, sizeof s->host, "%s", host);
    s->port = port;
    snprintf(s->addr, sizeof s->addr, "%s:%d", host, port);
    s->cbs = cbs;
    s->user = user;
    s->fd = -1;
    s->backoff = 1000;
    s->retry_at = lv_tick_get();
    s->timer = lv_timer_create(poll_cb, 20, s);
    return s;
}

void zstream_free(zstream_t *s)
{
    if (!s) return;
    lv_timer_delete(s->timer);
    if (s->fd >= 0) close(s->fd);
    lv_free(s);
}

bool zstream_connected(const zstream_t *s) { return s && s->st == S_UP; }

bool zstream_write(zstream_t *s, const void *data, size_t len)
{
    if (!s || s->st != S_UP || s->wlen + len > WBUF_SIZE) return false;
    memcpy(s->wbuf + s->wlen, data, len);
    s->wlen += len;
    flush(s);
    return true;
}

void zstream_reset(zstream_t *s)
{
    if (s && s->fd >= 0) {
        s->backoff = 500;
        drop(s);
    }
}

const char *zstream_addr(const zstream_t *s) { return s->addr; }
const char *zstream_error(const zstream_t *s) { return s->err; }
