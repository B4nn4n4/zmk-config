/*
 * Battery telemetry on a second USB HID interface. ZMK keeps HID_0 (keyboard,
 * consumer and mouse reports); this module owns HID_1 and never enables USB itself.
 *
 * Like the BLE charge-state module, "charging" means central-half USB power is
 * present, not measured charger current. Qi and peripheral power are unknown.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>

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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define USB_BATTERY_REPORT_ID 1
#define USB_BATTERY_INPUT_REPORT (0x0100 | USB_BATTERY_REPORT_ID)
#define USB_BATTERY_REFRESH K_SECONDS(30)
#define USB_BATTERY_RETRY K_SECONDS(1)

/*
 * Linux generic HID maps Battery Strength to a device-scoped power_supply and
 * Battery System / Charging to its status. Both must share one report ID.
 * Capacity is the first payload byte for Linux's GET_REPORT capacity query.
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
    0x15, 0x00, /* Logical Minimum (0) */
    0x25, 0x64, /* Logical Maximum (100) */
    0x75, 0x08, /* Report Size (8) */
    0x95, 0x01, /* Report Count (1) */
    0x81, 0x02, /* Input (Data, Variable, Absolute) */
    0x05, 0x85, /* Usage Page (Battery System) */
    0x09, 0x44, /* Usage (Charging) */
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
    0xc0,       /* End Collection */
};

struct usb_battery_report {
    uint8_t report_id;
    uint8_t level;
    uint8_t charging;
    uint8_t consumer; /* Always zero; never generate a Mute press. */
};

BUILD_ASSERT(sizeof(struct usb_battery_report) == 4);
BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT == 2, "USB battery requires exactly HID_0 and HID_1");
BUILD_ASSERT(CONFIG_HID_INTERRUPT_EP_MPS >= sizeof(struct usb_battery_report));
BUILD_ASSERT(CONFIG_ZMK_USB_HID_INIT_PRIORITY < CONFIG_ZMK_USB_INIT_PRIORITY,
             "Register USB battery before ZMK enables USB");

static const struct device *battery_hid_dev;

static struct usb_battery_report battery_snapshot(void) {
    return (struct usb_battery_report){
        .report_id = USB_BATTERY_REPORT_ID,
        .level = MIN(zmk_battery_state_of_charge(), 100),
        .charging = zmk_usb_is_powered() ? 1 : 0,
        .consumer = 0,
    };
}

static int battery_get_report(const struct device *dev, struct usb_setup_packet *setup,
                              int32_t *len, uint8_t **data) {
    if (setup->wValue != USB_BATTERY_INPUT_REPORT) {
        return -ENOTSUP;
    }

    /* EP0 requests are serialized. Keep this buffer separate from interrupt DMA. */
    static struct usb_battery_report control_report;
    control_report = battery_snapshot();
    *data = (uint8_t *)&control_report;
    *len = MIN(setup->wLength, sizeof(control_report));
    return 0;
}

static const struct hid_ops battery_hid_ops = {
    .get_report = battery_get_report,
};

static void battery_send_work(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(usb_battery_work, battery_send_work);

static void battery_send_work(struct k_work *work) {
    if (!battery_hid_dev || !zmk_usb_is_hid_ready() || zmk_usb_get_status() == USB_DC_SUSPEND) {
        /* Never wake the host for telemetry. USB events restart us on resume. */
        return;
    }

    /*
     * The nRF USB driver can retain a RAM buffer until interrupt completion.
     * Fill only the inactive buffer; advance on acceptance, not on -EAGAIN.
     * One serialized work item owns both buffers. No completion semaphore can
     * become stranded when USB reset/disconnect aborts an outstanding transfer.
     */
    static struct usb_battery_report reports[2];
    static uint8_t next_report;
    reports[next_report] = battery_snapshot();
    int err = hid_int_ep_write(battery_hid_dev, (const uint8_t *)&reports[next_report],
                               sizeof(reports[next_report]), NULL);
    if (!err) {
        next_report ^= 1;
    } else {
        LOG_DBG("USB battery report deferred: %d", err);
    }

    /*
     * Periodic reports also reach hosts that open the input device after
     * enumeration. schedule (not reschedule) preserves an immediate event
     * queued while this work item was running.
     */
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &usb_battery_work,
                              err ? USB_BATTERY_RETRY : USB_BATTERY_REFRESH);
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

static int usb_battery_init(void) {
    const struct device *dev = device_get_binding(CONFIG_USB_HID_DEVICE_NAME "_1");
    if (!dev || !device_is_ready(dev)) {
        LOG_ERR("USB battery HID device unavailable");
        return -ENODEV;
    }

    usb_hid_register_device(dev, usb_battery_report_desc, sizeof(usb_battery_report_desc),
                            &battery_hid_ops);
    int err = usb_hid_init(dev);
    if (err) {
        return err;
    }

    battery_hid_dev = dev;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &usb_battery_work, K_NO_WAIT);
    return 0;
}

SYS_INIT(usb_battery_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);
