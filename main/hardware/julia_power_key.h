#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 由单个轮询任务持有。先观察稳定松手，避免把上电按压算作关机；
 * 达到门限后也等稳定松手才断电，因为按住 PWR 本身仍能维持供电。 */
typedef struct {
    bool initialized;
    bool raw_pressed;
    bool pressed;
    bool armed;
    bool long_press;
    int64_t changed_ms;
    int64_t pressed_ms;
} julia_power_key_t;

static inline bool julia_power_key_update(julia_power_key_t *key, bool pressed,
                                         int64_t now_ms, int64_t hold_ms)
{
    if (!key->initialized || pressed != key->raw_pressed) {
        key->initialized = true;
        key->raw_pressed = pressed;
        key->changed_ms = now_ms;
    }
    if (now_ms - key->changed_ms < 50) return false;
    if (!key->armed) {
        if (!pressed) key->armed = true;
        return false;
    }
    if (pressed != key->pressed) {
        key->pressed = pressed;
        if (pressed) {
            key->pressed_ms = key->changed_ms;
            key->long_press = false;
        } else {
            bool shutdown = key->long_press ||
                key->changed_ms - key->pressed_ms >= hold_ms;
            key->long_press = false;
            return shutdown;
        }
    }
    if (pressed && now_ms - key->pressed_ms >= hold_ms) key->long_press = true;
    return false;
}
