#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"

void zos_http_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static size_t sink(char *p, size_t sz, size_t n, void *ud)
{
    zos_http_resp_t *r = ud;
    size_t add = sz * n;
    if (r->len + add > (32u << 20)) return 0;   /* 32 MB cap: nothing we fetch is that big */
    char *nb = realloc(r->body, r->len + add + 1);
    if (!nb) return 0;
    memcpy(nb + r->len, p, add);
    r->body = nb;
    r->len += add;
    r->body[r->len] = 0;
    return add;
}

bool zos_http(const zos_http_req_t *q, zos_http_resp_t *r)
{
    memset(r, 0, sizeof *r);
    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(r->err, sizeof r->err, "curl init failed");
        return false;
    }
    struct curl_slist *hl = NULL;
    for (const char *const *h = q->headers; h && *h; h++) hl = curl_slist_append(hl, *h);
    if (q->body) hl = curl_slist_append(hl, "Content-Type: application/json");
    hl = curl_slist_append(hl, "Accept: application/json");

    FILE *out = NULL;
    char tmp[600] = "";
    if (q->save_to) {
        snprintf(tmp, sizeof tmp, "%s.part", q->save_to);
        out = fopen(tmp, "wb");
        if (!out) {
            snprintf(r->err, sizeof r->err, "cannot write %s", tmp);
            curl_slist_free_all(hl);
            curl_easy_cleanup(c);
            return false;
        }
    }

    curl_easy_setopt(c, CURLOPT_URL, q->url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, q->method ? q->method : "GET");
    if (q->body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, q->body);
    else if (q->method && strcmp(q->method, "POST") == 0) curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    if (q->unix_socket) curl_easy_setopt(c, CURLOPT_UNIX_SOCKET_PATH, q->unix_socket);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, q->timeout_s ? q->timeout_s : 15L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "ZeroOS/0.3");
    char ebuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, ebuf);
    if (out) curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    else {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
    }

    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r->status);
    else snprintf(r->err, sizeof r->err, "%s", ebuf[0] ? ebuf : curl_easy_strerror(rc));

    if (out) {
        fclose(out);
        if (rc == CURLE_OK && r->status >= 200 && r->status < 300) rename(tmp, q->save_to);
        else remove(tmp);
    }
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && r->status >= 200 && r->status < 300;
}

void zos_http_free(zos_http_resp_t *r)
{
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

char *zos_url_escape(const char *s)
{
    char *e = curl_easy_escape(NULL, s ? s : "", 0);
    char *o = strdup(e ? e : "");
    curl_free(e);
    return o;
}
