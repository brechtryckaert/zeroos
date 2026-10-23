/* Keyboard input on the device: reads a Linux evdev keyboard (the 46-key
 * matrix shows up as one once the matrix-keypad / M5Stack driver is loaded)
 * and turns key codes into LVGL keys and ASCII.
 *
 * Fn layer: like the ESP32 Cardputer, Fn + ; . , / give Up Down Left Right
 * and Fn + ` gives Esc. Change FN_KEYCODE if the driver reports Fn differently. */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "zos.h"

#ifndef FN_KEYCODE
#define FN_KEYCODE KEY_FN
#endif

#define BITS_PER_LONG (sizeof(long) * 8)
#define TEST_BIT(b, arr) ((arr[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1)

static int fd = -1;
static bool shift, fn, ctrl;

/* keycode -> {normal, shifted} */
static const char keymap[KEY_SLASH + 1][2] = {
    [KEY_1] = {'1', '!'}, [KEY_2] = {'2', '@'}, [KEY_3] = {'3', '#'}, [KEY_4] = {'4', '$'},
    [KEY_5] = {'5', '%'}, [KEY_6] = {'6', '^'}, [KEY_7] = {'7', '&'}, [KEY_8] = {'8', '*'},
    [KEY_9] = {'9', '('}, [KEY_0] = {'0', ')'}, [KEY_MINUS] = {'-', '_'}, [KEY_EQUAL] = {'=', '+'},
    [KEY_Q] = {'q', 'Q'}, [KEY_W] = {'w', 'W'}, [KEY_E] = {'e', 'E'}, [KEY_R] = {'r', 'R'},
    [KEY_T] = {'t', 'T'}, [KEY_Y] = {'y', 'Y'}, [KEY_U] = {'u', 'U'}, [KEY_I] = {'i', 'I'},
    [KEY_O] = {'o', 'O'}, [KEY_P] = {'p', 'P'}, [KEY_LEFTBRACE] = {'[', '{'},
    [KEY_RIGHTBRACE] = {']', '}'}, [KEY_A] = {'a', 'A'}, [KEY_S] = {'s', 'S'},
    [KEY_D] = {'d', 'D'}, [KEY_F] = {'f', 'F'}, [KEY_G] = {'g', 'G'}, [KEY_H] = {'h', 'H'},
    [KEY_J] = {'j', 'J'}, [KEY_K] = {'k', 'K'}, [KEY_L] = {'l', 'L'},
    [KEY_SEMICOLON] = {';', ':'}, [KEY_APOSTROPHE] = {'\'', '"'}, [KEY_GRAVE] = {'`', '~'},
    [KEY_BACKSLASH] = {'\\', '|'}, [KEY_Z] = {'z', 'Z'}, [KEY_X] = {'x', 'X'},
    [KEY_C] = {'c', 'C'}, [KEY_V] = {'v', 'V'}, [KEY_B] = {'b', 'B'}, [KEY_N] = {'n', 'N'},
    [KEY_M] = {'m', 'M'}, [KEY_COMMA] = {',', '<'}, [KEY_DOT] = {'.', '>'},
    [KEY_SLASH] = {'/', '?'},
};

static uint32_t translate(int code)
{
    if (fn) {
        switch (code) {
        case KEY_SEMICOLON: return LV_KEY_UP;
        case KEY_DOT:       return LV_KEY_DOWN;
        case KEY_COMMA:     return LV_KEY_LEFT;
        case KEY_SLASH:     return LV_KEY_RIGHT;
        case KEY_GRAVE:     return LV_KEY_ESC;
        case KEY_BACKSPACE: return LV_KEY_DEL;
        }
    }
    switch (code) {
    case KEY_UP:        return LV_KEY_UP;
    case KEY_DOWN:      return LV_KEY_DOWN;
    case KEY_LEFT:      return LV_KEY_LEFT;
    case KEY_RIGHT:     return LV_KEY_RIGHT;
    case KEY_ENTER:     return LV_KEY_ENTER;
    case KEY_ESC:       return LV_KEY_ESC;
    case KEY_BACKSPACE: return LV_KEY_BACKSPACE;
    case KEY_DELETE:    return LV_KEY_DEL;
    case KEY_TAB:       return shift ? LV_KEY_PREV : LV_KEY_NEXT;
    case KEY_HOME:      return LV_KEY_HOME;
    case KEY_END:       return LV_KEY_END;
    case KEY_SPACE:     return ' ';
    }
    if (code >= 0 && code <= KEY_SLASH && keymap[code][0]) return (uint8_t)keymap[code][shift ? 1 : 0];
    return 0;
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    static uint32_t last_key;
    data->key = last_key;
    data->state = LV_INDEV_STATE_RELEASED;
    struct input_event ev;
    while (fd >= 0 && read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY || ev.value == 2) continue;   /* LVGL does its own repeat */
        bool down = ev.value == 1;
        switch (ev.code) {
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: shift = down; continue;
        case KEY_LEFTCTRL: case KEY_RIGHTCTRL:   ctrl = down; continue;
        case FN_KEYCODE:                          fn = down; continue;
        }
        uint32_t k = translate(ev.code);
        if (!k) continue;
        last_key = k;
        data->key = k;
        data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = true;   /* deliver queued events next pass */
        return;
    }
}

static int find_keyboard(void)
{
    for (int i = 0; i < 32; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = open(p, O_RDONLY | O_NONBLOCK);
        if (f < 0) continue;
        unsigned long bits[(KEY_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {0};
        if (ioctl(f, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0 && TEST_BIT(KEY_A, bits)
            && TEST_BIT(KEY_SPACE, bits)) {
            char name[128] = "?";
            ioctl(f, EVIOCGNAME(sizeof name), name);
            printf("zeroos: keyboard %s (%s)\n", p, name);
            return f;
        }
        close(f);
    }
    return -1;
}

lv_indev_t *zos_evdev_keyboard_create(const char *path)
{
    fd = path ? open(path, O_RDONLY | O_NONBLOCK) : find_keyboard();
    if (fd < 0) fprintf(stderr, "zeroos: no keyboard found%s%s\n", path ? " at " : "", path ? path : "");
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(in, read_cb);
    return in;
}
