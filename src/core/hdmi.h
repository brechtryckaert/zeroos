/* HDMI output.
 *
 * Watches the HDMI connector (hot-plug), and while a monitor is connected
 * mirrors the ZeroOS interface onto it: every frame drawn for the 320x170
 * panel is also scaled up (by a whole number, so pixels stay sharp) into a
 * framebuffer on the HDMI output. When mpv plays a video on HDMI, ZeroOS
 * lets go of the output first (zos_hdmi_hold) and takes it back afterwards.
 *
 * ZOS_FAKE_HDMI=1280x720 simulates a connected monitor (for tests); the
 * headless "hdmishot:file.ppm" script step saves what it would show. */
#ifndef ZOS_HDMI_H
#define ZOS_HDMI_H

#include <stdbool.h>
#include "lvgl.h"

void zos_hdmi_init(lv_display_t *disp);
bool zos_hdmi_connected(void);
/* DRM device of the HDMI output ("/dev/dri/card0"), or NULL. */
const char *zos_hdmi_card(void);
/* ALSA device for HDMI audio, for mpv's --audio-device. */
const char *zos_hdmi_audio_device(void);
/* Mirror setting (saved). */
bool zos_hdmi_mirror_enabled(void);
void zos_hdmi_set_mirror(bool on);
/* Another program uses the HDMI output while held. */
void zos_hdmi_hold(bool held);
/* "1920x1080, mirroring" etc., for Settings. */
void zos_hdmi_describe(char *out, size_t n);
bool zos_hdmi_save_ppm(const char *path);
/* Tests (ZOS_FAKE_HDMI): plug the simulated monitor in or out. */
void zos_hdmi_fake_plug(bool on);

#endif
