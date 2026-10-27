/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct zmk_split_battery_state {
    uint8_t level;
    /* Wired-power inference only; not measured charge current or Qi state. */
    bool charging;
};

/* Private session handshake protocol, not a user-bindable key behavior. */
#define ZMK_SPLIT_BATTERY_SESSION_MAGIC 0x534241A1U

/* Peripheral: accept a nonzero challenge and schedule an echo. Central: no-op. */
void zmk_split_battery_set_session(uint32_t nonce);

/* Central only: false until a valid, fresh sample arrives from the connected peer. */
bool zmk_split_battery_get_peer(struct zmk_split_battery_state *state);
