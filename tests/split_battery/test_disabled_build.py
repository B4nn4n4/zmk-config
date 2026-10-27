"""Host link regression for a retained input-split source with telemetry off.

settings_reset disables BLE, but its kscan-only overlay can retain the keymap's
telemetry source/proxy nodes. Model the root target_sources_ifdef condition, then
compile the real source as a separate object and link a fake proxy against its
external device ordinal. DEVICE_DT_DEFINE must provide an actual definition.

This is not a CMake or Zephyr invocation: the four generated Zephyr headers model
only device/devicetree/init/util contracts. No runtime, input, Bluetooth, kernel,
or ZMK headers/libraries are supplied. Only standard-library Python and GCC are
required. All generated files live in temporary directories.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/split_battery.c"
DT_GATE = "CONFIG_DT_HAS_ZMK_SPLIT_BATTERY_SOURCE_ENABLED"
RUNTIME_GATE = "CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY"
DEVICE_SYMBOL = "__device_dts_ord_196"

HEADERS = {
    "zephyr/devicetree.h": r"""
#pragma once
#define TEST_CAT_RAW(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT_RAW(a, b)
#define DT_DRV_INST(instance) split_battery_source
#define DT_NUM_INST_STATUS_OKAY(compat) 1
#define DT_DEP_ORD_split_battery_source 196
#define DT_DEP_ORD(node) TEST_CAT(DT_DEP_ORD_, node)
#define DEVICE_DT_NAME_GET(node) TEST_CAT(__device_dts_ord_, DT_DEP_ORD(node))
#define DEVICE_DT_GET(node) (&DEVICE_DT_NAME_GET(node))
""",
    "zephyr/device.h": r"""
#pragma once
#include <stddef.h>
#include <zephyr/devicetree.h>
struct device {
    int ordinal;
    int init_priority;
    const void *init;
    const void *pm;
    const void *data;
    const void *config;
    const void *api;
};
extern const struct device __device_dts_ord_196;
/* A real externally linked object, not the no-op used by runtime harnesses. */
#define DEVICE_DT_DEFINE(node, init_fn, pm_fn, data_ptr, config_ptr, level, priority, api_ptr, ...) \
    const struct device DEVICE_DT_NAME_GET(node) = { \
        .ordinal = DT_DEP_ORD(node), .init_priority = (priority), \
        .init = (init_fn), .pm = (pm_fn), .data = (data_ptr), \
        .config = (config_ptr), .api = (api_ptr), \
    }
""",
    "zephyr/init.h": r"""
#pragma once
#define POST_KERNEL 1
/* Deliberately no SYS_INIT or CONFIG_INPUT_INIT_PRIORITY. */
""",
    "zephyr/sys/util.h": r"""
#pragma once
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)
/* Preserve the Kconfig distinction: both an undefined token and 0 are false. */
#define TEST_ENABLED_1 0,
#define TEST_SECOND(ignored, value, ...) value
#define TEST_ENABLED_EXPAND(arg) TEST_SECOND(arg 1, 0)
#define TEST_ENABLED(config) TEST_ENABLED_EXPAND(TEST_ENABLED_##config)
#define IS_ENABLED(config) TEST_ENABLED(config)
""",
}

PROXY = r"""
#include <zephyr/device.h>

/* Like input_split.c, this separate translation unit needs the source device. */
const struct device *const proxy_source = DEVICE_DT_GET(DT_DRV_INST(0));

int main(void) {
    return !(proxy_source->ordinal == 196 && proxy_source->init_priority == 40 &&
             proxy_source->init == NULL && proxy_source->pm == NULL &&
             proxy_source->data == NULL && proxy_source->config == NULL &&
             proxy_source->api == NULL);
}
"""


class DisabledBuildTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="split-battery-disabled-")
        self.addCleanup(temp.cleanup)
        self.build = Path(temp.name)
        self.include = self.build / "include"
        for name, content in HEADERS.items():
            path = self.include / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        self.proxy = self.build / "proxy.c"
        self.proxy.write_text(PROXY)
        self.cc = shlex.split(os.environ.get("CC", "gcc"))

    def source_gate(self):
        # Model this one production statement, not general CMake evaluation.
        cmake = re.sub(r"#[^\n]*", "", (ROOT / "CMakeLists.txt").read_text())
        gates = re.findall(
            r"target_sources_ifdef\s*\(\s*(CONFIG_[A-Z0-9_]+)\s+"
            r"app\s+PRIVATE\s+src/split_battery\.c\s*\)",
            cmake,
        )
        self.assertEqual(len(gates), 1, "Expected one split_battery.c source gate")
        return gates[0]

    def run_command(self, command):
        return subprocess.run(command, capture_output=True, text=True, timeout=60)

    def assert_command_ok(self, command):
        result = self.run_command(command)
        self.assertEqual(
            result.returncode, 0,
            f"Command failed: {shlex.join(command)}\n{result.stdout}{result.stderr}",
        )
        return result

    def build_variant(self, central, telemetry, *, omit_source=False):
        name = f"central-{central}-telemetry-{telemetry}-omit-{omit_source}"
        directory = self.build / name
        directory.mkdir()
        config = {
            DT_GATE: 1,
            RUNTIME_GATE: 0,
            "CONFIG_ZMK_BLE": 0,
            "CONFIG_ZMK_SPLIT_ROLE_CENTRAL": central,
            "CONFIG_KERNEL_INIT_PRIORITY_DEFAULT": 40,
        }
        # Kconfig n is normally undefined; also guard against #ifdef bugs when 0.
        defines = {key: value for key, value in config.items() if key != RUNTIME_GATE}
        if telemetry == "zero":
            defines[RUNTIME_GATE] = 0
        compile_command = self.cc + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
            "-I", str(self.include),
        ] + [f"-D{key}={value}" for key, value in defines.items()]
        proxy_object = directory / "proxy.o"
        self.assert_command_ok(
            compile_command + ["-c", str(self.proxy), "-o", str(proxy_object)]
        )
        objects = [proxy_object]
        # If CMake regresses to the runtime gate, the real source is omitted and
        # the proxy link below must fail, rather than silently compiling it anyway.
        if config.get(self.source_gate(), 0) and not omit_source:
            source_object = directory / "split_battery.o"
            self.assert_command_ok(
                compile_command + ["-c", str(SOURCE), "-o", str(source_object)]
            )
            objects.append(source_object)
            preprocessed = self.assert_command_ok(
                compile_command + ["-E", "-P", str(SOURCE)]
            ).stdout
            self.assertIn(f"const struct device {DEVICE_SYMBOL} =", preprocessed)
            self.assertNotRegex(
                preprocessed,
                r"\b(?:zmk_split_battery_\w+|telemetry_\w+|"
                r"split_battery_conn_callbacks|INPUT_CALLBACK_DEFINE|BT_CONN_CB_DEFINE|"
                r"SYS_INIT|ZMK_EVENT_IMPL|K_WORK\w*)\b",
                "Disabled source must define no telemetry APIs, work, or callbacks",
            )
        executable = directory / "proxy"
        linked = self.run_command(
            self.cc + [str(path) for path in objects] + ["-o", str(executable)]
        )
        return linked, executable

    def assert_disabled_variant(self, central, telemetry):
        linked, executable = self.build_variant(central, telemetry)
        self.assertEqual(linked.returncode, 0, linked.stdout + linked.stderr)
        self.assert_command_ok([str(executable)])

    def test_cmake_keeps_device_source_when_runtime_is_disabled(self):
        self.assertEqual(self.source_gate(), DT_GATE)

    def test_central_undefined_telemetry_links_and_runs(self):
        self.assert_disabled_variant(1, "undefined")

    def test_central_zero_telemetry_links_and_runs(self):
        self.assert_disabled_variant(1, "zero")

    def test_peripheral_undefined_telemetry_links_and_runs(self):
        self.assert_disabled_variant(0, "undefined")

    def test_peripheral_zero_telemetry_links_and_runs(self):
        self.assert_disabled_variant(0, "zero")

    def test_negative_control_omitted_source_has_undefined_device(self):
        for central in (0, 1):
            for telemetry in ("undefined", "zero"):
                with self.subTest(central=central, telemetry=telemetry):
                    linked, _ = self.build_variant(central, telemetry, omit_source=True)
                    self.assertNotEqual(linked.returncode, 0)
                    diagnostics = linked.stdout + linked.stderr
                    self.assertIn(DEVICE_SYMBOL, diagnostics)
                    self.assertRegex(diagnostics, r"undefined reference|undefined symbol")


if __name__ == "__main__":
    unittest.main()
