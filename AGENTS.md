# AGENTS.md — zmk-config-hatsu

ZMK firmware for the Angry Miao Hatsu split keyboard (nRF52840, Adafruit UF2 bootloader).
Targets **upstream ZMK main** (Zephyr 4.1). GitHub Actions builds all firmware; there is
no local toolchain here.

## Build & release flow

- Push / pull request → `.github/workflows/build-all.yml` selects affected boards
  using `.github/build-filters.yml`; shared changes build all three boards once.
  Board workflows are reusable and manual-only, with no independent push/PR triggers.
  Selected pipelines run sequentially (Hatsu → Zen → Tractyl): upstream artifact
  merges use run-wide inputs. Keep the dependency chain and explicit status checks
  so skipped boards do not block later selections; failures stop later pipelines.
  Download artifacts into `out/firmware/` with `gh run download <id> -D out`.
- `build-hatsu.yaml` builds: `hatsu_left//zmk` (with `studio-rpc-usb-uart` snippet),
  `hatsu_right//zmk`, and `settings_reset` images for both halves
- CI iteration is the norm: expect a few push/fail/fix cycles; read failures with
  `gh run view <id> --log-failed | grep -i error`

## Flashing

- Normal path (no debugger): SYSTEM + `E` (left) / SYSTEM + `I` (right) → `AM_HATSU`
  drive → copy uf2. This is the custom `&bootl` behavior (writes `0x57` to
  `NRF_POWER->GPREGRET` then `sys_reboot()`).
- J-Link recovery: `openocd -f interface/jlink.cfg -c "transport select swd; source
  [find target/nrf52.cfg]; init; reset halt; program <firmware>.bin 0x1000 verify reset
  exit"` — **never mass-erase** (kills the UF2 bootloader at 0xf4000). Convert uf2→bin
  with the python snippet used in git history (UF2 base 0x1000).
- Settings partition at `0xd4000` size `0x20000` holds bonds; erase it via J-Link or
  flash `settings_reset-*.uf2` to both halves when pairing is stuck.

## Repo layout

- `boards/angry_miao/hatsu_{left,right}/` — HWMv2 board defs. dts/defconfig files must be
  named `<board>_nrf52840_zmk.{dts,overlay,defconfig}` (qualifier-suffixed)
- `dts/hatsu.dtsi` — shared: kscan transform, flash partitions, usbd, boot-magic keys
- `dts/hatsu_physical_layout.dtsi` — 52-key layout (centi-keyunits, centi-degrees)
- `drivers/led/aw20216s.c` — LED driver; children ordered = LED numbers, `index` prop =
  PWM register. Callers pass child **ordinal** (`DT_NODE_CHILD_IDX`)
- `drivers/sensor/cw2015/` — fuel gauge (read-only; no charge control exists)
- `src/` — backlight-map (custom `zmk_backlight_*` impl), battery_indicator, behaviors.
  Battery indicator doubles as the layer RGB display: per-layer colors live in a
  top-level `layer_colors` node in the keymap (`compatible = "zmk,layer-colors"`, one
  uint32 per layer index — node names are offset by one, `layer_N` = index N-1;
  system(4)=blue, reserved/default=0), selected via `chosen { zmk,layer-colors =
  &layer_colors; }`. Layer state exists only on the central; peripherals receive the
  active color through the `lc` relay behavior (see gotchas below). Indicator states,
  ascending precedence: dark < low-batt red (unplugged only) < charging pulse < full
  solid green (≥80%) < layer colors < `&bat` bar; there is deliberately no
  BT-profile display
- `config/hatsu_left.keymap` is the single source of truth; `config/hatsu_right.keymap`
  is a git **symlink** to it (T in `git status` = typechange). The build requires a
  file with that exact name per shield, and GLOBAL behaviors (`bootl`, `bat`) must
  exist in the right firmware's devicetree even though the peripheral resolves no
  bindings itself. Never convert back to separate files. Custom behavior nodes live
  in the keymap overlay (ZMK compiles keymaps against a stub dts — board dts nodes
  are NOT visible there)
- `src/charge_state.c` — shared across boards (Zen and Tractyl build it too):
  drives Zephyr's BAS 1.1 Battery Level Status characteristic (`0x2BED`) from
  `zmk_usb_is_powered()` so the BLE host can show charging state. Gated by
  `CONFIG_ZMK_CHARGE_STATE_REPORTING`, central-only. Its `SYS_INIT` only QUEUES the
  update onto the lowprio workqueue: Zephyr's `bas_init()` zeroes the BLS power-state
  bitfield at the same init priority, and work items don't run until every `SYS_INIT`
  has returned, so queueing is what makes it clobber-proof. Because it is
  shared, `.github/build-filters.yml` selects all three board workflows for changes
  to `src/charge_state.c`, `Kconfig` and `CMakeLists.txt`
- `src/usb_battery.c` — shared central-only USB battery telemetry, gated by
  `CONFIG_ZMK_USB_BATTERY_REPORTING` (independent of BLE). All three central configs
  also set `CONFIG_USB_HID_DEVICE_COUNT=2`. Owns `HID_1`; never touch ZMK's `HID_0`
  reports or call `usb_enable()` again. Registers at `CONFIG_ZMK_USB_HID_INIT_PRIORITY`
  before ZMK enables USB. Report ID 1: percent, Charging bit, always-zero Consumer
  Mute byte; this input capability prevents Linux discarding the battery interface.
  Uses two persistent RAM buffers, advancing only when the driver accepts a write,
  so pending DMA remains intact and aborted transfers cannot strand a semaphore.
  Reports on USB/battery events and every 30 s; never wakes suspended USB.
  `.github/build-filters.yml` selects all three board workflows for this file too.
  Local regression tests: `python3 -m unittest discover -s tests/usb_battery -v`
  (Python + GCC required).
- `config/west.yml` — pins `zmkfirmware/zmk@main`; `config/hatsu_left.conf` enables Studio

## Hard-won gotchas (do not rediscover)

- **Custom behavior node names must be ≤ 8 characters** or they silently fail to invoke
  on split peripherals (nodes are named `bat`/`bootl` for this reason)
- Zephyr ≥ 4 removed the nRF `sys_reboot(type)` → GPREGRET write; the Zephyr
  retention/bootmode path does not work reliably on the right half. Direct GPREGRET
  write + system reset works on both
- Left and right half row-gpio orders differ (right is NOT a copy of the left; getting
  this wrong mirrors all keys) — keep in sync with the original MiaoBreak dts
- `zmk,physical-layout` (with `keys`) replaces the `zmk,matrix-transform` chosen node
- Boards need `select ZMK_BOARD_COMPAT if BOARD_<NAME>_NRF52840_ZMK` and the
  `uf2_boot_mode.dtsi` include; dts files need `/dts-v1/;`
- phandle-array `led-channels` requires `#led-channel-cells = <0>;` in each LED child
- `CONFIG_SETTINGS` must be on BOTH halves or split pairing is lost on reset
- Right half is the BLE peripheral: no USB HID output, no keymap of its own (central
  resolves; behaviors route by locality)
- Nodes whose compatible matches NO loaded binding lose all non-whitelisted props
  SILENTLY (edtlib treats the node as unbound and skips validation); with a matching
  binding, undeclared props become a hard error instead. Keymap layer children inherit
  the `zmk,keymap` child-binding recursively. Even a dedicated subnode binding
  (`hatsu,layer-indicator` + `compatible` on each `indicator {}`) still dropped props in
  CI for reasons never pinned down — do NOT retry that scheme; use the separate
  `zmk,layer-colors` chosen node (proven to compile green). BUILD_ASSERTs in
  battery_indicator.c (`DT_PROP_BY_IDX != 0`) catch silent drops at compile time
- Module source files are listed EXPLICITLY in the root `CMakeLists.txt` — a new .c
  file that isn't added compiles into NOTHING and fails silently (e.g. a behavior
  device simply never exists; runtime name-lookup invocations just log a warning and
  no-op). When adding sources, always extend the CMakeLists list
- ZMK compiles several events' implementations ONLY into the central build
  (`app/CMakeLists.txt` wraps e.g. `layer_state_changed.c`, `endpoint_changed.c`,
  `ble_active_profile_changed.c` in the `(!SPLIT || ROLE_CENTRAL)` block). Referencing
  their `as_*()` accessors unconditionally breaks the PERIPHERAL link — gate every
  reference with `KEYMAP_LOCAL` (`activity_state_changed`, `battery_state_changed`,
  `position_state_changed`, `split_peripheral_status_changed` compile on both halves)
- `CONFIG_ZMK_USB` is `depends on (!ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL)` upstream, so
  `CONFIG_ZMK_USB=y` in `hatsu_right_nrf52840_zmk_defconfig` is SILENTLY DROPPED as an
  unmet dependency — `zmk_usb_is_powered()` can never be true on the right half no
  matter how VBUS is wired. The "no VBUS sensing on the right half" note below may
  therefore be a false negative: the hardware bit (`POWER->USBREGSTATUS` via
  `nrf_power_usbregstatus_vbusdet_get()`) is readable without ZMK's USB stack and has
  never actually been tested. Peripheral→central data has no user-extensible channel,
  but the split transport's existing `BATTERY_EVENT` / `SENSOR_EVENT` / `INPUT_EVENT`
  carriers can be repurposed without forking ZMK (see README limitations)
- `SYS_INIT(fn, LEVEL, prio)` stringifies `prio` into the linker section name
  `.z_init_<LEVEL><prio>_0_`, so it accepts only a decimal literal or a symbol that *is*
  one — **arithmetic is not permitted** (`init.h` names
  `CONFIG_KERNEL_INIT_PRIORITY_DEFAULT + 5` as the counter-example). An expression yields
  a section no level pattern matches; it lands in the `initlevel_error` catch-all and
  trips `ASSERT(SIZEOF(initlevel_error) == 0, "Undefined initialization levels used.")`
  at LINK time, long after the file compiles clean. To order after another SYS_INIT at
  the same priority, queue work instead of bumping the number
- Layer colors mirror to the peripheral via the hidden GLOBAL behavior `lc`
  (`src/behaviors/behavior_layer_color.c`, never bound to a key): the central invokes it
  programmatically on layer/activity/peripheral-reconnect events, and
  `zmk_behavior_invoke_binding()` fans GLOBAL invocations out over the split transport
  with param1 carrying the RGB24 color. Limitations: charging pulse can NOT be mirrored
  (right-half USB is charge-only, no VBUS sensing); if upstream lands split layer-state
  sync (zmk issue #3376) this relay can be replaced wholesale

## Hardware reference

- Per half: AW20216S (SPI) driving 26 white backlight LEDs + 4 RGB LEDs (battery bar),
  CW2015 fuel gauge (I2C), no reset button, no charge-control interface found
- Sleep: upstream `CONFIG_ZMK_SLEEP=y` keeps the device awake while USB/charging is
  present (ZMK's `is_usb_power_present()` guard), so the host never deasserts VBUS and
  overnight USB charging completes; it deep-sleeps only when fully unplugged. Qi
  charging is NOT detected by ZMK (`zmk_usb_is_powered()` is the only power source
  check) but still charges during deep sleep via the hardware receiver (not firmware-
  detectable/indicatable; schematics are confidential)
- SWD pads: GND/IO/CLK (+3V on left), photo in `bootloader/pinout.png`
- Backlight: `&bl` on SYSTEM layer; battery bar: `&bat`; profile/pair LEDs: see TODO.md
