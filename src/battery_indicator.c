/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/workqueue.h>
#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/position_state_changed.h>

#define KEYMAP_LOCAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RED 0
#define GREEN 1
#define BLUE 2

#define COLOR_BAT CONFIG_ZMK_BATTERY_INDICATOR_COLOR
#define COLOR_LOW 0xFF0000

#define TICK_MS 100
#define PULSE_PERIOD_TICKS 20

static const struct device *const battery_indicator_dev =
    DEVICE_DT_GET(DT_CHOSEN(zmk_battery_indicator));

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_battery),
             "CONFIG_ZMK_BATTERY_INDICATOR is enabled but no zmk,battery chosen node found");

BUILD_ASSERT(
    DT_HAS_CHOSEN(zmk_battery_indicator_map),
    "CONFIG_ZMK_BATTERY_INDICATOR is enabled but no zmk,battery-indicator-map chosen node found");

#define BATTERY_IND_NUM_LEDS DT_PROP_LEN(DT_CHOSEN(zmk_battery_indicator_map), led_channels)

BUILD_ASSERT(BATTERY_IND_NUM_LEDS % 3 == 0,
             "zmk,battery-indicator-map must have three channels per RGB LED");

#define NUM_RGB_LEDS (BATTERY_IND_NUM_LEDS / 3)

BUILD_ASSERT(NUM_RGB_LEDS == 4, "battery indicator patterns expect exactly 4 RGB LEDs");

#define GET_LED_CHANNEL_ORDINAL(idx, node_id) DT_NODE_CHILD_IDX(DT_PHANDLE_BY_IDX(node_id, led_channels, idx))

static const uint8_t battery_indicator_map_array[] = {LISTIFY(
    BATTERY_IND_NUM_LEDS, GET_LED_CHANNEL_ORDINAL, (, ), DT_CHOSEN(zmk_battery_indicator_map))};

static const uint8_t battery_indicator_color_map[] =
    DT_PROP(DT_CHOSEN(zmk_battery_indicator_map), colors);

#define LAYER_COLORS_ENABLED DT_HAS_CHOSEN(zmk_layer_colors)

#if LAYER_COLORS_ENABLED
static const uint32_t layer_color_table[] = DT_PROP(DT_CHOSEN(zmk_layer_colors), colors);

BUILD_ASSERT(ARRAY_SIZE(layer_color_table) == ZMK_KEYMAP_LAYERS_LEN,
             "zmk,layer-colors table length must match keymap layers");
BUILD_ASSERT(DT_PROP_BY_IDX(DT_CHOSEN(zmk_layer_colors), colors, 1) != 0 &&
                 DT_PROP_BY_IDX(DT_CHOSEN(zmk_layer_colors), colors, 2) != 0 &&
                 DT_PROP_BY_IDX(DT_CHOSEN(zmk_layer_colors), colors, 3) != 0 &&
                 DT_PROP_BY_IDX(DT_CHOSEN(zmk_layer_colors), colors, 4) != 0 &&
                 DT_PROP_BY_IDX(DT_CHOSEN(zmk_layer_colors), colors, 5) != 0,
             "layer indicator colors missing from devicetree");
#endif

enum indicator_state {
    IND_STATE_OFF,
    IND_STATE_BAT_BAR,
    IND_STATE_LOW,
    IND_STATE_LAYER,
    IND_STATE_FULL,
    IND_STATE_CHARGING,
};

static enum indicator_state last_state = IND_STATE_OFF;
static enum zmk_activity_state activity_state = ZMK_ACTIVITY_ACTIVE;
static int64_t bat_show_until;
static uint32_t tick;
static uint32_t layer_color;

// Mirrored from the central by the lc behavior on split peripherals.
static uint32_t remote_layer_color;

static int set_led(uint8_t led, uint32_t color, uint8_t brightness) {
    int err = 0;

    for (int c = 0; c < 3; c++) {
        int i = led * 3 + c;
        uint32_t component;

        if (battery_indicator_color_map[i] == RED) {
            component = (color >> 16) & 0xFF;
        } else if (battery_indicator_color_map[i] == GREEN) {
            component = (color >> 8) & 0xFF;
        } else if (battery_indicator_color_map[i] == BLUE) {
            component = color & 0xFF;
        } else {
            LOG_ERR("Invalid color map value: %d", battery_indicator_color_map[i]);
            return -EINVAL;
        }

        int rc = led_set_brightness(battery_indicator_dev, battery_indicator_map_array[i],
                                    (component * brightness) / 255);
        if (rc) {
            err = rc;
        }
    }

    return err;
}

static int set_all_leds(uint32_t color, uint8_t brightness) {
    int err = 0;

    for (int i = 0; i < NUM_RGB_LEDS; i++) {
        int rc = set_led(i, color, brightness);
        if (rc) {
            err = rc;
        }
    }

    return err;
}

static int set_bar_leds(uint32_t color, uint8_t bar_length, uint8_t brightness) {
    int err = 0;

    for (int i = 0; i < NUM_RGB_LEDS; i++) {
        int rc = set_led(i, color, i < bar_length ? brightness : 0);
        if (rc) {
            err = rc;
        }
    }

    return err;
}

static uint8_t pulse_brightness(void) {
    uint32_t phase = tick % PULSE_PERIOD_TICKS;

    if (phase <= PULSE_PERIOD_TICKS / 2) {
        return phase * 255 / (PULSE_PERIOD_TICKS / 2);
    }

    return (PULSE_PERIOD_TICKS - phase) * 255 / (PULSE_PERIOD_TICKS / 2);
}

static uint32_t highest_layer_color(void) {
#if LAYER_COLORS_ENABLED
    int max = MIN(ARRAY_SIZE(layer_color_table), ZMK_KEYMAP_LAYERS_LEN);

    for (int i = max - 1; i >= 0; i--) {
        if (layer_color_table[i] != 0 && zmk_keymap_layer_active((zmk_keymap_layer_id_t)i)) {
            return layer_color_table[i];
        }
    }
#endif
    return 0;
}

static enum indicator_state evaluate_state(void) {
    if (activity_state != ZMK_ACTIVITY_ACTIVE) {
        return IND_STATE_OFF;
    }

    if (k_uptime_get() < bat_show_until) {
        return IND_STATE_BAT_BAR;
    }

    uint8_t soc = zmk_battery_state_of_charge();

#if LAYER_COLORS_ENABLED
    layer_color = KEYMAP_LOCAL ? highest_layer_color() : remote_layer_color;
    if (layer_color != 0) {
        return IND_STATE_LAYER;
    }
#endif

    if (soc >= CONFIG_ZMK_BATTERY_INDICATOR_HIGH_THRESHOLD) {
        return IND_STATE_FULL;
    }

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK) && IS_ENABLED(CONFIG_ZMK_BATTERY_INDICATOR_CHARGING)
    // Charging outranks low-batt red so the bar shows charge progress while plugged
    // in; unplug and a <20% cell goes back to solid red.
    if (zmk_usb_is_powered()) {
        return IND_STATE_CHARGING;
    }
#endif

    if (soc < CONFIG_ZMK_BATTERY_INDICATOR_LOW_THRESHOLD) {
        return IND_STATE_LOW;
    }

    return IND_STATE_OFF;
}

static bool state_needs_ticks(enum indicator_state state) {
    return state == IND_STATE_BAT_BAR || state == IND_STATE_CHARGING;
}

static void render(enum indicator_state state) {
    uint8_t soc = zmk_battery_state_of_charge();
    uint8_t bar_length = ((uint32_t)soc * NUM_RGB_LEDS) / 100;

    if (soc > 0 && bar_length == 0) {
        bar_length = 1;
    }

    switch (state) {
    case IND_STATE_OFF:
        set_all_leds(0, 0);
        break;
    case IND_STATE_BAT_BAR:
        set_bar_leds(COLOR_BAT, bar_length, 255);
        break;
    case IND_STATE_LOW:
        set_all_leds(COLOR_LOW, 255);
        break;
    case IND_STATE_LAYER:
        set_all_leds(layer_color, 255);
        break;
    case IND_STATE_FULL:
        set_all_leds(COLOR_BAT, 255);
        break;
    case IND_STATE_CHARGING:
        set_bar_leds(COLOR_BAT, bar_length, pulse_brightness());
        break;
    }
}

extern struct k_timer indicator_timer;

static void indicator_work_handler(struct k_work *work) {
    enum indicator_state state = evaluate_state();

    if (state == IND_STATE_OFF) {
        k_timer_stop(&indicator_timer);
        if (last_state != IND_STATE_OFF) {
            render(IND_STATE_OFF);
            last_state = IND_STATE_OFF;
        }

        return;
    }

    if (state_needs_ticks(state)) {
        k_timer_start(&indicator_timer, K_MSEC(TICK_MS), K_MSEC(TICK_MS));
    } else {
        k_timer_stop(&indicator_timer);
    }

    render(state);

    last_state = state;
}

K_WORK_DEFINE(indicator_work, indicator_work_handler);

void zmk_battery_indicator_set_remote_layer_color(uint32_t color) {
    remote_layer_color = color;
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_work);
}

#if KEYMAP_LOCAL && LAYER_COLORS_ENABLED
// Invokes the hidden GLOBAL `lc` behavior; the central-side invocation fans out over
// the split transport to every peripheral (and runs locally), mirroring layer colors.
static void send_remote_layer_color(void) {
    const struct zmk_behavior_binding binding = {
        .behavior_dev = "lc",
        .param1 = highest_layer_color(),
        .param2 = 0,
    };
    const struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    int err = zmk_behavior_invoke_binding(&binding, event, true);
    if (err) {
        LOG_ERR("Failed to relay layer color: %d", err);
    }
}
#endif

static void indicator_timer_handler(struct k_timer *timer) {
    tick++;
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_work);
}

K_TIMER_DEFINE(indicator_timer, indicator_timer_handler, NULL);

int zmk_battery_indicator_show(void) {
    bat_show_until = k_uptime_get() + CONFIG_ZMK_BATTERY_INDICATOR_DURATION * 1000;
    return k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_work);
}

static int indicator_event_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *activity_ev = as_zmk_activity_state_changed(eh);

    if (activity_ev) {
        activity_state = activity_ev->state;
    }

#if KEYMAP_LOCAL && LAYER_COLORS_ENABLED
    // Relay on layer changes, on wake from idle, and when a peripheral reconnects
    // (it missed every update while asleep). The layer-state event implementation is
    // only compiled into the central build, so these references must stay in this
    // guard or the peripheral link fails.
    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    const struct zmk_split_peripheral_status_changed *periph_ev =
        as_zmk_split_peripheral_status_changed(eh);

    if (layer_ev || activity_ev || (periph_ev && periph_ev->connected)) {
        send_remote_layer_color();
    }
#endif

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(battery_indicator, indicator_event_listener);
#if KEYMAP_LOCAL
ZMK_SUBSCRIPTION(battery_indicator, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(battery_indicator, zmk_endpoint_changed);
#if LAYER_COLORS_ENABLED
ZMK_SUBSCRIPTION(battery_indicator, zmk_split_peripheral_status_changed);
#endif
#endif
ZMK_SUBSCRIPTION(battery_indicator, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(battery_indicator, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(battery_indicator, zmk_usb_conn_state_changed);
#endif

static int zmk_battery_indicator_init(void) {
    return k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_work);
}

SYS_INIT(zmk_battery_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
