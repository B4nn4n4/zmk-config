/* Minimal host API seam. This is not a Zephyr integration environment. */
#ifndef SPLIT_BATTERY_TEST_STUBS_H
#define SPLIT_BATTERY_TEST_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CONFIG_ZMK_SPLIT_ROLE_CENTRAL
#error "Select a split role for each host build"
#endif
#define CONFIG_ZMK_INPUT_SPLIT 1
#define CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS 1
#define CONFIG_KERNEL_INIT_PRIORITY_DEFAULT 40
#define CONFIG_INPUT_INIT_PRIORITY 50
#define CONFIG_APPLICATION_INIT_PRIORITY 90
#define IS_ENABLED(value) (value)
#define BIT(n) (1U << (n))
#define BUILD_ASSERT(condition, ...) _Static_assert(condition, #condition)
#define APPLICATION 1
#define POST_KERNEL 2
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)
#define SYS_INIT(fn, level, priority)                                                              \
    static int (*const registered_init)(void) __attribute__((used)) = fn;                          \
    static const int registered_init_priority __attribute__((used)) = priority

/* This fake topology satisfies the source's assertions, not real devicetree. */
enum { telemetry_source, split_battery_proxy, unrelated_device };
struct device {
    int id;
};
extern const struct device test_devices[3];
#define DT_DRV_INST(n) telemetry_source
#define DT_NODELABEL(name) name
#define DT_NUM_INST_STATUS_OKAY(compat) 1
#define DT_NODE_HAS_COMPAT(node, compat) 1
#define DT_HAS_COMPAT_STATUS_OKAY(compat) 1
#define DEVICE_DT_NAME(node) "sbat"
#define DT_REG_ADDR(node) 254
#define DT_PHANDLE(node, prop) telemetry_source
#define DT_SAME_NODE(a, b) ((a) == (b))
#define DEVICE_DT_DEFINE(...)
#define DEVICE_DT_GET(node) (&test_devices[node])

typedef int k_timeout_t;
struct k_work {
    void (*handler)(struct k_work *);
};
struct k_work_delayable {
    struct k_work work;
};
struct k_work_q {
    int unused;
};
struct k_spinlock {
    int unused;
};
typedef unsigned int k_spinlock_key_t;
#define K_WORK_DELAYABLE_DEFINE(name, fn)                                                          \
    static struct k_work_delayable name = {.work = {.handler = fn}}
#define K_WORK_DEFINE(name, fn) static struct k_work name = {.handler = fn}
int64_t k_uptime_get(void);
k_spinlock_key_t k_spin_lock(struct k_spinlock *lock);
void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key);
struct k_work_q *zmk_workqueue_lowprio_work_q(void);
int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                              k_timeout_t delay);
int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                                k_timeout_t delay);
int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work);

struct zmk_behavior_binding {
    const char *behavior_dev;
    uint32_t param1, param2;
};
struct zmk_behavior_binding_event {
    int64_t timestamp;
    uint32_t position;
    uint8_t source;
};
#define ZMK_BEHAVIOR_OPAQUE 0
struct behavior_driver_api {
    int (*binding_pressed)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
    int (*binding_released)(struct zmk_behavior_binding *, struct zmk_behavior_binding_event);
};
#define BEHAVIOR_DT_INST_DEFINE(inst, init, pm, data, config, level, priority, api)                \
    static const struct behavior_driver_api *const registered_behavior = api

#define INPUT_EV_MSC 4
#define INPUT_MSC_RAW 3
struct input_event {
    const struct device *dev;
    bool sync;
    uint8_t type;
    uint16_t code;
    int32_t value;
};
#define INPUT_CALLBACK_DEFINE(dev_arg, fn, data)                                                   \
    static const struct {                                                                          \
        const struct device *dev;                                                                  \
        void (*callback)(struct input_event *, void *);                                            \
        void *user_data;                                                                           \
    } registered_input_callback __attribute__((used)) = {dev_arg, fn, data}
/* Deliberately absent on central: accidental sender references must not link. */
#if !CONFIG_ZMK_SPLIT_ROLE_CENTRAL
int input_report(const struct device *dev, uint8_t type, uint16_t code, int32_t value, bool sync,
                 k_timeout_t timeout);
#endif

/* Use the real production state/event headers through ROOT/include. */
#define ZMK_EVENT_DECLARE(name) int raise_##name(struct name event)
#define ZMK_EVENT_IMPL(name)

/* No USB API is part of this module's seam, on either role. */
#pragma GCC poison zmk_usb_is_powered zmk_usb_get_status zmk_usb_is_hid_ready

#endif
