# TODO — Hatsu firmware roadmap

## Verify on hardware

- Battery indicator states (priority order): `&bat` green bar > per-layer colors incl.
  system=blue > full solid green at ≥80% (all 4 LEDs, no pulse) > USB charging pulse
  (outranks low-batt so plugged-in cells show charge progress) > SoC < 20% solid red
  (unplugged only); default layer dark otherwise.
- Mirrored layer colors on right half: toggle num/function/gaming/system/mouse → both
  halves match (incl. stacked layers); idle-sleep then wake on default layer → BOTH
  dark (peripheral reconnect resend); charging pulse stays per-half (VBUS sensing)
- Deep sleep: halves sleep, wake on keypress, re-pair cleanly, indicator resumes
- Overnight USB charge completes to 100% (device stays awake while plugged via
  `CONFIG_ZMK_SLEEP`, so VBUS is never dropped); Qi pad charge also works — battery
  reports full next morning (Qi itself isn't detected/indicated by firmware)

## Done

- [x] Per-layer RGB indication: top-level `layer_colors` node in the keymap
      (`compatible = "zmk,layer-colors"`, one uint32 per layer index, 0 = none) selected
      via `chosen { zmk,layer-colors = &layer_colors; }`; num=cyan, function=magenta,
      gaming=orange, mouse=yellow, system keeps BT-profile display. Replaces old
      upper/lower white patterns and their Kconfig entries. NOTE: an inline-subnode
      scheme (`indicator { color = ...; }` under each layer + dedicated binding) kept
      silently dropping props in CI even with the binding present — chosen-node table is
      the proven path; BUILD_ASSERTs in battery_indicator.c guard against drops
- [x] RGB battery indicator rework: state-based render loop with layer/BLE/battery/USB/
      activity event listeners; pink default removed; `&bat` bar now green; gentle idle
      sweep animation; layer 2 = white `[on on off off]`, layer 3 = white `[off off on on]`
- [x] Layers: `upper`(2) + `lower`(3) inserted (all `&trans`), 2 `reserved` layers added;
      `system` moved to index 4 (fixed missing thumb row: 48 → 52 bindings)
- [x] Deep sleep: `CONFIG_ZMK_SLEEP=y` + `wakeup-source` on both kscan nodes
- [x] Charge indication: green pulsing bar at SoC length while USB powered
      (`CONFIG_ZMK_BATTERY_INDICATOR_CHARGING`); no charger IC interface exists, so USB
      power is the only detectable charging source
- [x] Board port to upstream ZMK (Zephyr 4.1, HWMv2)
- [x] AW20216S + CW2015 out-of-tree drivers
- [x] ZMK Studio (physical layout with rotated thumbs)
- [x] `&bootl` disassembly-free UF2 flashing on both halves
- [x] Persistent split pairing (settings on both halves)
- [x] settings_reset images in CI
- [x] Per-half bootloaders (`AM_HATSU_L`/`AM_HATSU_R`) at ashleymcdonald/Adafruit_nRF52_Bootloader
