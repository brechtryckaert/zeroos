/* Keyboard input on the device: reads a Linux evdev keyboard (the 46-key
 * matrix shows up as one once the M5Stack driver is loaded) and pushes keys
 * into the shared queue (src/input.c).
 *
 * The CardputerZero's driver (M5Stack's tca8418_keypad_m5stack) does the
 * Fn, Sym and Shift layers itself (tap once, hold, or lock):
 *   Fn  sends real keys: arrows on F Z X C, Home/End/PgUp/PgDn, Delete,
 *       F1-F12, and media keys (Fn+A/S/D mute and volume, Fn+Q/W/E
 *       play-pause / previous / next, Fn+U/I brightness, Fn+H help).
 *   Sym reuses ordinary keycodes for the printed symbols (keycode 74 is
 *       "[", 91 is "."), M5Stack's tca8418_keypad_m5stack_keymap.map: on
 *       that keyboard those codes are translated with sym_chars below.
 * Ctrl + key gives ZOS_KEY_CTRL(key) for the terminal. Held keys repeat
 * (kernel autorepeat). Other keyboards: the ESP32 Cardputer-style Fn
 * layer (Fn + ; . , / = arrows) still works where a driver reports KEY_FN. */
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
static bool cardputer;   /* M5Stack's keyboard: Sym layer codes are symbols */

/* M5Stack's Sym layer: keycode -> character. */
static const struct { unsigned short code; char c; } sym_chars[] = {
    {26, '!'}, {27, '@'}, {39, '#'}, {40, '$'}, {41, '%'}, {43, '^'}, {51, '&'}, {52, '*'},
    {53, '('}, {94, ')'}, {55, '~'}, {69, '`'}, {70, '_'}, {71, '-'}, {72, '+'}, {73, '='},
    {74, '['}, {75, ']'}, {76, '{'}, {77, '}'}, {79, ';'}, {80, ':'}, {81, '\''}, {82, '"'},
    {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','}, {91, '.'}, {92, '/'}, {93, '?'},
};

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
    if (cardputer)
        for (size_t i = 0; i < sizeof sym_chars / sizeof sym_chars[0]; i++)
            if (sym_chars[i].code == code) return (uint8_t)sym_chars[i].c;
    switch (code) {
    case KEY_MUTE:           return ZOS_KEY_MUTE;
    case KEY_VOLUMEDOWN:     return ZOS_KEY_VOL_DOWN;
    case KEY_VOLUMEUP:       return ZOS_KEY_VOL_UP;
    case KEY_PLAYPAUSE: case KEY_PLAYCD: case KEY_PAUSECD: case KEY_PLAY: case KEY_PAUSE:
                             return ZOS_KEY_PLAYPAUSE;
    case KEY_PREVIOUSSONG:   return ZOS_KEY_PREV;
    case KEY_NEXTSONG:       return ZOS_KEY_NEXT;
    case KEY_BRIGHTNESSDOWN: return ZOS_KEY_BRIGHT_DOWN;
    case KEY_BRIGHTNESSUP:   return ZOS_KEY_BRIGHT_UP;
    case KEY_HELP:           return ZOS_KEY_HELP;
    /* Keypad keys (a USB keyboard; on the CardputerZero these are Sym). */
    case KEY_KP0: return '0';
    case KEY_KP1: return '1';
    case KEY_KP2: return '2';
    case KEY_KP3: return '3';
    case KEY_KP4: return '4';
    case KEY_KP5: return '5';
    case KEY_KP6: return '6';
    case KEY_KP7: return '7';
    case KEY_KP8: return '8';
    case KEY_KP9: return '9';
    case KEY_KPDOT: return '.';
    case KEY_KPPLUS: return '+';
    case KEY_KPMINUS: return '-';
    case KEY_KPASTERISK: return '*';
    case KEY_KPSLASH: return '/';
    case KEY_KPENTER: return LV_KEY_ENTER;
    }
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
        else if (ev.code != KEY_CD && ev.code != KEY_DVD && ev.code != KEY_LEFTALT && ev.code != KEY_CAPSLOCK) {
            static unsigned char told[KEY_MAX / 8 + 1];   /* once per code, for the journal */
            if (ev.code <= KEY_MAX && !(told[ev.code / 8] & (1u << (ev.code % 8)))) {
                told[ev.code / 8] |= (unsigned char)(1u << (ev.code % 8));
                fprintf(stderr, "zeroos: key code %d has no meaning here\n", ev.code);
            }
        }
    }
}

static int find_keyboard(void)
{
    for (int i = 0; i < 32; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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
    /* The CardputerZero's TCA8418 keyboard on I2C1 (M5Stack's device tree).
     * Preferred over the scan, which could also match the IR receiver. */
    static const char *cz_kbd = "/dev/input/by-path/platform-3f804000.i2c-event";
    if (!path && access(cz_kbd, R_OK) == 0) path = cz_kbd;
    fd = path ? open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : find_keyboard();
    if (fd < 0) fprintf(stderr, "zeroos: no keyboard found%s%s\n", path ? " at " : "", path ? path : "");
    if (fd >= 0) {
        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        cardputer = strstr(name, "tca8418") || (path && (strstr(path, "cardputer") || strstr(path, "3f804000.i2c")));
        printf("zeroos: keyboard \"%s\"%s\n", name, cardputer ? " (CardputerZero layers)" : "");
    }
    lv_timer_create(poll_cb, 5, NULL);
}
