/* Keys that work everywhere: volume, mute, brightness and play / pause
 * (the CardputerZero's printed Fn keys). Called for every key before it
 * reaches an app; returns true when the key was used up here. */
#ifndef ZOS_HOTKEYS_H
#define ZOS_HOTKEYS_H

#include <stdbool.h>
#include <stdint.h>

bool zos_hotkey(uint32_t key);

#endif
