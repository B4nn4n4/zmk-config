/* Minimal host-side API seam, not a substitute for Zephyr integration tests. */
#ifndef USB_BATTERY_TEST_STUBS_H
#define USB_BATTERY_TEST_STUBS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_ZMK_LOG_LEVEL 4
#define CONFIG_USB_HID_DEVICE_COUNT 2
#define CONFIG_HID_INTERRUPT_EP_MPS 64
#define CONFIG_ZMK_USB_HID_INIT_PRIORITY 50
#define CONFIG_ZMK_USB_INIT_PRIORITY 60
#define CONFIG_USB_HID_DEVICE_NAME "HID"
#define LOG_MODULE_DECLARE(...)
#define LOG_DBG(...)
#define LOG_ERR(...)
#define BUILD_ASSERT(condition, ...) _Static_assert(condition, #condition)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define K_SECONDS(s) ((s) * 1000)
#define K_NO_WAIT 0
#define APPLICATION 1
#define SYS_INIT(fn, level, priority) \
    static int (*const registered_init)(void) = fn; \
    static const int registered_init_priority = priority
#define ZMK_LISTENER(name, fn) static int (*const registered_listener)(const zmk_event_t *) = fn
#define ZMK_SUBSCRIPTION(...)
#define ZMK_EV_EVENT_BUBBLE 0

typedef int zmk_event_t;
typedef int k_timeout_t;
struct device { int unused; };
struct k_work { void (*handler)(struct k_work *); };
struct k_work_delayable { struct k_work work; };
struct k_work_q { int unused; };
#define K_WORK_DELAYABLE_DEFINE(name, fn) \
    static struct k_work_delayable name = {.work = {.handler = fn}}
struct usb_setup_packet { uint16_t wValue; uint16_t wLength; };
struct hid_ops {
    int (*get_report)(const struct device *, struct usb_setup_packet *, int32_t *, uint8_t **);
};
enum usb_dc_status_code { USB_DC_DISCONNECTED, USB_DC_CONFIGURED, USB_DC_SUSPEND };

const struct device *device_get_binding(const char *name);
bool device_is_ready(const struct device *dev);
void usb_hid_register_device(const struct device *dev, const uint8_t *desc, size_t size,
                             const struct hid_ops *ops);
int usb_hid_init(const struct device *dev);
int hid_int_ep_write(const struct device *dev, const uint8_t *data, uint32_t size,
                     uint32_t *written);
uint8_t zmk_battery_state_of_charge(void);
bool zmk_usb_is_powered(void);
bool zmk_usb_is_hid_ready(void);
enum usb_dc_status_code zmk_usb_get_status(void);
struct k_work_q *zmk_workqueue_lowprio_work_q(void);
int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                              k_timeout_t delay);
int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                                k_timeout_t delay);
int usb_wakeup_request(void);
#endif
