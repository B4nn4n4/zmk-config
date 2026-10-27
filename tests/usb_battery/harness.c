/* Include the production translation unit so its static functions are exercised. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs.h"
#include "src/usb_battery.c"

static struct device fake_device;
static struct k_work_q fake_queue;
static bool binding_exists = true, device_ready = true, hid_ready = true, powered = true;
static uint8_t percentage = 53;
static enum usb_dc_status_code usb_status = USB_DC_CONFIGURED;
static int init_result, send_result, registrations, initializations, sends, wakeups;
static int schedules, reschedules;
static bool pending;
static k_timeout_t pending_delay, last_schedule;
static const uint8_t *registered_desc;
static size_t registered_desc_size;
static const struct hid_ops *registered_ops;
static const uint8_t *outstanding;
static uint8_t outstanding_copy[4];
static const uint8_t *last_attempt;
static void (*during_write)(void);

const struct device *device_get_binding(const char *name) {
    assert(strcmp(name, "HID_1") == 0);
    return binding_exists ? &fake_device : NULL;
}
bool device_is_ready(const struct device *dev) {
    assert(dev == &fake_device);
    return device_ready;
}
void usb_hid_register_device(const struct device *dev, const uint8_t *desc, size_t size,
                             const struct hid_ops *ops) {
    assert(dev == &fake_device);
    registrations++;
    registered_desc = desc;
    registered_desc_size = size;
    registered_ops = ops;
}
int usb_hid_init(const struct device *dev) {
    assert(dev == &fake_device);
    assert(registrations == 1);
    initializations++;
    return init_result;
}
static void check_outstanding(void) {
    if (outstanding) {
        assert(memcmp(outstanding, outstanding_copy, sizeof(outstanding_copy)) == 0);
    }
}
int hid_int_ep_write(const struct device *dev, const uint8_t *data, uint32_t size,
                     uint32_t *written) {
    assert(dev == &fake_device);
    assert(size == 4 && written == NULL);
    assert(data[0] == 1 && data[1] == (percentage > 100 ? 100 : percentage));
    assert(data[2] == (powered ? 1 : 0) && data[3] == 0);
    check_outstanding(); /* Detect overwrite before even calling the driver. */
    sends++;
    last_attempt = data;
    if (during_write) {
        during_write();
    }
    if (!send_result) {
        /* Successful call means the prior transfer completed; retain the new RAM. */
        outstanding = data;
        memcpy(outstanding_copy, data, sizeof(outstanding_copy));
    }
    return send_result;
}
uint8_t zmk_battery_state_of_charge(void) { return percentage; }
bool zmk_usb_is_powered(void) { return powered; }
bool zmk_usb_is_hid_ready(void) { return hid_ready; }
enum usb_dc_status_code zmk_usb_get_status(void) { return usb_status; }
struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &fake_queue; }
int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                              k_timeout_t delay) {
    assert(queue == &fake_queue && work == &usb_battery_work);
    schedules++;
    last_schedule = delay;
    if (!pending) {
        pending = true;
        pending_delay = delay;
    }
    return 0;
}
int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                                k_timeout_t delay) {
    assert(queue == &fake_queue && work == &usb_battery_work);
    reschedules++;
    pending = true;
    pending_delay = delay;
    return 0;
}
int usb_wakeup_request(void) { wakeups++; return 0; }

static void init_ok(void) {
    assert(registered_init == usb_battery_init);
    assert(registered_init_priority == CONFIG_ZMK_USB_HID_INIT_PRIORITY);
    assert(registered_init() == 0);
    assert(battery_hid_dev == &fake_device);
    assert(registrations == 1 && initializations == 1);
    assert(registered_desc == usb_battery_report_desc);
    assert(registered_desc_size == sizeof(usb_battery_report_desc));
    assert(registered_ops == &battery_hid_ops);
    assert(registered_ops->get_report == battery_get_report);
    assert(reschedules == 1 && schedules == 0 && pending && pending_delay == 0);
    assert(sends == 0 && wakeups == 0);
}
static void run_work(void) {
    pending = false; /* Zephyr dequeues the timeout before invoking its callback. */
    usb_battery_work.work.handler(&usb_battery_work.work);
}
static void event(void) {
    zmk_event_t eh = 0;
    assert(registered_listener(&eh) == ZMK_EV_EVENT_BUBBLE);
}
static void get_and_check(uint8_t pct, bool charging, uint16_t requested) {
    percentage = pct;
    powered = charging;
    struct usb_setup_packet setup = {.wValue = 0x0101, .wLength = requested};
    uint8_t *data = NULL;
    int32_t len = -1;
    assert(battery_hid_ops.get_report(&fake_device, &setup, &len, &data) == 0);
    assert(data && len == (requested < 4 ? requested : 4));
    const uint8_t expected[] = {1, pct > 100 ? 100 : pct, charging ? 1 : 0, 0};
    assert(memcmp(data, expected, sizeof(expected)) == 0);
    check_outstanding();
}
static void test_get_report(void) {
    const uint8_t levels[] = {0, 1, 53, 99, 100, 101, 255};
    const uint16_t lengths[] = {0, 1, 2, 3, 4, 5, 64, 65535};
    for (size_t p = 0; p < sizeof(levels); p++) {
        for (int c = 0; c < 2; c++) {
            for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
                get_and_check(levels[p], c, lengths[l]);
            }
        }
    }
    /* Exhaust the 16-bit report-type/report-ID selector, not only common errors. */
    for (unsigned value = 0; value <= UINT16_MAX; value++) {
        if (value == 0x0101) {
            continue;
        }
        struct usb_setup_packet setup = {.wValue = value, .wLength = 4};
        uint8_t sentinel = 0xaa, *data = &sentinel;
        int32_t len = 37;
        assert(battery_get_report(&fake_device, &setup, &len, &data) == -ENOTSUP);
        assert(len == 37 && data == &sentinel && sentinel == 0xaa);
    }
}
static void test_gated(const char *mode) {
    if (strcmp(mode, "missing") != 0) {
        init_ok();
    }
    if (strcmp(mode, "disconnect") == 0) {
        hid_ready = false;
        usb_status = USB_DC_DISCONNECTED;
    } else if (strcmp(mode, "not-ready") == 0) {
        hid_ready = false;
    } else if (strcmp(mode, "suspend") == 0) {
        /* Keep readiness true to exercise the explicit suspend check. */
        usb_status = USB_DC_SUSPEND;
    }
    int prior_reschedules = reschedules;
    run_work();
    assert(sends == 0 && schedules == 0 && reschedules == prior_reschedules);
    assert(!pending && wakeups == 0);
    if (battery_hid_dev) {
        hid_ready = true;
        usb_status = USB_DC_CONFIGURED;
        event();
        assert(pending && pending_delay == 0);
        run_work();
        assert(sends == 1 && schedules == 1 && last_schedule == 30000);
    } else {
        event();
        assert(!pending && reschedules == 0);
    }
}
static void test_send(int error, bool inject_event) {
    init_ok();
    send_result = error;
    if (inject_event) {
        during_write = event;
    }
    run_work();
    assert(sends == 1 && schedules == 1);
    assert(reschedules == (inject_event ? 2 : 1));
    assert(last_schedule == (error ? 1000 : 30000));
    assert(pending && pending_delay == (inject_event ? 0 : last_schedule));
    assert(wakeups == 0);
    if (error) {
        const uint8_t *retry_buffer = last_attempt;
        send_result = 0;
        during_write = NULL;
        run_work();
        assert(sends == 2 && last_attempt == retry_buffer);
        assert(last_schedule == 30000 && pending_delay == 30000);
    }
}
static void test_lifetime(void) {
    init_ok();
    run_work();
    const uint8_t *first = outstanding;
    send_result = -EAGAIN;
    for (int i = 0; i < 3; i++) {
        percentage = 65 + i;
        run_work();
        assert(last_attempt != first && outstanding == first);
        check_outstanding();
        get_and_check(99, false, 4); /* EP0 must not touch interrupt DMA storage. */
    }
    const uint8_t *second = last_attempt;
    hid_ready = false;
    usb_status = USB_DC_DISCONNECTED;
    run_work();
    check_outstanding();
    assert(!pending);
    /* The real USB stack aborts the outstanding transfer on disconnect. */
    outstanding = NULL;
    hid_ready = true;
    usb_status = USB_DC_CONFIGURED;
    send_result = 0;
    percentage = 22;
    event();
    run_work();
    assert(last_attempt == second && outstanding == second);
    assert(outstanding[1] == 22 && outstanding[2] == 0);
    send_result = -EIO;
    percentage = 37;
    run_work();
    assert(last_attempt == first && outstanding == second);
    check_outstanding();
    get_and_check(100, true, 4);
    send_result = 0;
    run_work();
    assert(last_attempt == first && outstanding[1] == 100 && outstanding[2] == 1);
}
static void test_event(void) {
    event();
    assert(reschedules == 0 && !pending);
    init_ok();
    run_work();
    assert(pending_delay == 30000);
    event();
    assert(pending_delay == 0 && reschedules == 2 && schedules == 1);
    event();
    assert(pending_delay == 0 && reschedules == 3);
    assert(sends == 1); /* Listener queues work; never writes in event context. */
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *test = argv[1];
    if (strcmp(test, "init") == 0) {
        init_ok();
    } else if (strcmp(test, "init-missing") == 0 || strcmp(test, "init-unready") == 0 ||
               strcmp(test, "init-error") == 0) {
        binding_exists = strcmp(test, "init-missing") != 0;
        device_ready = strcmp(test, "init-unready") != 0;
        init_result = strcmp(test, "init-error") == 0 ? -EIO : 0;
        assert(usb_battery_init() == (init_result ? -EIO : -ENODEV));
        assert(battery_hid_dev == NULL && !pending && sends == 0);
        assert(reschedules == 0 && schedules == 0);
        assert(registrations == (init_result ? 1 : 0));
        assert(initializations == registrations);
        event();
        run_work();
        assert(sends == 0 && !pending && reschedules == 0 && schedules == 0);
    } else if (strcmp(test, "get-report") == 0) {
        test_get_report();
    } else if (strcmp(test, "missing") == 0 || strcmp(test, "disconnect") == 0 ||
               strcmp(test, "suspend") == 0 || strcmp(test, "not-ready") == 0) {
        test_gated(test);
    } else if (strcmp(test, "send") == 0) {
        test_send(0, false);
    } else if (strcmp(test, "retry") == 0) {
        test_send(-EAGAIN, false);
    } else if (strcmp(test, "send-error") == 0) {
        test_send(-EIO, false);
    } else if (strcmp(test, "event-during-send") == 0) {
        test_send(0, true);
    } else if (strcmp(test, "event-during-retry") == 0) {
        test_send(-EAGAIN, true);
    } else if (strcmp(test, "lifetime") == 0) {
        test_lifetime();
    } else if (strcmp(test, "event") == 0) {
        test_event();
    } else if (strcmp(test, "descriptor") == 0) {
        init_ok();
        for (size_t i = 0; i < registered_desc_size; i++) {
            printf("%02x", registered_desc[i]);
        }
        putchar('\n');
    } else {
        abort();
    }
    return 0;
}
