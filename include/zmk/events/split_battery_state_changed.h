/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <zmk/event_manager.h>

struct zmk_split_battery_state_changed {
    bool available;
};

ZMK_EVENT_DECLARE(zmk_split_battery_state_changed);
