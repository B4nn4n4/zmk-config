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
fresh process so production static state cannot leak between cases. The descriptor
parser consumes the bytes passed to the stub registration function, including HID
global-state inheritance, usages, collections, report IDs, and bit offsets.

Coverage includes HID_1 initialization and failures; every invalid 16-bit GET_REPORT
selector; capacity, charging, clamping and request lengths; disconnect/readiness/
suspend gates; periodic and deferred sends; retained endpoint-buffer lifetime across
failed sends, EP0 reads and reconnect; and immediate events queued during a send.
The harness retains accepted RAM pointers instead of merely copying report values.

Limits: stubs model serialized work execution, pending-work schedule/reschedule
semantics, and disconnect transfer abortion. They do not execute Zephyr's scheduler,
USB stack, driver DMA, real concurrent interrupts, or host HID/power-supply mapping.
No claim of firmware compilation, enumeration, charging indication, or hardware
validation follows from these tests. Zephyr API compatibility and Linux/Windows/macOS
behavior still need separate integration checks.
