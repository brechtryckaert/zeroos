/* Apps that are planned but not built yet. Each shows what it will do and
 * returns to the launcher on Esc. Replace one at a time with a real app. */
#include "../zos.h"

extern const lv_image_dsc_t zos_icon_music, zos_icon_video, zos_icon_camera, zos_icon_settings;

static void back_on_esc(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_ESC || k == LV_KEY_BACKSPACE || k == 'q' || k == '`') zos_app_exit();
}

static lv_obj_t *focus_sink(lv_obj_t *scr, lv_group_t *g)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 1, 1);
    lv_obj_add_event_cb(o, back_on_esc, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g, o);
    return o;
}

#define STUB(var, ...)                                                        \
    extern const zos_app_t var;                                               \
    static void var##_open(lv_obj_t *scr, lv_group_t *g) {                    \
        zos_placeholder_body(scr, &var, __VA_ARGS__);                         \
        focus_sink(scr, g);                                                   \
    }

STUB(zos_app_music,
     "MP3 player on mpd.\nLibrary, queue, gapless playback and resume.\n\nComing soon.  Esc to go back.")
STUB(zos_app_video,
     "MP4 / MKV via libmpv with hardware H.264 decode.\n\nComing soon.  Esc to go back.")
STUB(zos_app_camera,
     "Viewfinder and stills from the 8MP IMX219 via libcamera.\n\nComing soon.  Esc to go back.")
STUB(zos_app_settings,
     "Wi-Fi, brightness, sleep timer and storage.\n\nComing soon.  Esc to go back.")

const zos_app_t zos_app_music = {
    .id = "music", .name = "Music", .tagline = "MP3 player",
    .icon = &zos_icon_music, .color = 0xe5484d, .open = zos_app_music_open,
};
const zos_app_t zos_app_video = {
    .id = "video", .name = "Video", .tagline = "MP4 & MKV player",
    .icon = &zos_icon_video, .color = 0x8e4ec6, .open = zos_app_video_open,
};
const zos_app_t zos_app_camera = {
    .id = "camera", .name = "Camera", .tagline = "Photos with the 8MP camera",
    .icon = &zos_icon_camera, .color = 0x12a594, .open = zos_app_camera_open,
};
const zos_app_t zos_app_settings = {
    .id = "settings", .name = "Settings", .tagline = "Wi-Fi, display, storage",
    .icon = &zos_icon_settings, .color = 0x52525b, .open = zos_app_settings_open,
};
