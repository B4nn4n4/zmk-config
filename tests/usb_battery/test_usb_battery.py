"""Host-only regression tests of the actual USB battery translation unit."""

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
    "zephyr/init.h",
    "zephyr/kernel.h",
    "zephyr/logging/log.h",
    "zephyr/sys/util.h",
    "zephyr/usb/class/usb_hid.h",
    "zephyr/usb/usb_device.h",
    "zmk/battery.h",
    "zmk/event_manager.h",
    "zmk/events/battery_state_changed.h",
    "zmk/events/usb_conn_state_changed.h",
    "zmk/events/split_battery_state_changed.h",
    "zmk/split_battery.h",
    "zmk/usb.h",
    "zmk/workqueue.h",
)


def parse_descriptor(data):
    """Decode short items and independent (report type, ID) payload offsets."""
    state = {}
    usages = []
    collections = []
    applications = []
    fields = []
    offsets = {}
    report_ids = set()
    pos = 0
    while pos < len(data):
        prefix = data[pos]
        pos += 1
        if prefix == 0xFE:
            raise AssertionError("Unexpected HID long item")
        size = (0, 1, 2, 4)[prefix & 3]
        item_type = (prefix >> 2) & 3
        tag = prefix >> 4
        raw = data[pos : pos + size]
        if len(raw) != size:
            raise AssertionError("Truncated HID descriptor")
        pos += size
        value = int.from_bytes(raw, "little")
        if item_type == 1:  # Global
            if tag == 0:
                state["page"] = value
            elif tag == 1:
                state["minimum"] = int.from_bytes(raw, "little", signed=True)
            elif tag == 2:
                state["maximum"] = int.from_bytes(
                    raw, "little", signed=state.get("minimum", 0) < 0
                )
            elif tag == 7:
                state["size"] = value
            elif tag == 8:
                if not 1 <= value <= 255:
                    raise AssertionError("Invalid report ID")
                state["id"] = value
                report_ids.add(value)
            elif tag == 9:
                state["count"] = value
            else:
                raise AssertionError(f"Unexpected global tag {tag}")
        elif item_type == 2 and tag == 0:  # Local Usage
            usages.append((state["page"], value))
        elif item_type == 0:  # Main items consume local state
            if tag == 10:  # Collection
                collection = (value, tuple(usages))
                collections.append(collection)
                if value == 1:
                    applications.append(collection)
            elif tag == 12:  # End Collection
                if not collections:
                    raise AssertionError("Unbalanced End Collection")
                collections.pop()
            elif tag in (8, 11):  # Input and Feature occupy distinct report spaces.
                if not collections:
                    raise AssertionError("Report field outside collection")
                report_type = "input" if tag == 8 else "feature"
                report_id = state.get("id", 0)
                key = (report_type, report_id)
                offset = offsets.get(key, 0)
                fields.append(
                    dict(state, type=report_type, usages=tuple(usages), flags=value, offset=offset)
                )
                offsets[key] = offset + state["size"] * state["count"]
            else:
                raise AssertionError(f"Unexpected main tag {tag}; expected Input or Feature")
            usages = []
        else:
            raise AssertionError(f"Unexpected HID item {prefix:#x}")
    if collections or usages:
        raise AssertionError("Unclosed collection or unconsumed usages")
    return applications, report_ids, fields, offsets


class UsbBatteryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="usb-battery-test-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.build = Path(cls.temp.name)
        for header in HEADERS:
            path = cls.build / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "stubs.h"\n')
        cls.executables = {}
        command = shlex.split(os.environ.get("CC", "gcc")) + [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-I", str(cls.build),
            "-I", str(HERE),
            "-I", str(ROOT),
            str(HERE / "harness.c"),
        ]
        for variant, macros in {
            "split": ["-DCONFIG_USB_HID_DEVICE_COUNT=3", "-DCONFIG_ZMK_SPLIT_BATTERY_TELEMETRY=1"],
            # Kconfig n means undefined, NOT a macro defined to zero: production uses defined().
            "single": ["-DCONFIG_USB_HID_DEVICE_COUNT=2"],
            # Reset keymaps omit the telemetry DT nodes, while central config keeps HID count 3.
            "reset": ["-DCONFIG_USB_HID_DEVICE_COUNT=3"],
        }.items():
            executable = cls.build / f"usb_battery_{variant}"
            cls.executables[variant] = executable
            result = subprocess.run(command + macros + ["-o", str(executable)],
                                    capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise AssertionError(f"{variant} C harness build failed:\n{result.stdout}{result.stderr}")

    def run_case(self, name, variant="split"):
        # Each process starts with fresh production static state and buffers.
        result = subprocess.run(
            [str(self.executables[variant]), name],
            cwd=self.build,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, f"{variant}/{name}:\n{result.stdout}{result.stderr}")
        return result.stdout

    def test_count_three_without_telemetry_withholds_peer_and_keeps_central(self):
        self.run_case("reset-config", variant="reset")

    def test_init_registers_both_interfaces_and_queues_work(self):
        self.run_case("init")

    def test_init_missing_device_on_either_interface(self):
        for i in range(2):
            with self.subTest(interface=i + 1):
                self.run_case(f"init-{i}-missing")

    def test_init_unready_device_on_either_interface(self):
        for i in range(2):
            with self.subTest(interface=i + 1):
                self.run_case(f"init-{i}-unready")

    def test_init_propagates_each_hid_failure_without_starting_worker(self):
        for i in range(2):
            with self.subTest(interface=i + 1):
                self.run_case(f"init-{i}-error")

    def test_get_report_selector_capacity_charging_clamping_and_lengths(self):
        self.run_case("get-report")

    def test_missing_device_neither_sends_nor_schedules(self):
        self.run_case("missing")

    def test_disconnected_worker_stops_until_resume_event(self):
        self.run_case("disconnect")

    def test_unready_hid_neither_sends_nor_schedules(self):
        self.run_case("not-ready")

    def test_suspend_never_sends_or_wakes_host_and_resume_restarts(self):
        self.run_case("suspend")

    def test_suspend_protects_both_pending_buffers_and_resume_recovers(self):
        self.run_case("suspend-pending")

    def test_ready_worker_sends_and_schedules_30_second_refresh(self):
        self.run_case("send")

    def test_eagain_retries_each_endpoint_independently(self):
        for interface in ("central", "peer"):
            with self.subTest(interface=interface):
                self.run_case(f"retry-{interface}")

    def test_other_send_errors_on_either_endpoint_also_retry(self):
        for interface in ("central", "peer"):
            with self.subTest(interface=interface):
                self.run_case(f"error-{interface}")

    def test_each_inflight_buffer_survives_other_writes_failures_ep0_and_reconnect(self):
        for interface in ("central", "peer"):
            with self.subTest(interface=interface):
                self.run_case(f"lifetime-{interface}")

    def test_peer_absent_valid_empty_stale_invalid_and_full_transitions(self):
        self.run_case("transitions")

    def test_event_reschedules_pending_periodic_work_immediately(self):
        self.run_case("event")

    def test_periodic_schedule_preserves_event_queued_during_either_send(self):
        for interface in ("central", "peer"):
            with self.subTest(interface=interface):
                self.run_case(f"event-send-{interface}")

    def test_retry_schedule_preserves_event_queued_during_either_failed_send(self):
        for interface in ("central", "peer"):
            with self.subTest(interface=interface):
                self.run_case(f"event-retry-{interface}")

    def check_descriptor(self, descriptor):
        applications, report_ids, fields, offsets = parse_descriptor(descriptor)
        self.assertEqual(applications, [(1, ((0x0C, 0x01),))])
        self.assertEqual(report_ids, {1, 2})
        self.assertEqual(offsets, {("input", 1): 24, ("feature", 2): 24})
        self.assertTrue(all(1 + bits // 8 == 4 for bits in offsets.values()))
        self.assertEqual(len(fields), 6)
        inputs = [field for field in fields if field["type"] == "input"]
        features = [field for field in fields if field["type"] == "feature"]
        self.assertEqual([field["id"] for field in inputs], [1] * 5)
        self.assertEqual([field["offset"] for field in inputs], [0, 8, 9, 16, 17])
        self.assertEqual([field["offset"] + 8 for field in inputs], [8, 16, 17, 24, 25])
        self.assertEqual([field["size"] for field in inputs], [8, 1, 7, 1, 7])
        self.assertEqual([field["count"] for field in inputs], [1] * 5)
        self.assertEqual([field["flags"] for field in inputs], [2, 2, 3, 2, 3])
        self.assertEqual(
            [field["usages"] for field in inputs],
            [((0x06, 0x20),), ((0x85, 0x44),), (), ((0x0C, 0xE2),), ()],
        )
        self.assertEqual((inputs[0]["minimum"], inputs[0]["maximum"]), (1, 101))
        # Match generic Linux scaling; raw zero is never needed, even at true 0%.
        for percent in range(101):
            wire_value = percent + 1
            self.assertNotEqual(wire_value, 0)
            self.assertEqual((wire_value - 1) * 100 // (101 - 1), percent)
        self.assertEqual((inputs[1]["minimum"], inputs[1]["maximum"]), (0, 1))
        self.assertEqual((inputs[3]["minimum"], inputs[3]["maximum"]), (0, 1))
        self.assertEqual(len(features), 1)
        feature = features[0]
        self.assertEqual((feature["id"], feature["offset"], feature["size"], feature["count"]),
                         (2, 0, 8, 3))
        self.assertEqual((feature["minimum"], feature["maximum"], feature["flags"]), (0, 255, 2))
        self.assertEqual(feature["page"], 0xFF00)
        self.assertEqual(feature["usages"], ((0xFF00, 1),))
        # Feature metadata must not create another generic battery capacity field.
        capacities = [field for field in fields if (0x06, 0x20) in field["usages"]]
        self.assertEqual(len(capacities), 1)
        self.assertEqual((capacities[0]["type"], capacities[0]["id"]), ("input", 1))
        # C GET_REPORT and send cases separately assert Mute and padding stay zero.

    def test_emitted_descriptors_match_and_preserve_input_and_feature_layouts(self):
        # Read both descriptors registered by actual C code, not copied fixtures.
        descriptors = self.run_case("descriptor").splitlines()
        self.assertEqual(len(descriptors), 2)
        self.assertEqual(descriptors[0], descriptors[1])
        for interface, descriptor in enumerate(descriptors, 1):
            with self.subTest(interface=interface):
                self.check_descriptor(bytes.fromhex(descriptor))

    def test_count_two_without_split_telemetry_remains_compatible(self):
        cases = ("init", "init-0-missing", "init-0-unready", "init-0-error", "get-report",
                 "missing", "disconnect", "not-ready", "suspend", "suspend-pending", "send", "retry-central",
                 "error-central", "lifetime-central", "event", "event-send-central",
                 "event-retry-central")
        for case in cases:
            with self.subTest(case=case):
                self.run_case(case, "single")
        descriptors = self.run_case("descriptor", "single").splitlines()
        self.assertEqual(len(descriptors), 1)
        self.check_descriptor(bytes.fromhex(descriptors[0]))

    def test_descriptor_parser_keeps_type_and_id_offsets_independent(self):
        # Synthetic descriptor: Input and Feature deliberately reuse report ID 1.
        descriptor = bytes.fromhex(
            "050c0901a1018501750895018102b1028102b102c0"
        )
        _, _, fields, offsets = parse_descriptor(descriptor)
        self.assertEqual(offsets, {("input", 1): 16, ("feature", 1): 16})
        self.assertEqual([field["offset"] for field in fields], [0, 0, 8, 8])


if __name__ == "__main__":
    unittest.main()
