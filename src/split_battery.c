/*
 * Split battery telemetry over a dedicated virtual Zephyr input source.
 * The stock BAS battery byte and sensor/key/pointer paths remain untouched.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_split_battery_source

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#define TELEMETRY_SOURCE DT_DRV_INST(0)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Split battery telemetry needs exactly one virtual source");

/* The source exists on both roles; only the peripheral emits input events.
 * Keep its device definition even when telemetry is disabled: settings_reset
 * turns BLE off but can retain the DT input proxy and its DEVICE_DT_GET source
 * reference. This inert source needs neither INPUT nor BLE initialization.
 */
DEVICE_DT_DEFINE(TELEMETRY_SOURCE, NULL, NULL, NULL, NULL, POST_KERNEL,
                 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY)

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include <zmk/events/split_battery_state_changed.h>
#include <zmk/split_battery.h>
#include <zmk/workqueue.h>

#define TELEMETRY_PROXY DT_NODELABEL(split_battery_proxy)
#define TELEMETRY_REG 254
#define POLL_MS 1000
#define HEARTBEAT_MS 10000
#define PEER_TTL_MS 35000

/* One event carries nonce[31:16] in code, nonce[15:0] in value[31:16].
 * The low word is protocol/version 0xA1, charging, and a seven-bit level.
 * Code is private session data, not an MSC code for normal input listeners.
 */
#define PACKET_TAG 0xA100U
#define PACKET_TAG_MASK 0xFF00U
#define PACKET_CHARGING BIT(7)
#define PACKET_LEVEL_MASK 0x7FU

BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_INPUT_SPLIT), "Split battery telemetry needs input-split");
BUILD_ASSERT(DT_NODE_HAS_COMPAT(TELEMETRY_PROXY, zmk_input_split),
             "Split battery telemetry needs its dedicated input proxy");
BUILD_ASSERT(DT_REG_ADDR(TELEMETRY_PROXY) == TELEMETRY_REG,
             "Split battery input reg 254 is reserved for telemetry");
BUILD_ASSERT(DT_SAME_NODE(DT_PHANDLE(TELEMETRY_PROXY, device), TELEMETRY_SOURCE),
             "Split battery proxy must use the virtual telemetry source");

ZMK_EVENT_IMPL(zmk_split_battery_state_changed);

static void telemetry_tick(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_tick);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zephyr/bluetooth/conn.h>
#include <zephyr/random/random.h>
#include <zmk/split/central.h>

BUILD_ASSERT(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS == 1,
             "Split battery telemetry currently supports exactly one peripheral");

static struct k_spinlock peer_lock;
static struct zmk_split_battery_state peer_state;
static struct bt_conn *peer_conn;
static int64_t peer_sample_ms;
static bool peer_available;
static uint32_t peer_session;

/* The hidden behavior exists on both roles; only the peripheral sets a session. */
void zmk_split_battery_set_session(uint32_t nonce) {}

bool zmk_split_battery_get_peer(struct zmk_split_battery_state *state) {
    if (state == NULL) {
        return false;
    }

    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    bool available =
        peer_conn != NULL && peer_available && k_uptime_get() - peer_sample_ms < PEER_TTL_MS;
    if (available) {
        *state = peer_state;
    }
    k_spin_unlock(&peer_lock, key);
    return available;
}

static void notify_peer_state(struct k_work *work) {
    struct zmk_split_battery_state state;
    raise_zmk_split_battery_state_changed((struct zmk_split_battery_state_changed){
        .available = zmk_split_battery_get_peer(&state),
    });
}

/* Input and Bluetooth callbacks must not run USB subscribers on their stacks. */
K_WORK_DEFINE(peer_changed_work, notify_peer_state);

static void queue_peer_changed(void) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &peer_changed_work);
}

static void telemetry_input(struct input_event *evt, void *user_data) {
    const uint32_t packet = (uint32_t)evt->value;
    const uint8_t level = packet & PACKET_LEVEL_MASK;

    const uint32_t nonce = ((uint32_t)evt->code << 16) | (packet >> 16);

    /* The proxy maps reg 254 to this device. Bare zero is never a valid sample. */
    if (evt->dev != DEVICE_DT_GET(TELEMETRY_PROXY) || evt->type != INPUT_EV_MSC || !evt->sync ||
        (packet & PACKET_TAG_MASK) != PACKET_TAG || level > 100 || nonce == 0) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    bool current_session = peer_conn != NULL && nonce == peer_session;
    if (current_session) {
        peer_state.level = level;
        peer_state.charging = (packet & PACKET_CHARGING) != 0;
        peer_sample_ms = k_uptime_get();
        peer_available = true;
    }
    k_spin_unlock(&peer_lock, key);

    if (current_session) {
        /* Heartbeats also wake subscribers, allowing reports to recover after drops. */
        queue_peer_changed();
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TELEMETRY_PROXY), telemetry_input, NULL);

static bool is_split_role(struct bt_conn *conn) {
    struct bt_conn_info info;
    return bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE &&
           info.role == BT_CONN_ROLE_CENTRAL;
}

static void telemetry_connected(struct bt_conn *conn, uint8_t err) {
    if (err || !is_split_role(conn)) {
        return;
    }

    /* Entropy generation can block; never run it under the spinlock. */
    uint32_t nonce = sys_rand32_get();
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    /* Ensure a nonzero, different token even if the random draw repeats or is zero. */
    if (nonce == 0 || nonce == peer_session) {
        nonce = peer_session + 1U;
        if (nonce == 0) {
            nonce = 1;
        }
    }
    /* Do not retain a sample across reconnects, even to the same peer. */
    peer_session = nonce;
    peer_conn = conn;
    peer_available = false;
    k_spin_unlock(&peer_lock, key);
    queue_peer_changed();
}

static void telemetry_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (!is_split_role(conn)) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    bool matched = peer_conn == conn;
    if (matched) {
        peer_conn = NULL;
        peer_available = false;
    }
    k_spin_unlock(&peer_lock, key);
    if (matched) {
        queue_peer_changed();
    }
}

BT_CONN_CB_DEFINE(split_battery_conn_callbacks) = {
    .connected = telemetry_connected,
    .disconnected = telemetry_disconnected,
};

static void telemetry_tick(struct k_work *work) {
    k_spinlock_key_t key = k_spin_lock(&peer_lock);
    bool expired = peer_available && k_uptime_get() - peer_sample_ms >= PEER_TTL_MS;
    if (expired) {
        peer_available = false;
    }
    bool needs_session = peer_conn != NULL && !peer_available;
    uint32_t nonce = peer_session;
    k_spin_unlock(&peer_lock, key);
    if (expired) {
        queue_peer_changed();
    }
    if (needs_session) {
        struct zmk_behavior_binding binding = {
            .behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(split_battery_session)),
            .param1 = nonce,
            .param2 = ZMK_SPLIT_BATTERY_SESSION_MAGIC,
        };
        struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};
        /* Discovery/command queues may not be ready on connect. Retry each second
         * until a matching echo arrives, not merely until queue acceptance.
         */
        zmk_split_central_invoke_behavior(0, &binding, event, true);
    }

    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &telemetry_work, K_MSEC(POLL_MS));
}

#else

#include <hal/nrf_power.h>
#include <zmk/battery.h>

static struct k_spinlock session_lock;
static uint32_t session_nonce;
/* Only telemetry_work owns last-sent state, including across session changes. */
static uint32_t last_sent_nonce;
static uint32_t last_sent_packet;
static int64_t last_sent_ms;
static bool sent_once;

bool zmk_split_battery_get_peer(struct zmk_split_battery_state *state) { return false; }

void zmk_split_battery_set_session(uint32_t nonce) {
    if (nonce == 0) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&session_lock);
    session_nonce = nonce;
    k_spin_unlock(&session_lock, key);
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &telemetry_work, K_NO_WAIT);
}

static void telemetry_tick(struct k_work *work) {
    k_spinlock_key_t key = k_spin_lock(&session_lock);
    const uint32_t nonce = session_nonce;
    k_spin_unlock(&session_lock, key);
    if (nonce == 0) {
        /* No telemetry before the central supplies the first session challenge. */
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &telemetry_work, K_MSEC(POLL_MS));
        return;
    }
    const uint8_t level = zmk_battery_state_of_charge();
    /*
     * Raw VBUS bypasses the split peripheral's disabled ZMK_USB stack.
     * Wiring on these boards is unverified: this is wired-power inference,
     * not proof of charging, charge current, or Qi detection.
     */
    const bool charging = nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
    const uint32_t packet = (nonce << 16) | PACKET_TAG | level | (charging ? PACKET_CHARGING : 0U);
    const int64_t now = k_uptime_get();

    if (level <= 100 && (!sent_once || nonce != last_sent_nonce || packet != last_sent_packet ||
                         now - last_sent_ms >= HEARTBEAT_MS)) {
        int ret = input_report(DEVICE_DT_GET(TELEMETRY_SOURCE), INPUT_EV_MSC,
                               (uint16_t)(nonce >> 16), (int32_t)packet, true, K_NO_WAIT);
        if (ret == 0) {
            /* Queue acceptance is not BLE delivery; heartbeats retry unsubscribed links. */
            last_sent_nonce = nonce;
            last_sent_packet = packet;
            last_sent_ms = now;
            sent_once = true;
        }
    }

    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &telemetry_work, K_MSEC(POLL_MS));
}

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

static int split_battery_init(void) {
    /* Queue only: work runs after SYS_INIT and the low-priority queue are ready. */
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &telemetry_work, K_NO_WAIT);
    return 0;
}

SYS_INIT(split_battery_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY */
