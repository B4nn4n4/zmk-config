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
    "zmk/usb.h",
    "zmk/workqueue.h",
)


def parse_descriptor(data):
    """Decode HID short items, global inheritance, and per-ID bit offsets."""
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
            elif tag == 8:  # Input
                if not collections:
                    raise AssertionError("Input outside collection")
                report_id = state.get("id", 0)
                offset = offsets.get(report_id, 0)
                fields.append(
                    dict(state, usages=tuple(usages), flags=value, offset=offset)
                )
                offsets[report_id] = offset + state["size"] * state["count"]
            else:
                raise AssertionError(f"Unexpected main tag {tag}; expected input only")
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
        cls.executable = cls.build / "usb_battery_test"
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
            "-o", str(cls.executable),
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"C harness build failed:\n{result.stdout}{result.stderr}")

    def run_case(self, name):
        # Each process starts with fresh production static state and buffers.
        result = subprocess.run(
            [str(self.executable), name],
            cwd=self.build,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, f"{name}:\n{result.stdout}{result.stderr}")
        return result.stdout

    def test_init_registers_hid_1_before_initializing_and_queues_work(self):
        self.run_case("init")

    def test_init_missing_device(self):
        self.run_case("init-missing")

    def test_init_unready_device(self):
        self.run_case("init-unready")

    def test_init_propagates_hid_failure_without_starting_worker(self):
        self.run_case("init-error")

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

    def test_ready_worker_sends_and_schedules_30_second_refresh(self):
        self.run_case("send")

    def test_eagain_retries_after_one_second_without_advancing_buffer(self):
        self.run_case("retry")

    def test_other_send_errors_also_retry(self):
        self.run_case("send-error")

    def test_inflight_buffer_survives_failed_sends_ep0_and_reconnect(self):
        self.run_case("lifetime")

    def test_event_reschedules_pending_periodic_work_immediately(self):
        self.run_case("event")

    def test_periodic_schedule_preserves_event_queued_during_send(self):
        self.run_case("event-during-send")

    def test_retry_schedule_preserves_event_queued_during_failed_send(self):
        self.run_case("event-during-retry")

    def test_emitted_descriptor_layout(self):
        # Read bytes registered by actual C code, not a copied descriptor fixture.
        descriptor = bytes.fromhex(self.run_case("descriptor").strip())
        applications, report_ids, fields, offsets = parse_descriptor(descriptor)
        self.assertEqual(applications, [(1, ((0x0C, 0x01),))])
        self.assertEqual(report_ids, {1})
        self.assertEqual(offsets, {1: 24})  # 3-byte payload plus 1-byte report ID.
        self.assertEqual(1 + offsets[1] // 8, 4)
        self.assertEqual(len(fields), 5)
        self.assertEqual([field["id"] for field in fields], [1] * 5)
        self.assertEqual([field["offset"] for field in fields], [0, 8, 9, 16, 17])
        self.assertEqual([field["offset"] + 8 for field in fields], [8, 16, 17, 24, 25])
        self.assertEqual([field["size"] for field in fields], [8, 1, 7, 1, 7])
        self.assertEqual([field["count"] for field in fields], [1] * 5)
        self.assertEqual([field["flags"] for field in fields], [2, 2, 3, 2, 3])
        self.assertEqual(
            [field["usages"] for field in fields],
            [((0x06, 0x20),), ((0x85, 0x44),), (), ((0x0C, 0xE2),), ()],
        )
        self.assertEqual((fields[0]["minimum"], fields[0]["maximum"]), (0, 100))
        self.assertEqual((fields[1]["minimum"], fields[1]["maximum"]), (0, 1))
        self.assertEqual((fields[3]["minimum"], fields[3]["maximum"]), (0, 1))
        # C GET_REPORT and send cases separately assert Mute and padding stay zero.


if __name__ == "__main__":
    unittest.main()
