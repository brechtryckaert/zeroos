/* One keypad input device for every backend.
 *
 * Backends push key codes into a queue (evdev on the device, an SDL event
 * watch in the simulator, the script runner when headless); LVGL reads a
 * press + release for each. Keys are LVGL key codes, Unicode code points, or
 * the ZOS_KEY_* extensions for things LVGL has no code for (Ctrl+letter,
 * Page Up/Down). Text fields never see the extensions; the terminal does. */
#include <string.h>
#include "zos.h"
#include "core/hotkeys.h"

#define QLEN 256

static uint32_t q[QLEN];
static int head, tail;
static bool sent_press;
lv_indev_t *zos_keypad;

void zos_key_push(uint32_t key)
{
    if (zos_hotkey(key)) return;   /* volume, brightness, play / pause: everywhere */
    int next = (tail + 1) % QLEN;
    if (next == head) return;   /* full: drop, better than blocking */
    q[tail] = key;
    tail = next;
}

bool zos_key_queue_empty(void) { return head == tail; }

static bool is_text_field(lv_indev_t *in)
{
    lv_group_t *g = lv_indev_get_group(in);
    lv_obj_t *f = g ? lv_group_get_focused(g) : NULL;
    return f && lv_obj_check_type(f, &lv_textarea_class);
}

static void read_cb(lv_indev_t *in, lv_indev_data_t *data)
{
    static uint32_t last;
    data->key = last;
    data->state = LV_INDEV_STATE_RELEASED;
    while (head != tail) {
        uint32_t k = q[head];
        if (k >= ZOS_KEY_EXT_BASE && is_text_field(in)) {
            /* Text fields: Tab moves between fields; other extensions would be
             * inserted as garbage characters, so they are dropped. */
            if (k == ZOS_KEY_TAB) k = LV_KEY_DOWN;
            else if (k == ZOS_KEY_BACKTAB) k = LV_KEY_UP;
            else {
                head = (head + 1) % QLEN;
                continue;
            }
        }
        last = data->key = k;
        if (!sent_press) {
            data->state = LV_INDEV_STATE_PRESSED;
            sent_press = true;
        } else {
            sent_press = false;
            head = (head + 1) % QLEN;
        }
        data->continue_reading = head != tail || sent_press;
        return;
    }
}

lv_indev_t *zos_input_create(void)
{
    zos_keypad = lv_indev_create();
    lv_indev_set_type(zos_keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(zos_keypad, read_cb);
    return zos_keypad;
}

/* ---------------- SDL (simulator) ---------------- */
#if ZOS_WITH_SDL
#include LV_SDL_INCLUDE_PATH

static void push_utf8(const char *s)
{
    while (*s) {
        uint32_t c = (uint8_t)*s++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        while (extra-- && *s) c = (c << 6) | ((uint8_t)*s++ & 0x3F);
        zos_key_push(c);
    }
}

static int sdl_watch(void *ud, SDL_Event *e)
{
    LV_UNUSED(ud);
    if (e->type == SDL_TEXTINPUT) {
        if (!(SDL_GetModState() & KMOD_CTRL)) push_utf8(e->text.text);
        return 0;
    }
    if (e->type != SDL_KEYDOWN) return 0;
    SDL_Keycode k = e->key.keysym.sym;
    SDL_Keymod mod = e->key.keysym.mod;
    if ((mod & KMOD_CTRL) && ((k >= 'a' && k <= 'z') || k == '[' || k == ']' || k == '\\' || k == ' ')) {
        zos_key_push(ZOS_KEY_CTRL(k));
        return 0;
    }
    uint32_t key = 0;
    switch (k) {
    case SDLK_UP: key = LV_KEY_UP; break;
    case SDLK_DOWN: key = LV_KEY_DOWN; break;
    case SDLK_LEFT: key = LV_KEY_LEFT; break;
    case SDLK_RIGHT: key = LV_KEY_RIGHT; break;
    case SDLK_RETURN: case SDLK_KP_ENTER: key = LV_KEY_ENTER; break;
    case SDLK_ESCAPE: key = LV_KEY_ESC; break;
    case SDLK_BACKSPACE: key = LV_KEY_BACKSPACE; break;
    case SDLK_DELETE: key = LV_KEY_DEL; break;
    case SDLK_TAB: key = (mod & KMOD_SHIFT) ? ZOS_KEY_BACKTAB : ZOS_KEY_TAB; break;
    case SDLK_HOME: key = LV_KEY_HOME; break;
    case SDLK_END: key = LV_KEY_END; break;
    case SDLK_PAGEUP: key = ZOS_KEY_PGUP; break;
    case SDLK_PAGEDOWN: key = ZOS_KEY_PGDN; break;
    }
    if (key) zos_key_push(key);
    return 0;
}

void zos_input_sdl_attach(void)
{
    SDL_StartTextInput();
    SDL_AddEventWatch(sdl_watch, NULL);
}
#endif
