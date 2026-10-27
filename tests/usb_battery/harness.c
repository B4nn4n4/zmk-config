/* Include the production translation unit so its static functions are exercised. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs.h"
#include "src/usb_battery.c"

#define INTERFACES (CONFIG_USB_HID_DEVICE_COUNT - 1)
struct endpoint {
    struct device dev;
    bool binding_exists, ready;
    int init_result, send_result, registrations, initializations, sends;
    const uint8_t *desc;
    size_t desc_size;
    const struct hid_ops *ops;
    const uint8_t *outstanding, *last_attempt;
    uint8_t outstanding_copy[4];
};
static struct endpoint endpoints[2] = {
    {.binding_exists = true, .ready = true},
    {.binding_exists = true, .ready = true},
};
static struct k_work_q fake_queue;
static bool hid_ready = true, powered = true, peer_available = true;
static uint8_t percentage = 53;
static struct zmk_split_battery_state peer_state = {.level = 79, .charging = false};
static enum usb_dc_status_code usb_status = USB_DC_CONFIGURED;
static int wakeups, schedules, reschedules, peer_gets;
static bool pending;
static k_timeout_t pending_delay, last_schedule;
static void (*during_write)(void);
static int inject_interface;

static int device_index(const struct device *dev) {
    for (int i = 0; i < INTERFACES; i++) {
        if (dev == &endpoints[i].dev) { return i; }
    }
    abort(); /* HID_0 and unknown devices must never be touched. */
}
const struct device *device_get_binding(const char *name) {
    int i = strcmp(name, "HID_1") == 0 ? 0 : strcmp(name, "HID_2") == 0 ? 1 : -1;
    assert(i >= 0 && i < INTERFACES);
    return endpoints[i].binding_exists ? &endpoints[i].dev : NULL;
}
bool device_is_ready(const struct device *dev) { return endpoints[device_index(dev)].ready; }
void usb_hid_register_device(const struct device *dev, const uint8_t *desc, size_t size,
                             const struct hid_ops *ops) {
    struct endpoint *ep = &endpoints[device_index(dev)];
    assert(ep->registrations == 0 && ep->initializations == 0);
    ep->registrations++;
    ep->desc = desc;
    ep->desc_size = size;
    ep->ops = ops;
}
int usb_hid_init(const struct device *dev) {
    struct endpoint *ep = &endpoints[device_index(dev)];
    assert(ep->registrations == 1 && ep->initializations == 0);
    ep->initializations++;
    return ep->init_result;
}
static void check_outstanding(void) {
    for (int i = 0; i < INTERFACES; i++) {
        struct endpoint *ep = &endpoints[i];
        if (ep->outstanding) {
            assert(memcmp(ep->outstanding, ep->outstanding_copy, 4) == 0);
        }
    }
}
int hid_int_ep_write(const struct device *dev, const uint8_t *data, uint32_t size,
                     uint32_t *written) {
    int i = device_index(dev);
    struct endpoint *ep = &endpoints[i];
    assert(size == 4 && written == NULL);
    assert(data[0] == 1 && data[3] == 0);
    assert(data[1] == 1 + (i ? peer_state.level : (percentage > 100 ? 100 : percentage)));
    assert(data[2] == (i ? peer_state.charging : powered));
    assert(!i || peer_available);
    check_outstanding(); /* Detect overwrite before even calling the driver. */
    for (int other = 0; other < INTERFACES; other++) {
        if (other != i) {
            assert(data != endpoints[other].last_attempt);
            assert(data != endpoints[other].outstanding);
        }
    }
    ep->sends++;
    ep->last_attempt = data;
    if (during_write && inject_interface == i) { during_write(); }
    if (!ep->send_result) {
        /* Acceptance completes the prior transfer only on this endpoint. */
        ep->outstanding = data;
        memcpy(ep->outstanding_copy, data, 4);
    }
    check_outstanding();
    return ep->send_result;
}
uint8_t zmk_battery_state_of_charge(void) { return percentage; }
bool zmk_split_battery_get_peer(struct zmk_split_battery_state *state) {
    peer_gets++;
    /* Deliberately return retained/invalid bytes even when unavailable. */
    *state = peer_state;
    return peer_available;
}
bool zmk_usb_is_powered(void) { return powered; }
bool zmk_usb_is_hid_ready(void) { return hid_ready; }
enum usb_dc_status_code zmk_usb_get_status(void) { return usb_status; }
struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &fake_queue; }
int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                              k_timeout_t delay) {
    assert(queue == &fake_queue && work == &usb_battery_work);
    schedules++;
    last_schedule = delay;
    if (!pending) { pending = true; pending_delay = delay; }
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

static int total_sends(void) { return endpoints[0].sends + endpoints[1].sends; }
static void init_ok(void) {
    assert(registered_init == usb_battery_init);
    assert(registered_init_priority == CONFIG_ZMK_USB_HID_INIT_PRIORITY);
    assert(registered_init() == 0);
    assert(battery_hid_dev == &endpoints[0].dev);
    assert(peer_hid_dev == (INTERFACES == 2 ? &endpoints[1].dev : NULL));
    for (int i = 0; i < INTERFACES; i++) {
        struct endpoint *ep = &endpoints[i];
        assert(ep->registrations == 1 && ep->initializations == 1);
        assert(ep->desc == usb_battery_report_desc && ep->desc_size == sizeof(usb_battery_report_desc));
        assert(ep->ops == &battery_hid_ops && ep->ops->get_report == battery_get_report);
    }
    assert(reschedules == 1 && schedules == 0 && pending && pending_delay == 0);
    assert(total_sends() == 0 && wakeups == 0);
}
static void run_work(void) {
    pending = false; /* Zephyr dequeues the timeout before invoking its callback. */
    usb_battery_work.work.handler(&usb_battery_work.work);
    check_outstanding();
    assert(wakeups == 0);
}
static void event(void) {
    zmk_event_t eh = 0;
    assert(registered_listener(&eh) == ZMK_EV_EVENT_BUBBLE);
}
static void get_and_check(int i, uint16_t selector, uint16_t requested) {
    struct endpoint *ep = &endpoints[i];
    struct usb_setup_packet setup = {.wValue = selector, .wLength = requested};
    uint8_t sentinel = 0xaa, *data = &sentinel;
    int32_t len = 37;
    bool available = !i || peer_available;
    int result = ep->ops->get_report(&ep->dev, &setup, &len, &data);
    if (selector == 0x0101 && !available) {
        assert(result == -ENODATA && data == &sentinel && len == 37 && sentinel == 0xaa);
    } else {
        assert(result == 0 && data && len == (requested < 4 ? requested : 4));
        uint8_t pct = i ? peer_state.level : (percentage > 100 ? 100 : percentage);
        uint8_t charging = i ? peer_state.charging : powered;
        const uint8_t input[] = {1, pct + 1, charging, 0};
        const uint8_t feature[] = {2, available, available ? pct : 0, available ? charging : 0};
        assert(memcmp(data, selector == 0x0101 ? input : feature, 4) == 0);
    }
    check_outstanding(); /* EP0 must not touch either interrupt DMA buffer. */
}
static void read_all(void) {
    for (int i = 0; i < INTERFACES; i++) {
        get_and_check(i, 0x0101, 4);
        get_and_check(i, 0x0302, 4);
    }
}
static void test_get_report(void) {
    init_ok();
    const uint8_t levels[] = {0, 1, 53, 99, 100, 101, 255};
    const uint16_t lengths[] = {0, 1, 2, 3, 4, 5, 64, 65535};
    for (size_t p = 0; p < sizeof(levels); p++) {
        percentage = levels[p];
        /* Peer API contract guarantees valid available levels, not raw sensor bytes. */
        peer_state.level = levels[p] > 100 ? 100 : levels[p];
        for (int c = 0; c < 2; c++) {
            powered = c;
            peer_state.charging = !c;
            for (int available = 0; available < 2; available++) {
                peer_available = available;
                for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
                    for (int i = 0; i < INTERFACES; i++) {
                        get_and_check(i, 0x0101, lengths[l]);
                        get_and_check(i, 0x0302, lengths[l]);
                    }
                }
            }
        }
    }
    /* Exhaust the full report-type/report-ID selector on each registered device. */
    for (int i = 0; i < INTERFACES; i++) {
        for (unsigned value = 0; value <= UINT16_MAX; value++) {
            if (value == 0x0101 || value == 0x0302) { continue; }
            struct usb_setup_packet setup = {.wValue = value, .wLength = 4};
            uint8_t sentinel = 0xaa, *data = &sentinel;
            int32_t len = 37;
            assert(endpoints[i].ops->get_report(&endpoints[i].dev, &setup, &len, &data) == -ENOTSUP);
            assert(len == 37 && data == &sentinel && sentinel == 0xaa);
        }
    }
    assert(total_sends() == 0);
    if (INTERFACES == 1) { assert(peer_gets == 0); }
}
static void test_init_failure(int i, const char *kind) {
    assert(i < INTERFACES);
    struct endpoint *ep = &endpoints[i];
    ep->binding_exists = strcmp(kind, "missing") != 0;
    ep->ready = strcmp(kind, "unready") != 0;
    ep->init_result = strcmp(kind, "error") == 0 ? -EIO : 0;
    assert(usb_battery_init() == (ep->init_result ? -EIO : -ENODEV));
    assert(battery_hid_dev == NULL && peer_hid_dev == NULL);
    for (int j = 0; j < INTERFACES; j++) {
        int registered = ep->init_result && j <= i ? 1 : 0;
        assert(endpoints[j].registrations == registered && endpoints[j].initializations == registered);
    }
    event();
    run_work();
    assert(total_sends() == 0 && !pending && reschedules == 0 && schedules == 0);
}
static void test_gated(const char *mode) {
    if (strcmp(mode, "missing") != 0) { init_ok(); }
    if (strcmp(mode, "disconnect") == 0) {
        hid_ready = false; usb_status = USB_DC_DISCONNECTED;
    } else if (strcmp(mode, "not-ready") == 0) {
        hid_ready = false;
    } else if (strcmp(mode, "suspend") == 0) {
        /* Keep readiness true to exercise the explicit suspend check. */
        usb_status = USB_DC_SUSPEND;
    }
    int prior_reschedules = reschedules;
    run_work();
    assert(total_sends() == 0 && schedules == 0 && reschedules == prior_reschedules && !pending);
    event();
    if (battery_hid_dev) {
        hid_ready = true; usb_status = USB_DC_CONFIGURED;
        assert(pending && pending_delay == 0);
        run_work();
        assert(total_sends() == INTERFACES && schedules == 1 && last_schedule == 30000);
    } else { assert(!pending && reschedules == 0); }
}
static void test_send(int i, int error, bool inject_event) {
    init_ok();
    endpoints[i].send_result = error;
    if (inject_event) { during_write = event; inject_interface = i; }
    run_work();
    assert(total_sends() == INTERFACES && schedules == 1);
    assert(reschedules == (inject_event ? 2 : 1));
    assert(last_schedule == (error ? 1000 : 30000));
    assert(pending && pending_delay == (inject_event ? 0 : last_schedule));
    const uint8_t *attempts[2] = {endpoints[0].last_attempt, endpoints[1].last_attempt};
    if (error) {
        endpoints[i].send_result = 0;
        during_write = NULL;
        run_work();
        assert(total_sends() == 2 * INTERFACES && last_schedule == 30000 && pending_delay == 30000);
        for (int j = 0; j < INTERFACES; j++) {
            /* Failure holds only its endpoint's inactive slot; success advances. */
            assert((endpoints[j].last_attempt == attempts[j]) == (j == i));
        }
    }
}
static void test_lifetime(int failed) {
    init_ok();
    run_work();
    const uint8_t *first[2] = {endpoints[0].outstanding, endpoints[1].outstanding};
    endpoints[failed].send_result = -EAGAIN;
    const uint8_t *inactive = NULL;
    for (int n = 0; n < 4; n++) {
        percentage = 65 + n; peer_state.level = 35 + n;
        powered = n % 2; peer_state.charging = !powered;
        run_work();
        assert(endpoints[failed].outstanding == first[failed]);
        assert(endpoints[failed].last_attempt != first[failed]);
        if (inactive) { assert(endpoints[failed].last_attempt == inactive); }
        inactive = endpoints[failed].last_attempt;
        read_all();
    }
    /* Both endpoints can fail independently without damaging either retained RAM. */
    for (int i = 0; i < INTERFACES; i++) { endpoints[i].send_result = -EIO; }
    percentage = 9; peer_state.level = 91;
    run_work(); read_all();
    assert(endpoints[failed].outstanding == first[failed]);
    hid_ready = false; usb_status = USB_DC_DISCONNECTED;
    int sends_before = total_sends();
    run_work(); read_all();
    assert(total_sends() == sends_before && !pending);
    /* The real stack aborts pending transfers on disconnect, but it must not be
     * necessary to reset the module's buffer-selection state to recover. */
    for (int i = 0; i < INTERFACES; i++) {
        endpoints[i].outstanding = NULL;
        endpoints[i].send_result = 0;
    }
    hid_ready = true; usb_status = USB_DC_CONFIGURED;
    percentage = 0; peer_state.level = 100;
    event(); run_work(); read_all();
    assert(endpoints[failed].outstanding == inactive);
    /* A second failure/acceptance cycle must reuse, then advance, the right slot. */
    endpoints[failed].send_result = -EIO;
    percentage = 100; peer_state.level = 0;
    run_work(); read_all();
    assert(endpoints[failed].last_attempt == first[failed]);
    assert(endpoints[failed].outstanding == inactive);
    endpoints[failed].send_result = 0;
    run_work(); read_all();
    assert(endpoints[failed].outstanding == first[failed]);
}
static void test_transitions(void) {
    assert(INTERFACES == 2);
    init_ok();
    peer_available = false; peer_state.level = 255; peer_state.charging = true;
    run_work(); read_all();
    assert(endpoints[0].sends == 1 && endpoints[1].sends == 0 && last_schedule == 30000);
    peer_available = true; peer_state.level = 0; peer_state.charging = true;
    percentage = 100; powered = false;
    event(); run_work(); read_all();
    assert(endpoints[1].sends == 1 && endpoints[1].outstanding[1] == 1);
    assert(endpoints[0].outstanding[1] == 101 && endpoints[0].outstanding[2] == 0);
    /* Model invalidation/staleness through the peer API's false return. */
    peer_available = false; peer_state.level = 255; peer_state.charging = true;
    endpoints[1].send_result = -EIO;
    for (int n = 0; n < 3; n++) { event(); run_work(); read_all(); }
    assert(endpoints[1].sends == 1 && last_schedule == 30000);
    assert(endpoints[1].outstanding[1] == 1 && endpoints[1].outstanding[2] == 1);
    endpoints[1].send_result = 0;
    peer_available = true; peer_state.level = 100; peer_state.charging = false;
    percentage = 0; powered = true;
    event(); run_work(); read_all();
    assert(endpoints[1].sends == 2 && endpoints[1].outstanding[1] == 101);
    assert(endpoints[0].outstanding[1] == 1 && endpoints[0].outstanding[2] == 1);
}
static void test_suspend_pending(void) {
    init_ok(); run_work();
    int sends_before = total_sends();
    usb_status = USB_DC_SUSPEND;
    percentage = 0; peer_state.level = 100;
    powered = false; peer_state.charging = true;
    event(); run_work(); read_all();
    assert(total_sends() == sends_before && !pending && schedules == 1);
    /* Resume must recover with the previous transfers still protected until
     * their own endpoints accept replacements. No wakeup request is allowed. */
    usb_status = USB_DC_CONFIGURED;
    event(); run_work(); read_all();
    assert(total_sends() == 2 * INTERFACES && pending_delay == 30000);
}
static void test_event(void) {
    event(); assert(reschedules == 0 && !pending);
    init_ok(); run_work(); assert(pending_delay == 30000);
    event(); assert(pending_delay == 0 && reschedules == 2 && schedules == 1);
    event(); assert(pending_delay == 0 && reschedules == 3);
    assert(total_sends() == INTERFACES); /* Listener queues work; never writes. */
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *test = argv[1];
    int i = 0;
    char kind[16];
    if (strcmp(test, "init") == 0) { init_ok(); }
    else if (sscanf(test, "init-%d-%15s", &i, kind) == 2) { test_init_failure(i, kind); }
    else if (strcmp(test, "get-report") == 0) { test_get_report(); }
    else if (strcmp(test, "reset-config") == 0) {
        init_ok();
        peer_available = false;
        run_work();
        read_all();
        assert(endpoints[0].sends == 1 && endpoints[1].sends == 0 && peer_gets == 0);
    }
    else if (strcmp(test, "missing") == 0 || strcmp(test, "disconnect") == 0 ||
             strcmp(test, "suspend") == 0 || strcmp(test, "not-ready") == 0) { test_gated(test); }
    else if (strcmp(test, "send") == 0) { test_send(0, 0, false); }
    else if (strcmp(test, "retry-central") == 0) { test_send(0, -EAGAIN, false); }
    else if (strcmp(test, "retry-peer") == 0) { test_send(1, -EAGAIN, false); }
    else if (strcmp(test, "error-central") == 0) { test_send(0, -EIO, false); }
    else if (strcmp(test, "error-peer") == 0) { test_send(1, -EIO, false); }
    else if (strcmp(test, "event-send-central") == 0) { test_send(0, 0, true); }
    else if (strcmp(test, "event-send-peer") == 0) { test_send(1, 0, true); }
    else if (strcmp(test, "event-retry-central") == 0) { test_send(0, -EAGAIN, true); }
    else if (strcmp(test, "event-retry-peer") == 0) { test_send(1, -EAGAIN, true); }
    else if (strcmp(test, "lifetime-central") == 0) { test_lifetime(0); }
    else if (strcmp(test, "lifetime-peer") == 0) { test_lifetime(1); }
    else if (strcmp(test, "transitions") == 0) { test_transitions(); }
    else if (strcmp(test, "suspend-pending") == 0) { test_suspend_pending(); }
    else if (strcmp(test, "event") == 0) { test_event(); }
    else if (strcmp(test, "descriptor") == 0) {
        init_ok();
        for (int j = 0; j < INTERFACES; j++) {
            for (size_t n = 0; n < endpoints[j].desc_size; n++) { printf("%02x", endpoints[j].desc[n]); }
            putchar('\n');
        }
    } else { abort(); }
    if (INTERFACES == 1) { assert(peer_gets == 0); }
    return 0;
}
