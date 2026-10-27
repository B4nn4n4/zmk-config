# USB battery host unit tests

Run from the repository root:

```sh
nix shell nixpkgs#python3 nixpkgs#gcc --command python3 -B -m unittest discover -s tests/usb_battery -v
```

With Python 3 and GCC already on PATH, omit the `nix shell … --command` prefix.
If the agent host exports FHS library flags, its `/usr/lib` libc can conflict with
Nix's loader (`__pointer_chk_guard`, `GLIBC_PRIVATE`). This checkout's local run
needed a clean compiler/runtime environment:

```sh
nix shell nixpkgs#python3 nixpkgs#gcc --command \
  env -u NIX_LDFLAGS -u NIX_CFLAGS_LINK -u NIX_CFLAGS_COMPILE \
      -u LD_LIBRARY_PATH -u LD_PRELOAD \
  python3 -B -m unittest discover -s tests/usb_battery -v
```

Only Python's standard library and a C11 compiler are needed. `CC` may select
another compiler. Generated include shims and the executable live in a temporary
directory, removed after the suite. No firmware build, CI, or hardware access occurs.

`harness.c` includes the actual `src/usb_battery.c`. Minimal Zephyr/ZMK stubs capture
registration, GET_REPORT, endpoint buffers, and scheduling. Each scenario runs in a
fresh process so production static state cannot leak between cases. Two independent
fake devices capture registration, initialization results, endpoint-write results,
and retained RAM pointers. Every write and EP0 read checks that neither device's
accepted buffer changed. Acceptance completes only that endpoint's prior transfer;
failure does not advance its buffer. No real HID_0 interface is stubbed or touched.

The main binary uses `CONFIG_USB_HID_DEVICE_COUNT=3` and
`CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY=1`. A second binary uses count two with telemetry
undefined (Kconfig `n`, not a macro defined as zero). It runs the single-interface
initialization, error, report, gating, retry, buffer-lifetime, and scheduling cases
and checks the descriptor. The fake peer getter must never be called in that binary.
A third binary keeps count three but omits runtime telemetry, matching a central
`settings_reset` build with BLE disabled. Reset builds can retain the telemetry DT
nodes; the separate split tests cover that virtual-device linkage. It still
reports the central battery, withholds peer Input reports, returns unavailable
peer Feature data, and
never calls the absent peer API.

Coverage includes:

- HID_1/HID_2 registration before initialization, missing/unready devices, and
  initialization failures on either device without starting telemetry work.
- Device-routed GET_REPORT callbacks: Input 1 `[1, percent+1, charging, 0]` and vendor
  Feature 2 `[2, available, percent, charging]`. All other 16-bit selectors fail.
  Request lengths include zero, short, exact, oversized, and 65535 bytes.
- Distinct central/peer percentages, 0% versus 100%, opposite charging states,
  central clamping, and peer available/unavailable transitions. Input capacity's
  logical range 1–101 scales exactly to 0–100%, including empty batteries on Linux
  versions that ignore unsolicited raw zero. Vendor Feature uses plain percent.
  Unavailable Input
  returns `-ENODATA` without changing output arguments; Feature validity is zero
  with zero unused values. Unavailable peers never send interrupt reports or
  spuriously trigger error retries, even with an injected peer write failure.
- Independent `-EAGAIN`/`-EIO` failures, one-second retries, 30-second refreshes,
  separate buffer advancement, and immediate events queued during either send.
- Both accepted RAM buffers surviving the other endpoint's writes, failures,
  EP0 requests, suspend, and disconnect before transfer abortion. Reconnect and
  subsequent failures recover without resetting buffer-selection state.
- Disconnect/readiness/suspend gates, including suspend with both buffers pending;
  neither interface writes or requests host wakeup while suspended.

The descriptor parser consumes bytes registered by the production C code. It
checks identical descriptors on both interfaces, HID global-state inheritance,
usages, collections, and independent bit offsets/counts per `(report type, ID)`.
Input 1 and Feature 2 each have a three-byte payload plus report ID. Vendor Feature
metadata must not introduce a second generic Battery Strength usage. A synthetic
parser case reuses one ID across Input and Feature to prove offset isolation.

Limits: stubs model serialized work execution, pending-work schedule/reschedule
semantics, and disconnect transfer abortion. Peer staleness and invalidation are
modeled by a fake `zmk_split_battery_get_peer()` returning false with retained invalid
bytes; these tests do not validate the split transport, validation rules, or stale
timeout itself. Available peer samples follow the API contract (levels 0–100).
Generated header shims do not check real Zephyr/ZMK headers or API compatibility.
The tests do not execute Zephyr's scheduler, USB stack, driver DMA, real concurrent
interrupts, or host HID/power-supply mapping.
No claim of firmware compilation, enumeration, charging indication, or hardware
validation follows from these tests. Zephyr API compatibility and Linux/Windows/macOS
behavior still need separate integration checks.
