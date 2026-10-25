/* Blocking HTTP via libcurl, for use inside zos_task workers only. */
#ifndef ZOS_HTTP_H
#define ZOS_HTTP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    long status;        /* HTTP status, 0 if the request never completed */
    char *body;         /* NUL-terminated, malloc'd (may be NULL) */
    size_t len;
    char err[160];      /* transport error text when status == 0 */
} zos_http_resp_t;

typedef struct {
    const char *method;          /* "GET" if NULL */
    const char *url;
    const char *const *headers;  /* NULL-terminated list of "Name: value" */
    const char *body;            /* request body (JSON) or NULL */
    const char *unix_socket;     /* talk HTTP over a unix socket (LocalAPI) */
    const char *save_to;         /* write the body to this file instead (downloads) */
    long timeout_s;              /* 0 = 15 s */
} zos_http_req_t;

void zos_http_global_init(void);
bool zos_http(const zos_http_req_t *req, zos_http_resp_t *out);
void zos_http_free(zos_http_resp_t *r);
/* Percent-encode `s` for a query string; malloc'd. */
char *zos_url_escape(const char *s);

#endif
