/*
 * Private split battery session challenge receiver.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_split_battery_session

#include <drivers/behavior.h>
#include <errno.h>
#include <zephyr/device.h>
#include <zmk/behavior.h>
#include <zmk/split_battery.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_session_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    if (binding->param1 == 0 || binding->param2 != ZMK_SPLIT_BATTERY_SESSION_MAGIC) {
        return -EINVAL;
    }
    zmk_split_battery_set_session(binding->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_session_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

/* Direct split commands already target the peripheral; no GLOBAL fanout needed. */
static const struct behavior_driver_api split_battery_session_driver_api = {
    .binding_pressed = on_session_pressed,
    .binding_released = on_session_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &split_battery_session_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
