/* Exercise the actual translation unit, including its static callbacks/work. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs.h"
#include "src/split_battery.c"
#undef DT_DRV_COMPAT
#include "src/behaviors/behavior_split_battery.c"

const struct device test_devices[3] = {{0}, {1}, {2}};
static struct k_work_q lowprio_queue;
static int64_t now_ms, tick_due_ms;
static bool tick_pending;
static int schedules, lock_depth, lock_calls, callback_depth;

int64_t k_uptime_get(void) { return now_ms; }
k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    assert(lock != NULL && lock_depth == 0);
    lock_depth++;
    lock_calls++;
    return 0x1234;
}
void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    assert(lock != NULL && lock_depth == 1 && key == 0x1234);
    lock_depth--;
}
struct k_work_q *zmk_workqueue_lowprio_work_q(void) { return &lowprio_queue; }
int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                              k_timeout_t delay) {
    assert(queue == &lowprio_queue && work == &telemetry_work);
    assert(lock_depth == 0 && delay >= 0);
    schedules++;
    if (!tick_pending) {
        tick_pending = true;
        tick_due_ms = now_ms + delay;
        return 1;
    }
    return 0;
}
int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *work,
                                k_timeout_t delay) {
    assert(queue == &lowprio_queue && work == &telemetry_work);
    assert(lock_depth == 0 && delay == K_NO_WAIT);
    schedules++;
    tick_pending = true;
    tick_due_ms = now_ms + delay;
    return 1;
}
static void init(void) {
    assert(registered_init_priority == CONFIG_APPLICATION_INIT_PRIORITY);
    assert(registered_init() == 0);
    assert(schedules == 1 && tick_pending && tick_due_ms == 0);
}
static void tick(int64_t at_ms) {
    assert(tick_pending && at_ms >= tick_due_ms && at_ms >= now_ms);
    now_ms = at_ms;
    tick_pending = false;
    int before = schedules;
    telemetry_work.work.handler(&telemetry_work.work);
    assert(schedules == before + 1 && tick_pending && tick_due_ms == at_ms + 1000);
    assert(lock_depth == 0);
}

#if CONFIG_ZMK_SPLIT_ROLE_CENTRAL

static struct bt_conn split = {.type = BT_CONN_TYPE_LE, .role = BT_CONN_ROLE_CENTRAL};
static struct bt_conn other_split = {.type = BT_CONN_TYPE_LE, .role = BT_CONN_ROLE_CENTRAL};
static struct bt_conn host = {.type = BT_CONN_TYPE_LE, .role = BT_CONN_ROLE_PERIPHERAL};
static struct bt_conn non_le = {.type = 99, .role = BT_CONN_ROLE_CENTRAL};
static struct bt_conn bad_info = {
    .type = BT_CONN_TYPE_LE,
    .role = BT_CONN_ROLE_CENTRAL,
    .info_result = -EIO,
};
static bool notification_pending, running_notification, last_available;
static int submissions, raises, random_calls, handshake_calls, handshake_result;
static uint32_t random_value = 0x89ABCDEFU;
static struct zmk_behavior_binding last_handshake;

uint32_t sys_rand32_get(void) {
    assert(lock_depth == 0);
    random_calls++;
    return random_value;
}
int zmk_split_central_invoke_behavior(uint8_t source, struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event, bool state) {
    assert(lock_depth == 0 && callback_depth == 0);
    assert(source == 0 && state && event.timestamp == now_ms);
    assert(!strcmp(binding->behavior_dev, "sbat"));
    assert(binding->param1 != 0 && binding->param2 == ZMK_SPLIT_BATTERY_SESSION_MAGIC);
    handshake_calls++;
    last_handshake = *binding;
    return handshake_result;
}

int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info) {
    assert(conn != NULL && lock_depth == 0);
    *info = (struct bt_conn_info){.type = conn->type, .role = conn->role};
    return conn->info_result;
}
int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work) {
    assert(queue == &lowprio_queue && work == &peer_changed_work && lock_depth == 0);
    submissions++;
    bool already_pending = notification_pending;
    notification_pending = true;
    return already_pending ? 0 : 1;
}
int raise_zmk_split_battery_state_changed(struct zmk_split_battery_state_changed event) {
    /* Any inline notification from input/connection/timer callbacks fails here. */
    assert(running_notification && callback_depth == 0 && lock_depth == 0);
    struct zmk_split_battery_state state;
    assert(event.available == zmk_split_battery_get_peer(&state));
    raises++;
    last_available = event.available;
    return 0;
}
static void flush_notification(bool expected_available) {
    assert(notification_pending && callback_depth == 0);
    notification_pending = false;
    running_notification = true;
    int before = raises;
    peer_changed_work.handler(&peer_changed_work);
    running_notification = false;
    assert(raises == before + 1 && last_available == expected_available);
    assert(lock_depth == 0);
}
static void connected(struct bt_conn *conn, uint8_t err) {
    int before = raises;
    callback_depth++;
    split_battery_conn_callbacks.connected(conn, err);
    callback_depth--;
    assert(raises == before && lock_depth == 0);
}
static void disconnected(struct bt_conn *conn) {
    int before = raises;
    callback_depth++;
    split_battery_conn_callbacks.disconnected(conn, 0x13);
    callback_depth--;
    assert(raises == before && lock_depth == 0);
}
static struct input_event session_sample(uint32_t nonce, uint8_t level, bool charging) {
    return (struct input_event){
        .dev = &test_devices[split_battery_proxy],
        .type = INPUT_EV_MSC,
        .code = (uint16_t)(nonce >> 16),
        .sync = true,
        .value = (int32_t)((nonce << 16) | 0xA100U | level | (charging ? 0x80U : 0)),
    };
}
static struct input_event sample(uint8_t level, bool charging) {
    return session_sample(peer_session, level, charging);
}
static void input(struct input_event evt) {
    assert(registered_input_callback.dev == &test_devices[split_battery_proxy]);
    int before = raises;
    callback_depth++;
    registered_input_callback.callback(&evt, registered_input_callback.user_data);
    callback_depth--;
    assert(raises == before && lock_depth == 0);
}
static void unavailable(void) {
    struct zmk_split_battery_state state = {.level = 211, .charging = true};
    assert(!zmk_split_battery_get_peer(&state));
    assert(state.level == 211 && state.charging); /* No stale/zero sample returned. */
}
static void available(uint8_t level, bool charging) {
    struct zmk_split_battery_state state = {.level = 211, .charging = !charging};
    int before = lock_calls;
    assert(zmk_split_battery_get_peer(&state));
    assert(lock_calls == before + 1 && lock_depth == 0);
    assert(state.level == level && state.charging == charging);
}
static void start_connected(void) {
    init();
    connected(&split, 0);
    unavailable();
    assert(submissions == 1 && raises == 0);
    flush_notification(false);
}
static void reject(struct input_event evt) {
    int before = submissions;
    input(evt);
    unavailable();
    assert(submissions == before && !notification_pending);
}
static void case_init(void) {
    init();
    assert(!zmk_split_battery_get_peer(NULL));
    unavailable();
    assert(submissions == 0 && raises == 0);
    tick(0);
    unavailable();
    assert(submissions == 0 && raises == 0);
}
static void case_routing(void) {
    start_connected();
    struct input_event evt = sample(56, true);
    evt.dev = NULL;
    reject(evt);
    evt.dev = &test_devices[telemetry_source];
    reject(evt);
    evt.dev = &test_devices[unrelated_device];
    reject(evt);
    evt = sample(56, true);
    evt.type = INPUT_EV_MSC + 1;
    reject(evt);
    evt = sample(56, true);
    evt.code ^= 1;
    reject(evt);
    evt = sample(56, true);
    evt.sync = false;
    reject(evt);
    input(sample(56, true));
    available(56, true);
    flush_notification(true);
}
static void case_payload(void) {
    start_connected();
    const uint32_t invalid[] = {0,           1,           100,         0xffffffffU,
                                0x80000000U, 0x53420000U, 0x53422000U, 0x53421100U ^ 0x100000U};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        struct input_event evt = sample(0, false);
        evt.value = (int32_t)invalid[i];
        reject(evt);
    }
    /* Every tag/version bit is significant; sign bit is part of the nonce. */
    for (unsigned bit = 8; bit < 32; bit++) {
        struct input_event evt = sample(0, false);
        evt.value = (int32_t)((uint32_t)evt.value ^ (1U << bit));
        reject(evt);
    }
    for (unsigned bit = 0; bit < 16; bit++) {
        struct input_event evt = sample(0, false);
        evt.code ^= (uint16_t)(1U << bit);
        reject(evt);
    }
    reject(session_sample(0, 77, true));
    for (unsigned level = 101; level <= 127; level++) {
        reject(sample(level, false));
        reject(sample(level, true));
    }
    input(sample(0, false));
    available(0, false);
    flush_notification(true);
}
static void case_levels(void) {
    start_connected();
    /* Every valid percentage in both charge states, including real 0 and 100. */
    for (unsigned level = 0; level <= 100; level++) {
        for (unsigned charging = 0; charging <= 1; charging++) {
            input(sample(level, charging));
            available(level, charging);
            flush_notification(true);
        }
    }
}
static void case_ttl(void) {
    start_connected();
    input(sample(84, true));
    flush_notification(true);
    now_ms = 34999;
    available(84, true);
    assert(!notification_pending);
    now_ms = 35000;
    unavailable();
    assert(!notification_pending); /* Getter enforces TTL before periodic cleanup. */
    int before = submissions;
    tick(35000);
    assert(submissions == before + 1);
    flush_notification(false);
    tick(36000);
    assert(submissions == before + 1 && !notification_pending);
    input(sample(0, false));
    available(0, false);
    flush_notification(true);
}
static void case_bad_does_not_refresh(void) {
    start_connected();
    input(sample(63, false));
    flush_notification(true);
    now_ms = 34000;
    struct input_event evt = sample(64, true);
    evt.value ^= 0x100; /* Wrong protocol/version byte. */
    int before = submissions;
    input(evt);
    available(63, false);
    assert(submissions == before && !notification_pending);
    now_ms = 35000;
    unavailable();
}
static void case_heartbeat_refresh(void) {
    start_connected();
    input(sample(22, false));
    flush_notification(true);
    now_ms = 10000;
    input(sample(22, false)); /* Same-value heartbeats still notify and refresh TTL. */
    available(22, false);
    flush_notification(true);
    now_ms = 44999;
    available(22, false);
    now_ms = 45000;
    unavailable();
    tick(45000);
    flush_notification(false);
}
static void case_disconnect(void) {
    start_connected();
    input(sample(91, true));
    flush_notification(true);
    disconnected(&split);
    unavailable();
    flush_notification(false);
    reject(sample(92, false));
    disconnected(&split); /* Duplicate disconnect does not notify again. */
    assert(!notification_pending);
}
static void case_role_filter(void) {
    init();
    connected(&host, 0);
    connected(&non_le, 0);
    connected(&bad_info, 0);
    connected(&split, 1);
    disconnected(&host);
    disconnected(&non_le);
    disconnected(&bad_info);
    unavailable();
    assert(submissions == 0 && raises == 0);
    reject(sample(44, false));
    connected(&split, 0);
    flush_notification(false);
    input(sample(44, true));
    flush_notification(true);
    connected(&host, 0);
    disconnected(&host);
    disconnected(&other_split);
    disconnected(&non_le);
    disconnected(&bad_info);
    available(44, true);
    assert(!notification_pending);
}
static void case_reconnect(void) {
    start_connected();
    struct input_event retained = sample(77, true);
    uint32_t old_nonce = peer_session;
    input(retained);
    flush_notification(true);
    disconnected(&split);
    flush_notification(false);
    reject(sample(5, false));
    connected(&split, 0);
    unavailable();
    flush_notification(false);
    assert(peer_session != old_nonce);
    reject(retained); /* Old packet delivered AFTER reconnect with the same conn pointer. */
    tick(0);
    assert(last_handshake.param1 == peer_session);
    reject(retained); /* Handshake submission must not make an old echo acceptable. */
    input(sample(0, false));
    available(0, false);
    flush_notification(true);
    connected(&split, 0); /* Even the same connection identity clears validity. */
    unavailable();
    flush_notification(false);
    input(sample(10, true));
    flush_notification(true);
    retained = sample(77, true);
    connected(&other_split, 0);
    unavailable();
    flush_notification(false);
    reject(retained);     /* Different conn pointer is not needed to reject old data either. */
    disconnected(&split); /* Old peer must not clear the replacement connection. */
    assert(!notification_pending);
    input(sample(100, false));
    available(100, false);
    flush_notification(true);
}
static void case_handshake(void) {
    start_connected();
    uint32_t nonce = peer_session;
    assert(handshake_calls == 0);
    handshake_result = -ENODEV;
    tick(0);
    assert(handshake_calls == 1 && last_handshake.param1 == nonce);
    unavailable();
    handshake_result = -EAGAIN;
    tick(1000);
    assert(handshake_calls == 2 && last_handshake.param1 == nonce);
    handshake_result = 0;
    tick(2000);
    tick(3000);
    assert(handshake_calls == 4); /* Queue acceptance alone does not stop retries. */
    unavailable();
    input(sample(77, true));
    flush_notification(true);
    tick(4000);
    assert(handshake_calls == 4);
    tick(38000); /* Expiry retries the same challenge, without restoring validity. */
    assert(handshake_calls == 5 && last_handshake.param1 == nonce);
    flush_notification(false);
    disconnected(&split);
    flush_notification(false);
    tick(39000);
    assert(handshake_calls == 5);
}
static void case_nonce_generation(void) {
    init();
    random_value = 0;
    connected(&split, 0);
    assert(peer_session != 0 && random_calls == 1);
    uint32_t prior = peer_session;
    random_value = prior;
    connected(&split, 0);
    assert(peer_session != prior && peer_session != 0 && random_calls == 2);
    random_value = UINT32_MAX;
    connected(&split, 0);
    assert(peer_session == UINT32_MAX);
    connected(&split, 0);
    assert(peer_session == 1 && random_calls == 4);
    unavailable();
}
static void case_old_handshake(void) {
    start_connected();
    tick(0);
    uint32_t old_nonce = last_handshake.param1;
    disconnected(&split);
    flush_notification(false);
    connected(&split, 0);
    flush_notification(false);
    reject(session_sample(old_nonce, 77, true)); /* Old command completes on new link. */
    tick(1000);
    uint32_t new_nonce = last_handshake.param1;
    assert(new_nonce != old_nonce);
    reject(session_sample(old_nonce, 77, true)); /* Late echo after new handshake. */
    input(session_sample(new_nonce, 45, false));
    available(45, false);
    flush_notification(true);
    int before = submissions;
    now_ms = 34000;
    input(session_sample(old_nonce, 77, true));
    available(45, false);
    assert(submissions == before && !notification_pending);
    now_ms = 36000;
    unavailable(); /* Old-token notification did not refresh the TTL. */
}
static void case_behavior(void) {
    start_connected();
    struct zmk_behavior_binding binding = {
        .param1 = peer_session + 1,
        .param2 = ZMK_SPLIT_BATTERY_SESSION_MAGIC,
    };
    uint32_t nonce = peer_session;
    int before = schedules;
    assert(registered_behavior->binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) ==
           ZMK_BEHAVIOR_OPAQUE);
    zmk_split_battery_set_session(0);
    assert(peer_session == nonce && schedules == before);
    assert(registered_behavior->binding_released(
               &binding, (struct zmk_behavior_binding_event){0}) == ZMK_BEHAVIOR_OPAQUE);
    binding.param2 ^= 1;
    assert(registered_behavior->binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) ==
           -EINVAL);
    binding.param2 = ZMK_SPLIT_BATTERY_SESSION_MAGIC;
    binding.param1 = 0;
    assert(registered_behavior->binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) ==
           -EINVAL);
    assert(peer_session == nonce && schedules == before);
    unavailable();
}
static void case_deferred(void) {
    init();
    connected(&split, 0);
    input(sample(9, true));
    input(sample(10, false));
    assert(raises == 0 && submissions == 3);
    flush_notification(true); /* Coalescing reports current state, not first state. */
    available(10, false);
    input(sample(11, true));
    disconnected(&split);
    assert(raises == 1);
    flush_notification(false);
    connected(&split, 0);
    input(sample(12, true));
    now_ms = 35000;
    flush_notification(false); /* Work delayed beyond TTL cannot advertise valid data. */
}

#else

static NRF_POWER_Type power_registers;
NRF_POWER_Type *const test_nrf_power = &power_registers;
static uint8_t battery_level = 73;
static bool vbus;
static int battery_reads, vbus_reads, attempts, accepted, report_result;
static uint32_t packets[64];
static uint16_t codes[64];
static uint32_t expected_nonce = 0x89ABCDEFU;
static uint32_t session_during_report;

uint8_t zmk_battery_state_of_charge(void) {
    battery_reads++;
    return battery_level;
}
bool nrf_power_usbregstatus_vbusdet_get(const NRF_POWER_Type *power) {
    assert(power == test_nrf_power);
    vbus_reads++;
    return vbus;
}
int input_report(const struct device *dev, uint8_t type, uint16_t code, int32_t value, bool sync,
                 k_timeout_t timeout) {
    assert(dev == &test_devices[telemetry_source] && type == INPUT_EV_MSC);
    assert(sync && timeout == K_NO_WAIT);
    assert(lock_depth == 0 && attempts < (int)(sizeof(packets) / sizeof(packets[0])));
    codes[attempts] = code;
    packets[attempts++] = (uint32_t)value;
    if (session_during_report != 0) {
        uint32_t nonce = session_during_report;
        session_during_report = 0;
        zmk_split_battery_set_session(nonce);
    }
    if (!report_result) {
        accepted++;
    }
    return report_result;
}
/* Deliberately no connection APIs, central accessors, or event raise implementation. */
static void expect_packet(int attempt, uint8_t level, bool charging) {
    assert(codes[attempt] == (uint16_t)(expected_nonce >> 16));
    assert(packets[attempt] == ((expected_nonce << 16) | 0xA100U | level | (charging ? 0x80U : 0)));
    assert((((uint32_t)codes[attempt] << 16) | (packets[attempt] >> 16)) == expected_nonce);
}
static void start_session(void) {
    init();
    zmk_split_battery_set_session(expected_nonce);
    assert(tick_pending && tick_due_ms == now_ms);
}
static void case_init(void) {
    init();
    assert(attempts == 0 && battery_reads == 0 && vbus_reads == 0);
    struct zmk_split_battery_state state = {.level = 222, .charging = true};
    assert(!zmk_split_battery_get_peer(&state) && state.level == 222 && state.charging);
    assert(!zmk_split_battery_get_peer(NULL));
    tick(0);
    tick(1000);
    assert(attempts == 0 && accepted == 0 && battery_reads == 0 && vbus_reads == 0);
    int before = schedules;
    zmk_split_battery_set_session(0);
    assert(schedules == before);
    tick(2000);
    assert(attempts == 0);
    zmk_split_battery_set_session(expected_nonce);
    tick(2000); /* A real challenge reschedules immediately, without a settling delay. */
    assert(attempts == 1 && accepted == 1 && battery_reads == 1 && vbus_reads == 1);
    expect_packet(0, 73, false);
}
static void case_changes(void) {
    start_session();
    tick(0);
    tick(1000);
    assert(attempts == 1);
    battery_level = 74;
    tick(2000);
    assert(attempts == 2);
    expect_packet(1, 74, false);
    vbus = true;
    tick(3000);
    assert(attempts == 3);
    expect_packet(2, 74, true);
    vbus = false;
    tick(4000);
    assert(attempts == 4);
    expect_packet(3, 74, false);
    battery_level = 0;
    tick(5000);
    expect_packet(4, 0, false);
    battery_level = 100;
    vbus = true;
    tick(6000);
    expect_packet(5, 100, true);
    battery_level = 0;
    tick(7000);
    expect_packet(6, 0, true);
    battery_level = 100;
    vbus = false;
    tick(8000);
    expect_packet(7, 100, false);
    assert(attempts == 8 && accepted == 8 && battery_reads == 9 && vbus_reads == 9);
}
static void case_heartbeat(void) {
    start_session();
    tick(0);
    for (int i = 1; i < 10; i++) {
        tick(i * 1000);
    }
    assert(attempts == 1);
    tick(10000);
    assert(attempts == 2);
    expect_packet(1, 73, false);
    battery_level = 72;
    tick(11000);
    assert(attempts == 3);
    for (int i = 12; i <= 20; i++) {
        tick(i * 1000);
    }
    assert(attempts == 3); /* Successful change resets the heartbeat epoch. */
    tick(21000);
    assert(attempts == 4);
    expect_packet(3, 72, false);
}
static void case_retry_first(void) {
    start_session();
    report_result = -EAGAIN;
    tick(0);
    assert(attempts == 1 && accepted == 0);
    report_result = -EIO;
    tick(1000);
    assert(attempts == 2 && accepted == 0);
    report_result = 0;
    tick(2000);
    assert(attempts == 3 && accepted == 1);
    for (int i = 0; i < 3; i++) {
        expect_packet(i, 73, false);
    }
    tick(3000);
    assert(attempts == 3);
    for (int i = 4; i < 12; i++) {
        tick(i * 1000);
    }
    assert(attempts == 3);
    tick(12000);
    assert(attempts == 4); /* Failures did not set last-sent time. */
}
static void case_retry_change(void) {
    start_session();
    tick(0);
    battery_level = 74;
    vbus = true;
    report_result = -ENOSPC;
    tick(1000);
    assert(attempts == 2 && accepted == 1);
    report_result = 0;
    tick(2000);
    assert(attempts == 3 && accepted == 2);
    expect_packet(1, 74, true);
    expect_packet(2, 74, true);
    tick(3000);
    assert(attempts == 3);
    battery_level = 75;
    report_result = -EAGAIN;
    tick(4000);
    battery_level = 74;
    report_result = 0;
    tick(5000);
    assert(attempts == 4 && accepted == 2); /* Failed packet never became last-sent. */
}
static void case_retry_heartbeat(void) {
    start_session();
    tick(0);
    report_result = -EAGAIN;
    tick(10000);
    assert(attempts == 2 && accepted == 1);
    report_result = 0;
    tick(11000);
    assert(attempts == 3 && accepted == 2);
    tick(12000);
    assert(attempts == 3);
    tick(20000);
    assert(attempts == 3);
    tick(21000);
    assert(attempts == 4);
}
static void case_invalid_level(void) {
    start_session();
    const uint8_t invalid[] = {101, 127, 128, 255};
    for (size_t i = 0; i < sizeof(invalid); i++) {
        battery_level = invalid[i];
        vbus = i % 2;
        tick(i * 1000);
        assert(attempts == 0 && accepted == 0);
    }
    battery_level = 100;
    vbus = true;
    tick(4000);
    assert(attempts == 1);
    expect_packet(0, 100, true);
    battery_level = 255;
    vbus = false;
    tick(14000); /* Even overdue heartbeats cannot send an invalid level. */
    assert(attempts == 1);
    battery_level = 0;
    tick(15000);
    assert(attempts == 2 && accepted == 2);
    expect_packet(1, 0, false);
}
static void case_vbus(void) {
    start_session();
    battery_level = 42;
    vbus = false;
    tick(0);
    expect_packet(0, 42, false);
    vbus = true;
    tick(1000);
    expect_packet(1, 42, true);
    vbus = false;
    tick(2000);
    expect_packet(2, 42, false);
    assert(attempts == 3 && vbus_reads == 3);
}
static void case_session_change(void) {
    start_session();
    tick(0);
    uint32_t prior_packet = last_sent_packet;
    uint32_t prior_nonce = last_sent_nonce;
    /* Same low 16 bits: only the input code changes, not the packet value. */
    expected_nonce ^= 0x10000U;
    now_ms = 100;
    zmk_split_battery_set_session(expected_nonce);
    assert(last_sent_nonce == prior_nonce && last_sent_packet == prior_packet && last_sent_ms == 0);
    tick(100);
    assert(attempts == 2 && packets[0] == packets[1] && codes[0] != codes[1]);
    expect_packet(1, 73, false);
    /* Repeated handshake does not reset successful send state or heartbeat epoch. */
    now_ms = 200;
    zmk_split_battery_set_session(expected_nonce);
    tick(200);
    assert(attempts == 2);
    tick(10100);
    assert(attempts == 3);
    expect_packet(2, 73, false);
}
static void case_interleaved_session(void) {
    start_session();
    uint32_t next_nonce = expected_nonce ^ 0x00FF00FFU;
    session_during_report = next_nonce;
    /* A new handshake arrives while the worker submits an old-token frame. */
    tick_pending = false;
    telemetry_work.work.handler(&telemetry_work.work);
    assert(attempts == 1 && accepted == 1);
    expect_packet(0, 73, false); /* Both code and value came from one nonce snapshot. */
    assert(last_sent_nonce == expected_nonce && session_nonce == next_nonce);
    assert(tick_pending && tick_due_ms == 0); /* Setter's immediate reschedule survives. */
    expected_nonce = next_nonce;
    tick(0);
    assert(attempts == 2 && accepted == 2);
    expect_packet(1, 73, false);
}
static void case_nonce_bits(void) {
    init();
    /* Includes zero high/low words, all single bits, and signed int32 values. */
    for (unsigned bit = 0; bit < 32; bit++) {
        expected_nonce = 1U << bit;
        zmk_split_battery_set_session(expected_nonce);
        tick(bit * 1000);
        assert(attempts == (int)bit + 1);
        expect_packet(bit, 73, false);
    }
    expected_nonce = UINT32_MAX;
    vbus = true;
    zmk_split_battery_set_session(expected_nonce);
    tick(32000);
    assert(attempts == 33 && (int32_t)packets[32] < 0);
    expect_packet(32, 73, true);
}
static void case_behavior(void) {
    init();
    struct zmk_behavior_binding binding = {
        .param1 = expected_nonce,
        .param2 = ZMK_SPLIT_BATTERY_SESSION_MAGIC ^ 1,
    };
    int before = schedules;
    assert(registered_behavior->binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) ==
           -EINVAL);
    binding.param2 = ZMK_SPLIT_BATTERY_SESSION_MAGIC;
    binding.param1 = 0;
    assert(registered_behavior->binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) ==
           -EINVAL);
    assert(session_nonce == 0 && schedules == before);
    tick(0);
    assert(attempts == 0);
    binding.param1 = expected_nonce;
    assert(registered_behavior->binding_released(
               &binding, (struct zmk_behavior_binding_event){0}) == ZMK_BEHAVIOR_OPAQUE);
    assert(session_nonce == 0);
    now_ms = 100;
    assert(registered_behavior->binding_pressed(&binding, (struct zmk_behavior_binding_event){0}) ==
           ZMK_BEHAVIOR_OPAQUE);
    assert(session_nonce == expected_nonce && tick_pending && tick_due_ms == 100);
    tick(100);
    assert(attempts == 1);
    expect_packet(0, 73, false);
}

#endif

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    if (!strcmp(name, "init")) {
        case_init();
    }
#if CONFIG_ZMK_SPLIT_ROLE_CENTRAL
    else if (!strcmp(name, "routing")) {
        case_routing();
    } else if (!strcmp(name, "payload")) {
        case_payload();
    } else if (!strcmp(name, "levels")) {
        case_levels();
    } else if (!strcmp(name, "ttl")) {
        case_ttl();
    } else if (!strcmp(name, "bad-does-not-refresh")) {
        case_bad_does_not_refresh();
    } else if (!strcmp(name, "heartbeat-refresh")) {
        case_heartbeat_refresh();
    } else if (!strcmp(name, "disconnect")) {
        case_disconnect();
    } else if (!strcmp(name, "role-filter")) {
        case_role_filter();
    } else if (!strcmp(name, "reconnect")) {
        case_reconnect();
    } else if (!strcmp(name, "handshake")) {
        case_handshake();
    } else if (!strcmp(name, "nonce-generation")) {
        case_nonce_generation();
    } else if (!strcmp(name, "old-handshake")) {
        case_old_handshake();
    } else if (!strcmp(name, "deferred")) {
        case_deferred();
    }
#else
    else if (!strcmp(name, "changes")) {
        case_changes();
    } else if (!strcmp(name, "heartbeat")) {
        case_heartbeat();
    } else if (!strcmp(name, "retry-first")) {
        case_retry_first();
    } else if (!strcmp(name, "retry-change")) {
        case_retry_change();
    } else if (!strcmp(name, "retry-heartbeat")) {
        case_retry_heartbeat();
    } else if (!strcmp(name, "invalid-level")) {
        case_invalid_level();
    } else if (!strcmp(name, "vbus")) {
        case_vbus();
    } else if (!strcmp(name, "session-change")) {
        case_session_change();
    } else if (!strcmp(name, "nonce-bits")) {
        case_nonce_bits();
    } else if (!strcmp(name, "interleaved-session")) {
        case_interleaved_session();
    }
#endif
    else if (!strcmp(name, "behavior")) {
        case_behavior();
    } else {
        fprintf(stderr, "Unknown case: %s\n", name);
        return 2;
    }
    assert(lock_depth == 0 && callback_depth == 0);
    return 0;
}
