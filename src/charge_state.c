/*
 * Report USB charge state to the BLE host through the standard BAS 1.1
 * "Battery Level Status" characteristic (0x2BED), which carries a charge-state
 * field that plain BAS 1.0 (0x2A19, level only) does not.
 *
 * Zephyr implements the characteristic; nothing in ZMK ever populates it, so
 * this module drives it from ZMK's USB connection state.
 *
 * Central half only — see README. The peripheral has no channel to push VBUS
 * state back over the split link without forking ZMK, so there is deliberately
 * no per-half reporting here.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/services/bas.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static void charge_state_update(struct k_work *work) {
    const bool powered = zmk_usb_is_powered();

    LOG_DBG("charge state: wired power %s", powered ? "connected" : "disconnected");

    bt_bas_bls_set_wired_external_power_source(powered ? BT_BAS_BLS_WIRED_POWER_CONNECTED
                                                       : BT_BAS_BLS_WIRED_POWER_NOT_CONNECTED);

    /*
     * No board in this repo exposes a charger-status interface, so VBUS
     * presence is all we have: a full battery left plugged in still reads as
     * CHARGING. This matches what the on-keyboard indicator does.
     */
    bt_bas_bls_set_battery_charge_state(powered ? BT_BAS_BLS_CHARGE_STATE_CHARGING
                                                : BT_BAS_BLS_CHARGE_STATE_DISCHARGING_ACTIVE);
}

K_WORK_DEFINE(charge_state_work, charge_state_update);

static int charge_state_listener(const zmk_event_t *eh) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &charge_state_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(charge_state, charge_state_listener);
ZMK_SUBSCRIPTION(charge_state, zmk_usb_conn_state_changed);

static int charge_state_init(void) {
    bt_bas_bls_set_battery_present(BT_BAS_BLS_BATTERY_PRESENT);

    /*
     * Qi charging is not firmware-detectable on any board here (AGENTS.md), so
     * UNKNOWN rather than NOT_CONNECTED: a keyboard on a Qi pad is charging
     * and we genuinely cannot tell.
     */
    bt_bas_bls_set_wireless_external_power_source(BT_BAS_BLS_WIRELESS_POWER_UNKNOWN);

    return k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &charge_state_work);
}

/*
 * Must run AFTER Zephyr's bas_init(), which calls bt_bas_bls_init() and zeroes
 * the whole power-state bitfield -- at the same init level and priority it
 * would race and silently wipe everything set above.
 */
SYS_INIT(charge_state_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY + 1);
