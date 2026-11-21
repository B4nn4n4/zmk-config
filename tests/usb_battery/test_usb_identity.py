"""Config and Linux 6.19 naming regressions, not a USB/kernel integration test."""

import ast
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CENTRAL_CONFIGS = (
    "config/hatsu_left.conf",
    "config/corneish_zen_left.conf",
    "config/tracktyl_left.conf",
)
HID_DEVICES = ("0003:1D50:615E.000A", "0003:1D50:615E.000B")


def config_values(path):
    """Read explicit assignments only; this does not evaluate Kconfig defaults."""
    values = {}
    for key, value in re.findall(r"^(CONFIG_\w+)=(.*)$", path.read_text(), re.MULTILINE):
        if key in values:
            raise AssertionError(f"Duplicate assignment for {key} in {path}")
        values[key] = value.strip()
    return values


def linux_619_battery_name(hid_uniq, hid_device_name):
    """Model hidinput_setup_battery(): uniq if nonempty, otherwise dev_name().

    Linux v6.19 drivers/hid/hid-input.c uses `hid-%s-battery`. USB interfaces
    share the device serial in HID_UNIQ; neither interface nor report ID is
    appended when that serial is present. This models only the naming rule.
    """
    return f"hid-{hid_uniq or hid_device_name}-battery"


class UsbBatteryIdentityTests(unittest.TestCase):
    def test_every_two_battery_central_config_uses_an_empty_usb_serial(self):
        for relative in CENTRAL_CONFIGS:
            with self.subTest(config=relative):
                values = config_values(ROOT / relative)
                self.assertEqual(values["CONFIG_ZMK_USB_BATTERY_REPORTING"], "y")
                self.assertEqual(values["CONFIG_USB_HID_DEVICE_COUNT"], "3")
                self.assertEqual(ast.literal_eval(values["CONFIG_USB_DEVICE_SN"]), "")
                # Removing only the USB string must not turn off existing BLE reporting.
                self.assertEqual(values["CONFIG_ZMK_CHARGE_STATE_REPORTING"], "y")

    def test_shared_serial_reproduces_collision_despite_distinct_hid_devices(self):
        names = [linux_619_battery_name("example-shared-serial", dev) for dev in HID_DEVICES]
        self.assertEqual(names, ["hid-example-shared-serial-battery"] * 2)

    def test_empty_configured_serial_uses_distinct_names_for_both_halves(self):
        for relative in CENTRAL_CONFIGS:
            with self.subTest(config=relative):
                serial = ast.literal_eval(config_values(ROOT / relative)["CONFIG_USB_DEVICE_SN"])
                names = [linux_619_battery_name(serial, dev) for dev in HID_DEVICES]
                self.assertEqual(names, [f"hid-{dev}-battery" for dev in HID_DEVICES])
                self.assertEqual(len(set(names)), 2)

    def test_serial_free_battery_names_are_not_stable_across_reenumeration(self):
        # This is the intentional history/path trade-off, not a stable device ID.
        before = linux_619_battery_name("", "0003:1D50:615E.000A")
        after = linux_619_battery_name("", "0003:1D50:615E.000C")
        self.assertNotEqual(before, after)


if __name__ == "__main__":
    unittest.main()
