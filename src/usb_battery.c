/*
 * Battery telemetry on a second USB HID interface. ZMK keeps HID_0 (keyboard,
 * consumer and mouse reports); this module owns HID_1 (central) and, when
 * configured, HID_2 (peripheral). It never enables USB itself.
 *
 * "Charging" means USB power is present, not measured charger current. The
 * peripheral relays its own VBUS reading over the split link. Qi is not sensed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#include <zmk/workqueue.h>

#if defined(CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY)
#include <zmk/events/split_battery_state_changed.h>
#include <zmk/split_battery.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define USB_BATTERY_REPORT_ID 1
#define USB_BATTERY_INPUT_REPORT (0x0100 | USB_BATTERY_REPORT_ID)
#define USB_BATTERY_STATUS_ID 2
#define USB_BATTERY_FEATURE_REPORT (0x0300 | USB_BATTERY_STATUS_ID)
#define USB_BATTERY_REFRESH K_SECONDS(30)
#define USB_BATTERY_RETRY K_SECONDS(1)

/*
 * Linux generic HID maps Battery Strength to a device-scoped power_supply and
 * Battery System / Charging to its status. Both must share one report ID.
 * Capacity is the first payload byte for Linux's GET_REPORT capacity query.
 * Encode percent + 1 in logical range 1..101: Linux scales this back to 0..100,
 * and older kernels that ignore unsolicited raw zero still see an empty battery.
 *
 * A battery-only collection has no ordinary input capabilities and Linux can
 * discard it (and its power_supply). A Consumer Control collection with Mute
 * permanently released keeps the interface registered without emitting keys.
 */
static const uint8_t usb_battery_report_desc[] = {
    0x05, 0x0c, /* Usage Page (Consumer) */
    0x09, 0x01, /* Usage (Consumer Control) */
    0xa1, 0x01, /* Collection (Application) */
    0x85, USB_BATTERY_REPORT_ID,
    0x05, 0x06, /* Usage Page (Generic Device Controls) */
    0x09, 0x20, /* Usage (Battery Strength) */
    0x15, 0x01, /* Logical Minimum (1) */
    0x25, 0x65, /* Logical Maximum (101) */
    0x75, 0x08, /* Report Size (8) */
    0x95, 0x01, /* Report Count (1) */
    0x81, 0x02, /* Input (Data, Variable, Absolute) */
    0x05, 0x85, /* Usage Page (Battery System) */
    0x09, 0x44, /* Usage (Charging) */
    0x15, 0x00, /* Logical Minimum (0), reset after capacity's offset range */
    0x25, 0x01, /* Logical Maximum (1) */
    0x75, 0x01, /* Report Size (1) */
    0x81, 0x02, /* Input (Data, Variable, Absolute) */
    0x75, 0x07, /* Report Size (7) */
    0x81, 0x03, /* Input (Constant) */
    0x05, 0x0c, /* Usage Page (Consumer) */
    0x09, 0xe2, /* Usage (Mute) -- always released */
    0x75, 0x01, /* Report Size (1) */
    0x81, 0x02, /* Input (Data, Variable, Absolute) */
    0x75, 0x07, /* Report Size (7) */
    0x81, 0x03, /* Input (Constant) */
    0x06, 0x00,
    0xff,       /* Usage Page (Vendor Defined) */
    0x09, 0x01, /* Usage (Telemetry validity and snapshot) */
    0x85, USB_BATTERY_STATUS_ID,
    0x26, 0xff,
    0x00,       /* Logical Maximum (255) */
    0x75, 0x08, /* Report Size (8) */
    0x95, 0x03, /* Report Count (3) */
    0xb1, 0x02, /* Feature (Data, Variable, Absolute) */
    0xc0,       /* End Collection */
};

struct usb_battery_report {
    uint8_t report_id;
    uint8_t level; /* Percent + 1 on Input; the vendor Feature returns plain percent. */
    uint8_t charging;
    uint8_t consumer; /* Always zero; never generate a Mute press. */
};

BUILD_ASSERT(sizeof(struct usb_battery_report) == 4);
BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT >= 2 && CONFIG_USB_HID_DEVICE_COUNT <= 3,
             "USB battery owns HID_1 and optional HID_2; HID_0 belongs to ZMK");
#if defined(CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY)
BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT == 3, "Split battery telemetry needs HID_2");
#endif
#if CONFIG_USB_HID_DEVICE_COUNT == 3
/* Linux 6.19 uses HID_UNIQ (the shared USB serial) as the power_supply name.
 * Without a serial it falls back to each distinct HID device path. Merely using
 * two interfaces or changing their report IDs cannot prevent that name collision.
 */
BUILD_ASSERT(sizeof(CONFIG_USB_DEVICE_SN) == 1,
             "Two USB batteries require USB_DEVICE_SN=\"\" to avoid duplicate Linux battery names");
#endif
BUILD_ASSERT(CONFIG_HID_INTERRUPT_EP_MPS >= sizeof(struct usb_battery_report));
BUILD_ASSERT(CONFIG_ZMK_USB_HID_INIT_PRIORITY < CONFIG_ZMK_USB_INIT_PRIORITY,
             "Register USB battery before ZMK enables USB");

static const struct device *battery_hid_dev;
static const struct device *peer_hid_dev;

static bool battery_snapshot(const struct device *dev, struct usb_battery_report *report) {
    *report = (struct usb_battery_report){.report_id = USB_BATTERY_REPORT_ID};
    if (dev != peer_hid_dev) {
        report->level = MIN(zmk_battery_state_of_charge(), 100) + 1;
        report->charging = zmk_usb_is_powered() ? 1 : 0;
        return true;
    }

#if defined(CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY)
    struct zmk_split_battery_state state;
    if (zmk_split_battery_get_peer(&state)) {
        report->level = state.level + 1;
        report->charging = state.charging ? 1 : 0;
        return true;
    }
#endif
    return false;
}

static int battery_get_report(const struct device *dev, struct usb_setup_packet *setup,
                              int32_t *len, uint8_t **data) {
    if (setup->wValue != USB_BATTERY_INPUT_REPORT && setup->wValue != USB_BATTERY_FEATURE_REPORT) {
        return -ENOTSUP;
    }

    struct usb_battery_report snapshot;
    bool available = battery_snapshot(dev, &snapshot);
    if (setup->wValue == USB_BATTERY_INPUT_REPORT && !available) {
        /* Do not invent a 0%/discharging sample for an absent or stale peer. */
        return -ENODATA;
    }

    /* EP0 requests are serialized. Keep this buffer separate from interrupt DMA. */
    static uint8_t control_report[4];
    if (setup->wValue == USB_BATTERY_FEATURE_REPORT) {
        /* Raw readers can distinguish missing data from a genuine empty battery. */
        control_report[0] = USB_BATTERY_STATUS_ID;
        control_report[1] = available ? 1 : 0;
        control_report[2] = available ? snapshot.level - 1 : 0;
        control_report[3] = available ? snapshot.charging : 0;
    } else {
        memcpy(control_report, &snapshot, sizeof(snapshot));
    }
    *data = control_report;
    *len = MIN(setup->wLength, sizeof(control_report));
    return 0;
}

static const struct hid_ops battery_hid_ops = {
    .get_report = battery_get_report,
};

static void battery_send_work(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(usb_battery_work, battery_send_work);

static int battery_send_interface(const struct device *dev, uint8_t interface_index) {
    struct usb_battery_report snapshot;
    if (!dev || !battery_snapshot(dev, &snapshot)) {
        return 0;
    }

    /*
     * The nRF USB driver can retain a RAM buffer until interrupt completion.
     * Fill only the inactive buffer; advance on acceptance, not on -EAGAIN.
     * One serialized work item owns both buffers. No completion semaphore can
     * become stranded when USB reset/disconnect aborts an outstanding transfer.
     */
    static struct usb_battery_report reports[2][2];
    static uint8_t next_report[2];
    struct usb_battery_report *report = &reports[interface_index][next_report[interface_index]];
    *report = snapshot;
    int err = hid_int_ep_write(dev, (const uint8_t *)report, sizeof(*report), NULL);
    if (!err) {
        next_report[interface_index] ^= 1;
    } else {
        LOG_DBG("USB battery interface %d report deferred: %d", interface_index + 1, err);
    }
    return err;
}

static void battery_send_work(struct k_work *work) {
    if (!battery_hid_dev || !zmk_usb_is_hid_ready() || zmk_usb_get_status() == USB_DC_SUSPEND) {
        /* Never wake the host for telemetry. USB events restart us on resume. */
        return;
    }

    int central_err = battery_send_interface(battery_hid_dev, 0);
    int peer_err = battery_send_interface(peer_hid_dev, 1);

    /*
     * Periodic reports also reach hosts that open the input device after
     * enumeration. schedule (not reschedule) preserves an immediate event
     * queued while this work item was running.
     */
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &usb_battery_work,
                              (central_err || peer_err) ? USB_BATTERY_RETRY : USB_BATTERY_REFRESH);
}

static int usb_battery_listener(const zmk_event_t *eh) {
    if (battery_hid_dev) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &usb_battery_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(usb_battery, usb_battery_listener);
ZMK_SUBSCRIPTION(usb_battery, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(usb_battery, zmk_battery_state_changed);
#if defined(CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY)
ZMK_SUBSCRIPTION(usb_battery, zmk_split_battery_state_changed);
#endif

static int usb_battery_init(void) {
    const struct device *dev = device_get_binding(CONFIG_USB_HID_DEVICE_NAME "_1");
    if (!dev || !device_is_ready(dev)) {
        LOG_ERR("USB battery HID device unavailable");
        return -ENODEV;
    }

    const struct device *peer = NULL;
#if CONFIG_USB_HID_DEVICE_COUNT == 3
    peer = device_get_binding(CONFIG_USB_HID_DEVICE_NAME "_2");
    if (!peer || !device_is_ready(peer)) {
        LOG_ERR("Peripheral battery HID device unavailable");
        return -ENODEV;
    }
#endif

    usb_hid_register_device(dev, usb_battery_report_desc, sizeof(usb_battery_report_desc),
                            &battery_hid_ops);
    int err = usb_hid_init(dev);
    if (err) {
        return err;
    }
    if (peer) {
        usb_hid_register_device(peer, usb_battery_report_desc, sizeof(usb_battery_report_desc),
                                &battery_hid_ops);
        err = usb_hid_init(peer);
        if (err) {
            return err;
        }
    }

    battery_hid_dev = dev;
    peer_hid_dev = peer;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &usb_battery_work, K_NO_WAIT);
    return 0;
}

SYS_INIT(usb_battery_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);
