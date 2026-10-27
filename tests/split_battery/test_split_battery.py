"""Host-only tests compiling the actual split battery source for each role."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HEADERS = (
    "zephyr/device.h",
    "zephyr/devicetree.h",
    "zephyr/init.h",
    "zephyr/input/input.h",
    "zephyr/kernel.h",
    "zephyr/sys/util.h",
    "zmk/event_manager.h",
    "zmk/workqueue.h",
    "zmk/behavior.h",
    "drivers/behavior.h",
)
ROLE_HEADERS = {
    "zephyr/random/random.h": """
#pragma once
#include "stubs.h"
#if !CONFIG_ZMK_SPLIT_ROLE_CENTRAL
#error "Peripheral must not generate central session challenges"
#endif
uint32_t sys_rand32_get(void);
""",
    "zmk/split/central.h": """
#pragma once
#include "stubs.h"
#if !CONFIG_ZMK_SPLIT_ROLE_CENTRAL
#error "Peripheral must not include central behavior transport"
#endif
int zmk_split_central_invoke_behavior(uint8_t source, struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event, bool state);
""",
    "zephyr/bluetooth/conn.h": """
#pragma once
#include "stubs.h"
#if !CONFIG_ZMK_SPLIT_ROLE_CENTRAL
#error "Peripheral must not include the central connection API"
#endif
#define BT_CONN_TYPE_LE 1
#define BT_CONN_ROLE_CENTRAL 0
#define BT_CONN_ROLE_PERIPHERAL 1
struct bt_conn { uint8_t type; uint8_t role; int info_result; };
struct bt_conn_info { uint8_t type; uint8_t role; };
struct bt_conn_cb {
    void (*connected)(struct bt_conn *, uint8_t);
    void (*disconnected)(struct bt_conn *, uint8_t);
};
#define BT_CONN_CB_DEFINE(name) const struct bt_conn_cb name
int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info);
""",
    "hal/nrf_power.h": """
#pragma once
#include "stubs.h"
#if CONFIG_ZMK_SPLIT_ROLE_CENTRAL
#error "Central must not include the peripheral raw POWER API"
#endif
typedef struct { int unused; } NRF_POWER_Type;
extern NRF_POWER_Type *const test_nrf_power;
#define NRF_POWER test_nrf_power
bool nrf_power_usbregstatus_vbusdet_get(const NRF_POWER_Type *power);
""",
    "zmk/battery.h": """
#pragma once
#include "stubs.h"
#if CONFIG_ZMK_SPLIT_ROLE_CENTRAL
#error "Central must not include the peripheral battery API"
#endif
uint8_t zmk_battery_state_of_charge(void);
""",
}
VARIANTS = {
    "central": {"CONFIG_ZMK_SPLIT_ROLE_CENTRAL": 1},
    "peripheral-radio-usb-off": {
        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL": 0,
        "CONFIG_ZMK_USB": 0,
        "CONFIG_ZMK_BLE": 0,
    },
    "peripheral-radio-usb-on": {
        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL": 0,
        "CONFIG_ZMK_USB": 1,
        "CONFIG_ZMK_BLE": 1,
    },
}


class SplitBatteryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="split-battery-test-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.build = Path(cls.temp.name)
        for header in HEADERS:
            path = cls.build / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "stubs.h"\n')
        for header, content in ROLE_HEADERS.items():
            path = cls.build / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        cls.executables = {}
        cls.preprocessed = {}
        for variant, defines in VARIANTS.items():
            command = shlex.split(os.environ.get("CC", "gcc")) + [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-DCONFIG_ZMK_SPLIT_BATTERY_TELEMETRY=1",
                "-I", str(cls.build),
                "-I", str(HERE),
                "-I", str(ROOT),
                "-I", str(ROOT / "include"),
            ] + [f"-D{name}={value}" for name, value in defines.items()]
            executable = cls.build / variant
            result = subprocess.run(
                command + [str(HERE / "harness.c"), "-o", str(executable)],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if result.returncode:
                raise AssertionError(
                    f"{variant} C harness build failed:\n{result.stdout}{result.stderr}"
                )
            cls.executables[variant] = executable
            # Inspect the real translation unit after conditional compilation.
            result = subprocess.run(
                command + ["-E", "-P", str(ROOT / "src/split_battery.c")],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if result.returncode:
                raise AssertionError(f"{variant} preprocessing failed:\n{result.stderr}")
            cls.preprocessed[variant] = result.stdout

    def run_case(self, role, name):
        variants = [variant for variant in VARIANTS if variant.startswith(role)]
        self.assertTrue(variants, role)
        for variant in variants:
            with self.subTest(variant=variant, case=name):
                # Separate processes reset production static state for every case.
                result = subprocess.run(
                    [str(self.executables[variant]), name],
                    cwd=self.build,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(
                    result.returncode, 0, f"{variant}/{name}:\n{result.stdout}{result.stderr}"
                )

    def test_central_startup_unknown_null_safe_and_init_only_queues(self):
        self.run_case("central", "init")

    def test_central_rejects_wrong_device_type_nonce_code_and_unsynchronized_input(self):
        self.run_case("central", "routing")

    def test_central_rejects_zero_nonce_bad_magic_version_nonce_bits_and_range(self):
        self.run_case("central", "payload")

    def test_central_all_percentages_in_both_independent_charge_states(self):
        self.run_case("central", "levels")

    def test_central_35_second_ttl_boundary_and_one_deferred_expiry_notification(self):
        self.run_case("central", "ttl")

    def test_central_invalid_input_cannot_replace_or_refresh_sample(self):
        self.run_case("central", "bad-does-not-refresh")

    def test_central_unchanged_heartbeat_refreshes_ttl_and_notifies(self):
        self.run_case("central", "heartbeat-refresh")

    def test_central_disconnect_clears_cache_and_rejects_subsequent_input(self):
        self.run_case("central", "disconnect")

    def test_central_host_ble_role_errors_non_le_and_other_disconnect_are_ignored(self):
        self.run_case("central", "role-filter")

    def test_central_reconnect_rejects_retained_packets_even_with_connection_pointer_reuse(self):
        self.run_case("central", "reconnect")

    def test_central_handshake_retries_until_matching_echo_not_queue_acceptance(self):
        self.run_case("central", "handshake")

    def test_central_nonce_zero_repetition_and_wrap_never_reuse_prior_nonce(self):
        self.run_case("central", "nonce-generation")

    def test_central_late_old_handshake_echo_cannot_validate_new_connection(self):
        self.run_case("central", "old-handshake")

    def test_hidden_behavior_actual_source_validates_magic_nonce_and_role(self):
        self.run_case("central", "behavior")
        self.run_case("peripheral", "behavior")

    def test_central_notifications_are_deferred_coalesced_and_use_current_state(self):
        self.run_case("central", "deferred")

    def test_peripheral_init_queues_without_reading_or_transmitting(self):
        self.run_case("peripheral", "init")

    def test_peripheral_new_handshake_sends_immediately_and_compares_full_nonce(self):
        self.run_case("peripheral", "session-change")

    def test_peripheral_handshake_during_report_never_mixes_nonce_or_loses_reschedule(self):
        self.run_case("peripheral", "interleaved-session")

    def test_peripheral_packet_code_and_signed_value_preserve_all_nonce_bits(self):
        self.run_case("peripheral", "nonce-bits")

    def test_peripheral_reports_level_and_charge_changes_including_real_zero_and_100(self):
        self.run_case("peripheral", "changes")

    def test_peripheral_one_second_poll_and_ten_second_heartbeat_after_last_acceptance(self):
        self.run_case("peripheral", "heartbeat")

    def test_peripheral_queue_errors_retry_first_packet_without_advancing_state(self):
        self.run_case("peripheral", "retry-first")

    def test_peripheral_failed_change_retries_and_never_becomes_last_sent(self):
        self.run_case("peripheral", "retry-change")

    def test_peripheral_failed_heartbeat_retries_on_next_poll(self):
        self.run_case("peripheral", "retry-heartbeat")

    def test_peripheral_out_of_range_level_never_transmits_even_when_overdue(self):
        self.run_case("peripheral", "invalid-level")

    def test_peripheral_raw_vbus_is_independent_of_usb_and_ble_config_flags(self):
        self.run_case("peripheral", "vbus")

    def test_session_behavior_shared_node_binding_and_gated_source_registration(self):
        import re

        overlay = (ROOT / "config/telemetry/split_battery.dtsi").read_text()
        node = re.search(r"split_battery_session:\s+(\w+)\s*\{([^}]+)\}", overlay)
        self.assertIsNotNone(node)
        self.assertLessEqual(len(node.group(1)), 8)
        self.assertEqual(node.group(1), "sbat")
        self.assertIn('compatible = "zmk,behavior-split-battery-session";', node.group(2))
        self.assertIn("#binding-cells = <2>;", node.group(2))
        self.assertNotIn("zmk,input-listener", overlay)
        binding = (
            ROOT / "dts/bindings/behaviors/zmk,behavior-split-battery-session.yaml"
        ).read_text()
        self.assertIn('compatible: "zmk,behavior-split-battery-session"', binding)
        self.assertIn("include: two_param.yaml", binding)
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn(
            "target_sources_ifdef(CONFIG_DT_HAS_ZMK_SPLIT_BATTERY_SOURCE_ENABLED app PRIVATE "
            "src/split_battery.c)", cmake,
        )
        self.assertIn(
            "target_sources_ifdef(CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY app PRIVATE "
            "src/behaviors/behavior_split_battery.c)", cmake,
        )

    def test_role_isolated_link_and_preprocessor_api_symbols(self):
        # Builds already link without any opposite-role API definitions. Their
        # fake headers also reject importing opposite-role APIs. Check retained
        # declarations/calls explicitly, without requiring nm or a Zephyr tree.
        self.assertNotRegex(
            self.preprocessed["central"],
            r"\b(?:zmk_battery_state_of_charge|nrf_power_\w*|NRF_POWER\w*)\b",
        )
        for variant in VARIANTS:
            with self.subTest(variant=variant):
                self.assertNotRegex(
                    self.preprocessed[variant],
                    r"\b(?:CONFIG_ZMK_USB|zmk_usb_\w+)\b",
                )
                if variant.startswith("peripheral"):
                    self.assertNotRegex(
                        self.preprocessed[variant],
                        r"\b(?:bt_conn\w*|as_zmk_\w+|sys_rand32_get|zmk_split_central_\w+)\b",
                    )
        # The actual behavior source is included and linked in each role harness.
        # Its fake API registration exposes its real pressed/released callbacks.
        # Preprocessing alone would hide configuration-dependent USB use.
        # Remove comments before checking source tokens; its explanatory comment
        # about working without ZMK_USB is allowed, executable dependencies are not.
        import re

        source = (ROOT / "src/split_battery.c").read_text()
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)
        self.assertNotRegex(source, r"\b(?:CONFIG_ZMK_USB|ZMK_USB|zmk_usb_\w+)\b")


if __name__ == "__main__":
    unittest.main()
