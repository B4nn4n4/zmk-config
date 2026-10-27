# Split battery telemetry host regression tests

These tests compile and run the actual `src/split_battery.c` and
`src/behaviors/behavior_split_battery.c` sources. They use the real
`include/zmk/split_battery.h` and split battery event header. Python generates
small fake Zephyr/ZMK/nRF API headers in a temporary directory; `harness.c`
supplies deterministic time, queue, connection, random, behavior transport, input,
POWER-register, and spinlock seams. No firmware file is changed or copied into a
test fixture.

Requirements: standard-library Python 3 and GCC. No Zephyr SDK, board, BLE adapter,
third-party Python package, or `nm` is required.

```sh
nix shell nixpkgs#python3 nixpkgs#gcc --command env -u LD_LIBRARY_PATH -u NIX_CFLAGS_COMPILE -u NIX_CFLAGS_LINK -u NIX_LDFLAGS python3 -B -m unittest discover -s tests/split_battery -v
```

When Python/GCC are already available:

```sh
python3 -B -m unittest discover -s tests/split_battery -v
```

Each scenario runs in a fresh process so production static state starts clean.
The suite builds the central role and two peripheral variants, with USB/BLE
configuration flags both disabled and both enabled. Peripheral scenarios run in
both variants. These role harnesses explicitly enable the telemetry runtime to
isolate its logic; the USB/BLE flag variants are API-isolation checks, not complete
Kconfig-valid reset configurations. Disabled-runtime builds are tested separately
below. Builds use `-Wall -Wextra -Werror` and no optimization, and provide no
implementations for opposite-role APIs. Role-specific fake headers reject
wrong-role imports. Preprocessed-source checks also guard against central
battery/nRF dependencies, peripheral connection/event-accessor/random/central
transport dependencies, and USB API dependencies. A comment-stripped source
check catches USB config tokens that preprocessing would otherwise erase.

## Coverage

- Startup is unknown, not a fabricated zero-percent sample. Initialization queues
  work without immediate reads, transmission, or event delivery.
- Central input requires the dedicated proxy device, `INPUT_EV_MSC`, sync, and
  a nonzero nonce matching the active connection's challenge. The full 32-bit
  nonce spans `code[15:0]` (high word) and `value[31:16]` (low word).
  `value[15:8]` is magic/version `0xA1`, bit 7 is charging, and bits 6–0 are level.
  Code is private session data, not `INPUT_MSC_RAW`. Every nonce and magic bit,
  zero nonce, bare zero, and invalid percentages are tested. Signed values retain
  their bits. Every valid percentage 0–100 is tested in both charge states.
- Cache access takes the spinlock. Freshness changes exactly at 35 seconds.
  Invalid input does not replace or refresh a sample. Same-value heartbeats do.
  Periodic expiry queues exactly one invalidation event until new input arrives.
- Split-role LE connection callbacks clear validity on connect/disconnect.
  Host BLE peripheral-role callbacks, failed connections, non-LE connections,
  failed connection-info lookups, and unmatched disconnects are ignored.
  Disconnected input cannot become valid. Retained old input delivered **after**
  reconnect is rejected, including when the connection pointer is reused.
  Late echoes from old handshakes cannot restore validity or refresh a new
  sample's TTL. A matching new-session echo succeeds.
- Each split connection generates a fresh nonzero nonce outside the spinlock.
  Repeated and zero random draws, including wrap from `UINT32_MAX`, cannot reuse
  the immediately preceding token. The low-priority tick retries the same
  challenge every second through command failures and queue acceptance, stopping
  only after a matching echo. Expiry resumes retries; disconnect stops them.
- The actual hidden behavior's pressed/released callbacks run on both roles.
  Bad protocol magic and zero nonce are rejected. Valid press schedules the
  peripheral worker immediately; release does nothing. The central setter is a
  no-op. Static checks cover the shared `sbat` node, two-parameter binding,
  no normal listener, and feature-gated registration of both source files.
- Notifications run only when the fake low-priority queue is drained, never
  inside input, connection, or timer callbacks and never under the spinlock.
  Coalesced/delayed notifications resolve current cache validity.
- Peripheral input packets use the virtual source, private nonce code, sync
  flag, and nonblocking queue submission. No frame or sensor read occurs before
  the first handshake. Every nonce bit, zero high/low words, and signed values
  round-trip through code/value. Changing only the nonce's high word still sends
  immediately. Repeated handshakes do not reset the heartbeat clock. A handshake
  arriving during submission cannot mix code/value tokens or lose its immediate
  reschedule; only the worker changes last-sent state.
- Raw nRF VBUS controls charging independently of USB/BLE config flags. One-second
  polls report changes and accepted packets reset the ten-second heartbeat clock.
- Queue errors on the first packet, a change, or a heartbeat retry on the next
  poll without marking unsuccessful packets as sent. Levels above 100 never
  transmit, even when a heartbeat is overdue.

## Disabled-runtime device linkage

`test_disabled_build.py` compiles the real `src/split_battery.c` separately with
BLE off and telemetry either undefined or explicitly zero, for both roles. Only
four minimal generic Zephyr headers are supplied. No INPUT, Bluetooth, kernel
runtime, ZMK runtime headers, or INPUT init-priority symbol is available.

Unlike the normal role harness, its `DEVICE_DT_DEFINE` mock emits an externally
linked device object. A separate proxy translation unit references the modeled
`__device_dts_ord_196` symbol and must link and run. Four negative controls omit
the source and require an undefined-device link error. The test also parses the
production CMake source gate and models it with the DT node enabled and runtime
disabled, so changing the source back to the runtime gate breaks the regression.

This models the device-reference contract, not actual CMake execution, generated
Zephyr device ordinals, or an end-to-end firmware link. Reset builds may retain
keymap source/proxy nodes even though their BLE telemetry runtime is disabled.

## Limits

This is host simulation, not a real Zephyr build or runtime test. Fake devicetree
macros satisfy assertions but do not verify bindings, overlays, source/proxy
registration, reg-254 routing, or generated device ordering. Workqueue execution
and coalescing are modeled, not Zephyr scheduler behavior. Spinlocks detect
missing/imbalanced seam use, not multicore races or memory-order correctness.

Connection callbacks are invoked directly in controlled order. These tests do
not verify real Bluetooth callback registration/order, `bt_conn` lifetime,
connection-info availability during disconnection, host/split identification in
the actual stack, actual-stack input delivery during connection transitions,
or transport queue ordering. Retained packets are captured and delivered explicitly after simulated
reconnects; old handshake echoes and an in-flight peripheral session change are
also simulated. There is no BLE transport, input-split queue,
subscription/delivery acknowledgement, or radio-loss test. Queue acceptance is
not proof of transport delivery. Nonces are 32-bit random challenges, not an
unbounded session counter or cryptographic authentication. Tests verify nonzero
and immediately preceding-token exclusion, not collision freedom across all
historical connections or actual entropy quality.

No USB descriptors, host battery enumeration, DMA/buffer lifetime, charging
circuit, real POWER registers, VBUS wiring, Qi detection, hardware charging, deep
sleep, firmware/board build, CI, flashing, or device behavior is verified here.
USB telemetry and its tests are separate. Raw VBUS remains a wired-power
inference, not proof that charge current is flowing.
