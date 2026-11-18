#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if ZOS_WITH_DRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif
#include "src/display/lv_display_private.h"
#include "../zos.h"
#include "hdmi.h"
#include "settings.h"

#define POLL_MS 1500

static struct {
    lv_display_t *disp;
    lv_display_flush_cb_t orig_flush;
    char card[64];          /* /dev/dri/cardN */
    char status_path[160];  /* /sys/class/drm/cardN-HDMI-A-1/status */
    bool fake;
    bool connected, held, active;
    uint32_t retry_at;
    bool warned;
    /* output */
    int fd;
    uint32_t fb_id, handle, crtc, conn_id;
    uint8_t *map;
    size_t map_size;
    uint32_t pitch;
    int w, h, scale, ox, oy;
} H = {.fd = -1};

bool zos_hdmi_connected(void) { return H.connected; }
const char *zos_hdmi_card(void) { return H.card[0] ? H.card : NULL; }
const char *zos_hdmi_audio_device(void)
{
    const char *e = getenv("ZOS_HDMI_AUDIO");
    return e ? e : "alsa/hdmi:CARD=vc4hdmi,DEV=0";
}
bool zos_hdmi_mirror_enabled(void) { return zos_setting_int("hdmi_mirror", 1) != 0; }

/* ---------------- output (DRM) ---------------- */

static void clear_black(void)
{
    if (H.map) memset(H.map, 0, H.map_size);
}

static void close_output(void)
{
    if (H.fake) {
        free(H.map);
        H.map = NULL;
        H.active = false;
        return;
    }
#if ZOS_WITH_DRM
    if (H.map) munmap(H.map, H.map_size);
    if (H.fb_id) drmModeRmFB(H.fd, H.fb_id);
    if (H.handle) {
        struct drm_mode_destroy_dumb d = {.handle = H.handle};
        drmIoctl(H.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
#endif
    if (H.fd >= 0) close(H.fd);
    H.fd = -1;
    H.map = NULL;
    H.fb_id = H.handle = 0;
    H.active = false;
}

static void setup_geometry(void)
{
    int sx = H.w / ZOS_HOR_RES, sy = H.h / ZOS_VER_RES;
    H.scale = sx < sy ? sx : sy;
    if (H.scale < 1) H.scale = 1;
    H.ox = (H.w - ZOS_HOR_RES * H.scale) / 2;
    H.oy = (H.h - ZOS_VER_RES * H.scale) / 2;
    if (H.ox < 0) H.ox = 0;
    if (H.oy < 0) H.oy = 0;
}

#if ZOS_WITH_DRM
static drmModeModeInfo *pick_mode(drmModeConnector *c)
{
    drmModeModeInfo *best = NULL;
    for (int i = 0; i < c->count_modes; i++) {
        drmModeModeInfo *m = &c->modes[i];
        if (m->hdisplay > 1920 || m->vdisplay > 1080 || m->vrefresh > 60) continue;
        if (m->flags & DRM_MODE_FLAG_INTERLACE) continue;
        if (m->type & DRM_MODE_TYPE_PREFERRED) return m;
        if (!best || m->hdisplay * m->vdisplay > best->hdisplay * best->vdisplay ||
            (m->hdisplay * m->vdisplay == best->hdisplay * best->vdisplay && m->vrefresh > best->vrefresh))
            best = m;
    }
    return best ? best : (c->count_modes ? &c->modes[0] : NULL);
}

static bool open_drm(char *err, size_t errn)
{
    H.fd = open(H.card, O_RDWR | O_CLOEXEC);
    if (H.fd < 0) { snprintf(err, errn, "open %s: %s", H.card, strerror(errno)); return false; }
    drmModeRes *res = drmModeGetResources(H.fd);
    if (!res) { snprintf(err, errn, "no KMS resources"); return false; }
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector *c = drmModeGetConnector(H.fd, res->connectors[i]);
        if (c && c->connector_type == DRM_MODE_CONNECTOR_HDMIA && c->connection == DRM_MODE_CONNECTED && c->count_modes)
            conn = c;
        else if (c)
            drmModeFreeConnector(c);
    }
    bool ok = false;
    if (!conn) { snprintf(err, errn, "no connected HDMI connector"); goto out; }
    drmModeModeInfo mode = *pick_mode(conn);
    H.conn_id = conn->connector_id;
    H.crtc = 0;
    if (conn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(H.fd, conn->encoder_id);
        if (e) { H.crtc = e->crtc_id; drmModeFreeEncoder(e); }
    }
    for (int i = 0; i < conn->count_encoders && !H.crtc; i++) {
        drmModeEncoder *e = drmModeGetEncoder(H.fd, conn->encoders[i]);
        if (!e) continue;
        for (int c = 0; c < res->count_crtcs; c++)
            if (e->possible_crtcs & (1u << c)) { H.crtc = res->crtcs[c]; break; }
        drmModeFreeEncoder(e);
    }
    if (!H.crtc) { snprintf(err, errn, "no CRTC for HDMI"); goto out; }

    struct drm_mode_create_dumb cd = {.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32};
    if (drmIoctl(H.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { snprintf(err, errn, "create buffer: %s", strerror(errno)); goto out; }
    H.handle = cd.handle;
    H.pitch = cd.pitch;
    H.map_size = cd.size;
    if (drmModeAddFB(H.fd, mode.hdisplay, mode.vdisplay, 24, 32, cd.pitch, cd.handle, &H.fb_id)) {
        snprintf(err, errn, "add framebuffer: %s", strerror(errno)); goto out;
    }
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (drmIoctl(H.fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { snprintf(err, errn, "map: %s", strerror(errno)); goto out; }
    void *p = mmap(NULL, H.map_size, PROT_READ | PROT_WRITE, MAP_SHARED, H.fd, (off_t)md.offset);
    if (p == MAP_FAILED) { snprintf(err, errn, "mmap: %s", strerror(errno)); goto out; }
    H.map = p;
    H.w = mode.hdisplay;
    H.h = mode.vdisplay;
    clear_black();
    if (drmModeSetCrtc(H.fd, H.crtc, H.fb_id, 0, 0, &H.conn_id, 1, &mode)) {
        snprintf(err, errn, "set mode %dx%d: %s", H.w, H.h, strerror(errno)); goto out;
    }
    ok = true;
out:
    if (conn) drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return ok;
}
#endif

static void full_redraw(void)
{
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
}

static void open_output(void)
{
    if (H.active || H.held || !H.connected || !zos_hdmi_mirror_enabled()) return;
    if (lv_tick_get() < H.retry_at) return;
    char err[160] = "";
    bool ok = false;
    if (H.fake) {
        if (sscanf(getenv("ZOS_FAKE_HDMI"), "%dx%d", &H.w, &H.h) != 2) { H.w = 1280; H.h = 720; }
        H.pitch = (uint32_t)H.w * 4;
        H.map_size = (size_t)H.pitch * H.h;
        H.map = calloc(1, H.map_size);
        ok = H.map != NULL;
    }
#if ZOS_WITH_DRM
    else ok = open_drm(err, sizeof err);
#endif
    if (!ok) {
        if (!H.warned) fprintf(stderr, "zeroos: HDMI mirror unavailable: %s\n", err[0] ? err : "no DRM support in this build");
        H.warned = true;
        close_output();
        H.retry_at = lv_tick_get() + 5000;
        return;
    }
    H.warned = false;
    setup_geometry();
    H.active = true;
    full_redraw();
}

/* ---------------- mirror ---------------- */

static void blit(const lv_area_t *a, const uint8_t *px, uint32_t stride)
{
    int s = H.scale;
    for (int32_t y = a->y1; y <= a->y2; y++) {
        if (y < 0 || y >= ZOS_VER_RES) continue;
        const uint16_t *src = (const uint16_t *)(px + (size_t)(y - a->y1) * stride);
        uint32_t *row = (uint32_t *)(H.map + (size_t)(H.oy + y * s) * H.pitch) + H.ox + a->x1 * s;
        int32_t w = a->x2 - a->x1 + 1;
        for (int32_t x = 0; x < w; x++) {
            uint16_t c = src[x];
            uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
            uint32_t v = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            for (int k = 0; k < s; k++) row[x * s + k] = v;
        }
        for (int k = 1; k < s; k++)
            memcpy((uint8_t *)row + (size_t)k * H.pitch, row, (size_t)w * s * 4);
    }
#if ZOS_WITH_DRM
    if (!H.fake) {
        drmModeClip clip = {(uint16_t)(H.ox + a->x1 * s), (uint16_t)(H.oy + a->y1 * s),
                            (uint16_t)(H.ox + (a->x2 + 1) * s), (uint16_t)(H.oy + (a->y2 + 1) * s)};
        drmModeDirtyFB(H.fd, H.fb_id, &clip, 1);   /* vc4 scans out directly; harmless if unsupported */
    }
#endif
}

static void flush_wrap(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    if (H.active && H.map) {
        lv_area_t a = *area;
        const uint8_t *src = px;
        uint32_t stride;
        if (d->render_mode == LV_DISPLAY_RENDER_MODE_PARTIAL) {
            stride = lv_draw_buf_width_to_stride((uint32_t)lv_area_get_width(&a), LV_COLOR_FORMAT_RGB565);
        } else {   /* direct / full: px is the whole screen */
            stride = lv_draw_buf_width_to_stride(ZOS_HOR_RES, LV_COLOR_FORMAT_RGB565);
            src = px + (size_t)a.y1 * stride + (size_t)a.x1 * 2;
        }
        if (a.x1 < 0) a.x1 = 0;
        if (a.x2 >= ZOS_HOR_RES) a.x2 = ZOS_HOR_RES - 1;
        if (a.x2 >= a.x1) blit(&a, src, stride);
    }
    H.orig_flush(d, area, px);
}

/* ---------------- hot-plug ---------------- */

static bool find_connector(void)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        const char *dash = strstr(e->d_name, "-HDMI-A-");
        if (strncmp(e->d_name, "card", 4) || !dash) continue;
        snprintf(H.status_path, sizeof H.status_path, "/sys/class/drm/%s/status", e->d_name);
        snprintf(H.card, sizeof H.card, "/dev/dri/%.*s", (int)(dash - e->d_name), e->d_name);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

static bool read_connected(void)
{
    if (H.fake) return true;
    FILE *f = fopen(H.status_path, "r");
    if (!f) return false;
    char s[32] = "";
    if (!fgets(s, sizeof s, f)) s[0] = 0;
    fclose(f);
    return !strncmp(s, "connected", 9);
}

static void poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    bool c = read_connected();
    if (c != H.connected) {
        H.connected = c;
        if (c) {
            open_output();
            zos_notify(0x3b82f6, "HDMI", H.active ? "Monitor connected, mirroring the screen" : "Monitor connected");
        } else {
            close_output();
            zos_notify(0x52525b, "HDMI", "Monitor disconnected");
        }
    } else if (c && !H.active) {
        open_output();   /* retry after a failure, or after the mirror was switched on */
    }
}

void zos_hdmi_init(lv_display_t *disp)
{
    H.disp = disp;
    H.fake = getenv("ZOS_FAKE_HDMI") != NULL;
    if (!H.fake && !find_connector()) return;   /* no HDMI on this machine */
    if (H.fake) snprintf(H.card, sizeof H.card, "/dev/null");
    H.orig_flush = disp->flush_cb;
    lv_display_set_flush_cb(disp, flush_wrap);
    H.connected = read_connected();
    if (H.connected) open_output();
    lv_timer_create(poll_cb, POLL_MS, NULL);
}

void zos_hdmi_set_mirror(bool on)
{
    zos_setting_set_int("hdmi_mirror", on);
    if (on) { H.retry_at = 0; open_output(); }
    else close_output();
}

void zos_hdmi_hold(bool held)
{
    if (held == H.held) return;
    H.held = held;
    if (held) close_output();
    else { H.retry_at = 0; open_output(); }
}

void zos_hdmi_describe(char *out, size_t n)
{
    if (!H.card[0]) snprintf(out, n, "No HDMI output");
    else if (!H.connected) snprintf(out, n, "No monitor connected");
    else if (H.held) snprintf(out, n, "Connected, playing video");
    else if (H.active) snprintf(out, n, "%dx%d, mirroring (x%d)", H.w, H.h, H.scale);
    else if (!zos_hdmi_mirror_enabled()) snprintf(out, n, "Connected, mirror off");
    else snprintf(out, n, "Connected, can't show the screen");
}

bool zos_hdmi_save_ppm(const char *path)
{
    if (!H.active || !H.map) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", H.w, H.h);
    for (int y = 0; y < H.h; y++) {
        const uint32_t *row = (const uint32_t *)(H.map + (size_t)y * H.pitch);
        for (int x = 0; x < H.w; x++) {
            uint8_t rgb[3] = {(uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x]};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("zeroos: saved %s\n", path);
    return true;
}
