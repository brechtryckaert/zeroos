/* Keyboard input on the device: reads a Linux evdev keyboard (the 46-key
 * matrix shows up as one once the matrix-keypad / M5Stack driver is loaded)
 * and pushes keys into the shared queue (src/input.c).
 *
 * Fn layer: like the ESP32 Cardputer, Fn + ; . , / give Up Down Left Right,
 * Fn + ` gives Esc and Fn + - / = give Page Up / Down. Ctrl + key gives
 * ZOS_KEY_CTRL(key) for the terminal. Held keys repeat (kernel autorepeat).
 * Change FN_KEYCODE if the driver reports Fn differently. */
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
        case KEY_MINUS:     return ZOS_KEY_PGUP;
        case KEY_EQUAL:     return ZOS_KEY_PGDN;
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
    case KEY_TAB:       return shift ? ZOS_KEY_BACKTAB : ZOS_KEY_TAB;
    case KEY_HOME:      return LV_KEY_HOME;
    case KEY_END:       return LV_KEY_END;
    case KEY_PAGEUP:    return ZOS_KEY_PGUP;
    case KEY_PAGEDOWN:  return ZOS_KEY_PGDN;
    case KEY_SPACE:     return ctrl ? ZOS_KEY_CTRL(' ') : ' ';
    }
    if (code >= 0 && code <= KEY_SLASH && keymap[code][0]) {
        char c = keymap[code][0];
        if (ctrl && ((c >= 'a' && c <= 'z') || c == '[' || c == ']' || c == '\\')) return ZOS_KEY_CTRL(c);
        return (uint8_t)keymap[code][shift ? 1 : 0];
    }
    return 0;
}

static void poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    struct input_event ev;
    while (fd >= 0 && read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY) continue;
        bool down = ev.value != 0;   /* 1 press, 2 autorepeat, 0 release */
        switch (ev.code) {
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: shift = down; continue;
        case KEY_LEFTCTRL: case KEY_RIGHTCTRL:   ctrl = down; continue;
        case FN_KEYCODE:                          fn = down; continue;
        }
        if (!down) continue;
        uint32_t k = translate(ev.code);
        if (k) zos_key_push(k);
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

void zos_evdev_keyboard_start(const char *path)
{
    fd = path ? open(path, O_RDONLY | O_NONBLOCK) : find_keyboard();
    if (fd < 0) fprintf(stderr, "zeroos: no keyboard found%s%s\n", path ? " at " : "", path ? path : "");
    lv_timer_create(poll_cb, 5, NULL);
}
