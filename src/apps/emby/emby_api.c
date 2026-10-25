#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../core/http.h"
#include "../../zos.h"
#include "emby_api.h"

/* The CardputerZero decodes H.264 in hardware; transcoding on the server to a
 * small H.264 stream is what makes most libraries playable on it. */
const emby_quality_t emby_qualities[] = {
    {"480p", 854, 480, 1500},
    {"360p", 640, 360, 800},
    {"720p", 1280, 720, 3000},
    {"Original", 0, 0, 0},
};
const int emby_quality_count = sizeof emby_qualities / sizeof emby_qualities[0];

/* ---------------- config ---------------- */

static void cfg_path(char *out, size_t n) { snprintf(out, n, "%s/emby.json", zos_cfg.state_dir); }

static void jcopy(char *dst, size_t cap, const cJSON *j)
{
    dst[0] = 0;
    if (cJSON_IsString(j) && j->valuestring) snprintf(dst, cap, "%s", j->valuestring);
}

bool emby_cfg_load(emby_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    char p[512];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        char buf[2048];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        cJSON *j = cJSON_Parse(buf);
        if (j) {
            jcopy(c->server, sizeof c->server, cJSON_GetObjectItem(j, "server"));
            jcopy(c->user_id, sizeof c->user_id, cJSON_GetObjectItem(j, "user_id"));
            jcopy(c->user_name, sizeof c->user_name, cJSON_GetObjectItem(j, "user_name"));
            jcopy(c->token, sizeof c->token, cJSON_GetObjectItem(j, "token"));
            jcopy(c->device_id, sizeof c->device_id, cJSON_GetObjectItem(j, "device_id"));
            jcopy(c->server_name, sizeof c->server_name, cJSON_GetObjectItem(j, "server_name"));
            const cJSON *q = cJSON_GetObjectItem(j, "quality");
            c->quality = cJSON_IsNumber(q) ? q->valueint % emby_quality_count : 0;
            cJSON_Delete(j);
        }
    }
    if (!c->device_id[0]) {   /* stable per device, so the server lists one ZeroOS */
        for (int i = 0; i < 32; i++) c->device_id[i] = "0123456789abcdef"[lv_rand(0, 15)];
        c->device_id[32] = 0;
    }
    return c->token[0] && c->user_id[0] && c->server[0];
}

void emby_cfg_save(const emby_cfg_t *c)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "server", c->server);
    cJSON_AddStringToObject(j, "user_id", c->user_id);
    cJSON_AddStringToObject(j, "user_name", c->user_name);
    cJSON_AddStringToObject(j, "token", c->token);
    cJSON_AddStringToObject(j, "device_id", c->device_id);
    cJSON_AddStringToObject(j, "server_name", c->server_name);
    cJSON_AddNumberToObject(j, "quality", c->quality);
    char *s = cJSON_Print(j);
    char p[512], tmp[520];
    cfg_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (f) {
        chmod(tmp, 0600);   /* holds an access token */
        fputs(s, f);
        fclose(f);
        rename(tmp, p);
    }
    free(s);
    cJSON_Delete(j);
}

void emby_cfg_forget(void)
{
    emby_cfg_t c;
    emby_cfg_load(&c);
    c.token[0] = c.user_id[0] = 0;
    emby_cfg_save(&c);
}

/* ---------------- requests ---------------- */

static void auth_header(const emby_cfg_t *c, char *out, size_t n)
{
    char host[64] = "CardputerZero";
    gethostname(host, sizeof host);
    snprintf(out, n, "X-Emby-Authorization: Emby %s%s%sClient=\"ZeroOS\", Device=\"%s\", DeviceId=\"%s\", Version=\"0.4\"",
             c->user_id[0] ? "UserId=\"" : "", c->user_id, c->user_id[0] ? "\", " : "", host, c->device_id);
}

static bool request(const emby_cfg_t *c, const char *method, const char *path, const char *body, zos_http_resp_t *r)
{
    char url[1024], auth[400], tok[200];
    snprintf(url, sizeof url, "%s%s", c->server, path);
    auth_header(c, auth, sizeof auth);
    snprintf(tok, sizeof tok, "X-Emby-Token: %s", c->token);
    const char *hdr[] = {auth, c->token[0] ? tok : NULL, NULL};
    zos_http_req_t q = {.method = method, .url = url, .headers = hdr, .body = body, .timeout_s = 20};
    return zos_http(&q, r);
}

static void normalize_server(char *out, size_t n, const char *in)
{
    while (*in == ' ') in++;
    if (strncmp(in, "http://", 7) && strncmp(in, "https://", 8)) snprintf(out, n, "http://%s", in);
    else snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '/' || out[l - 1] == ' ')) out[--l] = 0;
    /* a bare host: Emby's default port */
    const char *host = strstr(out, "://") + 3;
    if (!strchr(host, ':') && !strchr(host, '/') && l + 6 < n) strcat(out, ":8096");
}

bool emby_login(emby_cfg_t *c, const char *server, const char *user, const char *pass, char *err, int errlen)
{
    normalize_server(c->server, sizeof c->server, server);
    c->token[0] = c->user_id[0] = 0;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "Username", user);
    cJSON_AddStringToObject(b, "Pw", pass);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    zos_http_resp_t r;
    bool ok = request(c, "POST", "/Users/AuthenticateByName", body, &r);
    free(body);
    if (!ok) {
        if (r.status == 401) snprintf(err, (size_t)errlen, "Wrong username or password.");
        else if (r.status) snprintf(err, (size_t)errlen, "Server said %ld. Is this an Emby server?", r.status);
        else snprintf(err, (size_t)errlen, "Can't reach %s: %s", c->server, r.err);
        zos_http_free(&r);
        return false;
    }
    cJSON *j = cJSON_Parse(r.body);
    zos_http_free(&r);
    const cJSON *u = j ? cJSON_GetObjectItem(j, "User") : NULL;
    jcopy(c->token, sizeof c->token, j ? cJSON_GetObjectItem(j, "AccessToken") : NULL);
    jcopy(c->user_id, sizeof c->user_id, u ? cJSON_GetObjectItem(u, "Id") : NULL);
    jcopy(c->user_name, sizeof c->user_name, u ? cJSON_GetObjectItem(u, "Name") : NULL);
    cJSON_Delete(j);
    if (!c->token[0] || !c->user_id[0]) {
        snprintf(err, (size_t)errlen, "Unexpected answer from the server.");
        return false;
    }
    /* friendly server name for the header; optional */
    if (request(c, "GET", "/System/Info/Public", NULL, &r)) {
        cJSON *info = cJSON_Parse(r.body);
        if (info) jcopy(c->server_name, sizeof c->server_name, cJSON_GetObjectItem(info, "ServerName"));
        cJSON_Delete(info);
    }
    zos_http_free(&r);
    return true;
}

static int jint(const cJSON *j, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static int64_t jticks(const cJSON *j, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

static void parse_item(emby_item_t *it, const cJSON *j)
{
    memset(it, 0, sizeof *it);
    jcopy(it->id, sizeof it->id, cJSON_GetObjectItem(j, "Id"));
    jcopy(it->name, sizeof it->name, cJSON_GetObjectItem(j, "Name"));
    jcopy(it->type, sizeof it->type, cJSON_GetObjectItem(j, "Type"));
    jcopy(it->collection, sizeof it->collection, cJSON_GetObjectItem(j, "CollectionType"));
    jcopy(it->media_type, sizeof it->media_type, cJSON_GetObjectItem(j, "MediaType"));
    jcopy(it->series, sizeof it->series, cJSON_GetObjectItem(j, "SeriesName"));
    jcopy(it->artist, sizeof it->artist, cJSON_GetObjectItem(j, "AlbumArtist"));
    jcopy(it->overview, sizeof it->overview, cJSON_GetObjectItem(j, "Overview"));
    jcopy(it->rating, sizeof it->rating, cJSON_GetObjectItem(j, "OfficialRating"));
    const cJSON *tags = cJSON_GetObjectItem(j, "ImageTags");
    if (tags) jcopy(it->image_tag, sizeof it->image_tag, cJSON_GetObjectItem(tags, "Primary"));
    it->is_folder = cJSON_IsTrue(cJSON_GetObjectItem(j, "IsFolder"));
    it->year = jint(j, "ProductionYear");
    it->index = jint(j, "IndexNumber");
    it->parent_index = jint(j, "ParentIndexNumber");
    it->child_count = jint(j, "ChildCount");
    it->runtime_ticks = jticks(j, "RunTimeTicks");
    const cJSON *ud = cJSON_GetObjectItem(j, "UserData");
    if (ud) {
        it->pos_ticks = jticks(ud, "PlaybackPositionTicks");
        it->played = cJSON_IsTrue(cJSON_GetObjectItem(ud, "Played"));
    }
}

int emby_get_items(const emby_cfg_t *c, const char *path, emby_item_t **out, int *total, char *err, int errlen)
{
    *out = NULL;
    if (total) *total = 0;
    char p[1024];
    const char *uid = strstr(path, "{uid}");
    if (uid) snprintf(p, sizeof p, "%.*s%s%s", (int)(uid - path), path, c->user_id, uid + 5);
    else snprintf(p, sizeof p, "%s", path);
    zos_http_resp_t r;
    if (!request(c, "GET", p, NULL, &r)) {
        int rc = r.status == 401 ? -2 : -1;
        if (r.status) snprintf(err, (size_t)errlen, "Server error %ld", r.status);
        else snprintf(err, (size_t)errlen, "%s", r.err);
        zos_http_free(&r);
        return rc;
    }
    cJSON *j = cJSON_Parse(r.body);
    zos_http_free(&r);
    if (!j) {
        snprintf(err, (size_t)errlen, "Unreadable reply from the server");
        return -1;
    }
    /* Most endpoints wrap results as {Items, TotalRecordCount}; /Latest returns a bare array. */
    const cJSON *arr = cJSON_IsArray(j) ? j : cJSON_GetObjectItem(j, "Items");
    int n = cJSON_GetArraySize(arr);
    if (n > 0) {
        *out = calloc((size_t)n, sizeof **out);
        int i = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, arr) parse_item(&(*out)[i++], e);
    }
    if (total) {
        const cJSON *t = cJSON_IsArray(j) ? NULL : cJSON_GetObjectItem(j, "TotalRecordCount");
        *total = cJSON_IsNumber(t) ? t->valueint : n;
    }
    cJSON_Delete(j);
    return n;
}

char *emby_fetch_image(const emby_cfg_t *c, const emby_item_t *it, int max_w, int max_h)
{
    if (!it->image_tag[0]) return NULL;
    char dir[512], path[640];
    snprintf(dir, sizeof dir, "%s/emby-img", zos_cfg.state_dir);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s-%.24s-%dx%d.jpg", dir, it->id, it->image_tag, max_w, max_h);
    if (access(path, R_OK) == 0) return strdup(path);
    char url[1024];
    /* format=jpg + quality: baseline JPEG for LVGL's decoder; tag makes it cacheable */
    snprintf(url, sizeof url, "%s/Items/%s/Images/Primary?MaxWidth=%d&MaxHeight=%d&tag=%s&format=jpg&quality=85&api_key=%s",
             c->server, it->id, max_w, max_h, it->image_tag, c->token);
    zos_http_req_t q = {.url = url, .save_to = path, .timeout_s = 20};
    zos_http_resp_t r;
    bool ok = zos_http(&q, &r);
    zos_http_free(&r);
    return ok ? strdup(path) : NULL;
}

void emby_report(const emby_cfg_t *c, const char *kind, const char *item_id, const char *play_session,
                 int64_t pos_ticks, bool paused, const char *method, const char *event)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "ItemId", item_id);
    cJSON_AddStringToObject(b, "MediaSourceId", item_id);
    cJSON_AddStringToObject(b, "PlaySessionId", play_session);
    cJSON_AddNumberToObject(b, "PositionTicks", (double)pos_ticks);
    cJSON_AddBoolToObject(b, "IsPaused", paused);
    cJSON_AddBoolToObject(b, "CanSeek", true);
    cJSON_AddStringToObject(b, "PlayMethod", method);
    if (event) cJSON_AddStringToObject(b, "EventName", event);
    cJSON *types = cJSON_AddArrayToObject(b, "QueueableMediaTypes");
    cJSON_AddItemToArray(types, cJSON_CreateString("Audio"));
    cJSON_AddItemToArray(types, cJSON_CreateString("Video"));
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    char path[64];
    snprintf(path, sizeof path, "/Sessions/Playing%s", kind);
    zos_http_resp_t r;
    request(c, "POST", path, body, &r);
    zos_http_free(&r);
    free(body);
}

void emby_stop_encodings(const emby_cfg_t *c, const char *play_session)
{
    char path[200];
    snprintf(path, sizeof path, "/Videos/ActiveEncodings?DeviceId=%s&PlaySessionId=%s", c->device_id, play_session);
    zos_http_resp_t r;
    request(c, "DELETE", path, NULL, &r);
    zos_http_free(&r);
}

bool emby_is_video(const emby_item_t *it)
{
    return !strcmp(it->media_type, "Video") || !strcmp(it->type, "Movie") || !strcmp(it->type, "Episode") ||
           !strcmp(it->type, "Video") || !strcmp(it->type, "MusicVideo");
}

bool emby_is_playable(const emby_item_t *it)
{
    return !it->is_folder && (emby_is_video(it) || !strcmp(it->media_type, "Audio") || !strcmp(it->type, "Audio"));
}

char *emby_stream_url(const emby_cfg_t *c, const emby_item_t *it, const char *ps, const char **method)
{
    char *u = malloc(1400);
    const emby_quality_t *q = &emby_qualities[c->quality % emby_quality_count];
    if (!emby_is_video(it)) {
        snprintf(u, 1400, "%s/Audio/%s/universal?UserId=%s&DeviceId=%s&api_key=%s&PlaySessionId=%s"
                 "&MaxStreamingBitrate=320000&Container=mp3,aac,m4a,flac,ogg,opus,wav"
                 "&TranscodingContainer=mp3&TranscodingProtocol=http&AudioCodec=mp3",
                 c->server, it->id, c->user_id, c->device_id, c->token, ps);
        *method = "DirectStream";
    } else if (q->max_w == 0) {
        snprintf(u, 1400, "%s/Videos/%s/stream?static=true&MediaSourceId=%s&DeviceId=%s&api_key=%s&PlaySessionId=%s",
                 c->server, it->id, it->id, c->device_id, c->token, ps);
        *method = "DirectPlay";
    } else {
        /* HLS so mpv can seek; the server transcodes segments on demand */
        snprintf(u, 1400, "%s/Videos/%s/master.m3u8?MediaSourceId=%s&DeviceId=%s&api_key=%s&PlaySessionId=%s"
                 "&VideoCodec=h264&AudioCodec=aac&MaxAudioChannels=2&AudioBitrate=128000"
                 "&VideoBitrate=%d&MaxWidth=%d&MaxHeight=%d&Profile=high&Level=41&SegmentContainer=ts&MinSegments=1"
                 "&BreakOnNonKeyFrames=True",
                 c->server, it->id, it->id, c->device_id, c->token, ps, q->video_kbps * 1000, q->max_w, q->max_h);
        *method = "Transcode";
    }
    return u;
}
