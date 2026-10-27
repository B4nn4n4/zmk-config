# zmk-config (unified)

Unified ZMK config for **3 keyboards × 2 base layouts** — Hatsu / Corneish Zen v2 / Tractyl (nice_nano + tracktyl shields). Targets **upstream ZMK main** (Zephyr 4.1) with shared QWERTY/Colemak.

## Boards

| Board | Build target | Keymap | Conf |
|-------|--------------|--------|------|
| Hatsu | `hatsu_left//zmk`, `hatsu_right//zmk` | `config/hatsu_left.keymap` (symlink `hatsu_right`) | `hatsu_left.conf` / `hatsu_right.conf` |
| Zen v2 | `corneish_zen_left//zmk`, `corneish_zen_right//zmk` | `config/corneish_zen.keymap` | `corneish_zen.conf` |
| Tractyl | `nice_nano//zmk + tracktyl_left/right` | `config/tracktyl.keymap` (symlinks) | `tracktyl*.conf` + `config/boards/shields/tracktyl/` |

## Shared layouts

- `config/layouts/alphas.h` — QWERTY vs Colemak (plain) per-row macros (`QW_R*`, `CM_R*`). Geometries differ so only letters are shared, not full `bindings` vectors.
- `config/behaviors/homerow.dtsi` — single `bhm` 200 ms balanced hold-tap (included via `#include`).
- `config/combos/umlauts.dtsi` — ä/ö/ü/ß combos for all three boards, parameterized by three `UML_ROW*` origin macros each keymap defines (so positions are computed, not hand-listed per board).
- `config/west.yml` — `zmk@main` + `zmk-pmw3610-driver@main` for Tractyl trackball.

Base layers (default Colemak, runtime toggle, not separate UF2s):
- `0 BASE_COLEMAK` (plain: `Q W F P G / A R S T D`), `1 BASE_QWERTY`, `2 LOWER/NUM`, `3 RAISE/NAV`, `4 SYSTEM`, `5 MOUSE`, `6 GAMING`, `7 FUSION`
- Flip QWERTY↔Colemak live via outer `Q+P` chord (`&tog` flip-flop) or **SYSTEM layer** `&tog` keys; reboot always returns to Colemak (runtime-only, nothing persisted). Hatsu shows white while on QWERTY.

Other shared: `NUM` F-keys/numbers, `NAV` arrows/media, `SYSTEM` BT, `MOUSE` `&mkp/&mmv/&msc`. Tractyl retains `trackball_listener` + combos; Hatsu retains `bat`/`bootl`/`lc` layer-color relay.

### Umlauts (all boards)

`X` is the anchor, the other key is the vowel: **X+A = ä, X+O = ö, X+U = ü, X+Z = ß** (`Z` for the "sz" transliteration — QWERTY's `S` shares the ring finger with `X` and can't be chorded). Hold either shift for Ä/Ö/Ü.

Requires the host layout **US, international with AltGr dead keys** — the combos emit `RA(Q)/RA(P)/RA(Y)/RA(S)`, not Unicode. `ö` and `ü` are forked per base layer since those vowels move between Colemak and QWERTY; `ä` and `ß` share positions and are single combos. Active on both bases plus `FUSION`, never on `GAMING`. Needs `CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY=8` (six combos share `X`; ZMK's default cap is 5, counted at compile time regardless of layer scoping).

On Colemak, `O` is the right homerow shift — hold the shift a beat before chording, or `right-shift + X` fires `ö` instead of capital `X`.

### Trackball scrolling (Tractyl)

Hold **LOWER** for vertical, **RAISE** for horizontal. Reachable three ways: the thumbs, the scroll combo (`combo_middleclick`, hold = LOWER) with the right thumb added for horizontal, and the ball-activated `MOUSE` layer, whose Space/Bksp thumbs (pos 30/31) are now layer-taps for the same purpose.

`scroll_h` is declared **before** `scroll_v` in `trackball_listener` deliberately: ZMK evaluates listener overrides top-to-bottom and the first match wins (later overrides *and* the parent's base processors are skipped), so with both layers held RAISE takes precedence and a vertical scroll becomes horizontal.

## Keymap overview

> Interactive version (rendered HTML): [`keymap_overview.html`](https://b4nn4n4.github.io/zmk-config/keymap_overview.html) via GitHub Pages.
> GitHub strips `<script>`/`<style>` from READMEs, so the layer graphics below are embedded SVGs generated from the same data (thumb clusters sit under their respective half).
> Legend: `·` = no key, `▽` = transparent (fall through to lower layer), `KEY (hold)` = tap KEY / hold MOD or layer, `mo` / `tog` / `TO` = momentary / toggle / jump-to layer.

<!-- KEYMAP-OVERVIEW-START -->

### Hatsu · 52 keys
Source: `config/hatsu_left.keymap (symlinked as hatsu_right.keymap) · 52 keys (pos 0–51) · 8 layers · 10 combos`

**Combos (Hatsu · 52 keys)** — 50 ms timeout unless noted

| Name | Keys (pos) | Output | Note |
|---|---|---|---|
| `combo_esc` | 0 + 1 | `ESC` | top-left two keys |
| `combo_base_flip` | 1 + 10 | `&tog BASE_QWERTY (flip-flop)` | Q + ; — toggles Colemak ↔ QWERTY |
| `combo_bootload` | 44 + 47 | `&bootl (UF2)` | hidden thumb chord (both ∅ on base layers) |
| `combo_sysreset` | 45 + 46 | `&sys_reset` | hidden thumb chord (both ∅ on base layers) |
| `combo_ae` | 26 + 13 | `ä — &kp RA(Q)` | X + A · both bases · **40 ms** ("ax" is a real bigram) |
| `combo_ss` | 26 + 25 | `ß — &kp RA(S)` | X + Z · both bases · Z not S ("sz"); QWERTY S shares the ring finger with X |
| `combo_oe_cm` | 26 + 22 | `ö — &kp RA(P)` | X + O · Colemak only |
| `combo_ue_cm` | 26 + 8 | `ü — &kp RA(Y)` | X + U · Colemak only |
| `combo_oe_qw` | 26 + 9 | `ö — &kp RA(P)` | X + O · QWERTY + FUSION |
| `combo_ue_qw` | 26 + 7 | `ü — &kp RA(Y)` | X + U · QWERTY + FUSION |

![Layer overview — Hatsu · 52 keys](docs/keymap-hatsu.svg)

### Corneish Zen · 42 keys
Source: `config/corneish_zen.keymap · 42 keys (pos 0–41: 3×12 + 6 thumbs) · 8 layers · 10 combos`

**Combos (Corneish Zen · 42 keys)** — 50 ms timeout unless noted

| Name | Keys (pos) | Output | Note |
|---|---|---|---|
| `combo_esc` | 1 + 2 | `ESC` | Q + W |
| `combo_base_flip` | 1 + 10 | `&tog BASE_QWERTY (flip-flop)` | Q + ; — toggles Colemak ↔ QWERTY |
| `combo_bootload` | 0 + 11 | `&bootloader` | outer top-row ∅ pair |
| `combo_sysreset` | 12 + 23 | `&sys_reset` | outer homerow ∅ pair |
| `combo_ae` | 26 + 13 | `ä — &kp RA(Q)` | X + A · both bases · **40 ms** ("ax" is a real bigram) |
| `combo_ss` | 26 + 25 | `ß — &kp RA(S)` | X + Z · both bases · Z not S ("sz"); QWERTY S shares the ring finger with X |
| `combo_oe_cm` | 26 + 22 | `ö — &kp RA(P)` | X + O · Colemak only |
| `combo_ue_cm` | 26 + 8 | `ü — &kp RA(Y)` | X + U · Colemak only |
| `combo_oe_qw` | 26 + 9 | `ö — &kp RA(P)` | X + O · QWERTY + FUSION |
| `combo_ue_qw` | 26 + 7 | `ü — &kp RA(Y)` | X + U · QWERTY + FUSION |

![Layer overview — Corneish Zen · 42 keys](docs/keymap-zen.svg)

### Tractyl · 33 keys
Source: `config/tracktyl.keymap + shields/tracktyl/tracktyl.dtsi transform (3×10 + 3 thumbs) · 33 keys (pos 0–32) · 8 layers · 12 combos`

**Combos (Tractyl · 33 keys)** — 50 ms timeout unless noted

| Name | Keys (pos) | Output | Note |
|---|---|---|---|
| `combo_esc` | 0 + 1 | `ESC` | Q + W |
| `combo_middleclick` | 12 + 13 | `&mlt 2 MCLK (tap MCLK / hold LOWER)` | **scroll combo** — holding it puts the trackball in vertical scroll |
| `combo_middlepress` | 16 + 17 | `&mkp MCLK` | N+E position pair |
| `combo_bootload` | 0 + 30 | `&bootloader` | Q + LOWER/Space thumb |
| `combo_sysreset` | 0 + 24 | `&sys_reset` | Q + B |
| `combo_base_flip` | 0 + 9 | `&tog BASE_QWERTY (flip-flop)` | Q + ; — toggles Colemak ↔ QWERTY |
| `combo_ae` | 21 + 10 | `ä — &kp RA(Q)` | X + A · both bases · **40 ms** ("ax" is a real bigram) |
| `combo_ss` | 21 + 20 | `ß — &kp RA(S)` | X + Z · both bases · Z not S ("sz"); QWERTY S shares the ring finger with X |
| `combo_oe_cm` | 21 + 19 | `ö — &kp RA(P)` | X + O · Colemak only |
| `combo_ue_cm` | 21 + 7 | `ü — &kp RA(Y)` | X + U · Colemak only |
| `combo_oe_qw` | 21 + 8 | `ö — &kp RA(P)` | X + O · QWERTY + FUSION |
| `combo_ue_qw` | 21 + 6 | `ü — &kp RA(Y)` | X + U · QWERTY + FUSION |

![Layer overview — Tractyl · 33 keys](docs/keymap-tractyl.svg)


<!-- KEYMAP-OVERVIEW-END -->

## Charging state on the host (Waybar)

All three boards report USB charge state to the BLE host. Plain BAS (`0x2A19`) carries
a battery *level* only, so this uses the BAS 1.1 **Battery Level Status**
characteristic (`0x2BED`), which has a charge-state field. Zephyr implements the
characteristic but ZMK never populates it, so `src/charge_state.c` drives it from
`zmk_usb_is_powered()` — enabled per central half with:

```
CONFIG_ZMK_CHARGE_STATE_REPORTING=y
```

in `config/hatsu_left.conf`, `config/tracktyl_left.conf`, `config/corneish_zen_left.conf`.

Host side, `host/waybar-zmk-charging.sh` reads `0x2BED` over D-Bus (BlueZ's battery
plugin only reads `0x2A19` and `org.bluez.Battery1` has no charging property — the
per-component battery objects that do are still behind BlueZ's experimental D-Bus
flag):

```jsonc
"custom/keeb-charging": {
  "exec": "~/Repos/zmk-config/host/waybar-zmk-charging.sh hatsu",
  "return-type": "json",
  "interval": 30
}
```

Empty `text` when on battery, so the module hides itself; `ZMK_SHOW_IDLE=1` keeps it
visible. Classes: `charging` / `discharging` / `absent`.

### USB battery reporting

All three central halves also enable `src/usb_battery.c` with:

```conf
CONFIG_ZMK_USB_BATTERY_REPORTING=y
CONFIG_USB_HID_DEVICE_COUNT=3
```

This adds **HID_1 for the left/central battery** and **HID_2 for the right/peripheral
battery**. Both expose their own percentage and inferred charging state through
the central's USB data connection. Each has separate endpoint buffers and battery
reports, so older Linux kernels do not merge the two batteries into one device.
ZMK still owns **HID_0**, and Studio's USB serial interface remains separate. The
central battery's USB reporting is independent of BLE and
`CONFIG_ZMK_CHARGE_STATE_REPORTING`; the right battery needs the internal BLE split
connection, not a Bluetooth connection to the host. Peripheral firmware does not
enable the USB stack.

**Flash both halves from the updated build.** All three shared keymaps include
`config/telemetry/split_battery.dtsi`. This opts both roles into
`CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY`: a virtual input source and dedicated
`zmk,input-split` channel at register 254. It has no normal input listener or input
processors, so telemetry cannot generate key presses or pointer movement. Upstream
requires `ZMK_POINTING` for this transport; enabling it can also add mouse
capabilities to HID_0 on previously non-pointing builds. Existing trackball channels
and the stock split battery/BLE BAS values are not repurposed. A hidden `sbat`
behavior carries a new central challenge on each split connection; the peripheral
must echo that session token in every telemetry packet. Old queued packets cannot
restore availability after reconnect. The challenge is retried while waiting for
telemetry, including while split discovery/subscription is still starting.

`src/split_battery.c` polls the peripheral's raw nRF52840 VBUS detector once a
second. It relays changed state and a ten-second heartbeat with the cached ZMK
battery percentage. Unlike `zmk_usb_is_powered()`, this raw detector does not need
the peripheral USB stack. **VBUS wiring on these boards is still unverified**:
test the right cable both connected and disconnected before treating the flag as
reliable. No charger-current/termination or Qi detection is available.

Reset images disable BLE but can retain the keymap's telemetry DT nodes. Their
input-split proxy still needs the virtual source device. CMake therefore includes
`src/split_battery.c` based on DT presence; only its telemetry runtime is gated by
`CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY`. The inert device remains defined without
starting telemetry work or pulling BLE dependencies into a reset build.

The numbered Input report is four bytes: `01 (percentage+1) charging 00`. Battery
Strength (usage page `0x06`, usage `0x20`) is the first payload byte, logical range
1–101. Linux scales this exactly back to 0–100%, avoiding older kernels' rejection
of raw-zero interrupt capacity updates. The vendor Feature below uses plain percent;
Charging (page `0x85`, usage `0x44`) occupies bit 0 of the next byte. The final byte
is an always-released Consumer Mute bit with padding. This gives Linux an ordinary
input capability so it does not discard a battery-only interface; it never sends
a Mute press. `GET_REPORT` returns a fresh snapshot of ZMK's cached battery level
and USB power state. Interrupt reports run on battery/USB events and every 30
seconds while configured, with one-second retries on write errors. Suspended USB
is not woken for telemetry; resume/reconnect restarts reports.

Both interfaces also expose vendor Feature report **2** as
`02 available percentage charging`. `available=0` means the remaining fields are
not measurements. Before receiving a right sample, after its split disconnect,
or after 35 seconds without telemetry, the firmware withholds right Input reports
and fails right Input `GET_REPORT` with `-ENODATA`; it never fabricates a 0% or
"discharging" reading for missing data. Reconnection alone does not restore
validity. A new sample is required. Percentage is ZMK's cached gauge reading, not
a guarantee of a fresh sensor conversion; ZMK starts at 0% before its first sample
and can pause gauge updates during idle.

On **Linux 6.3+**, generic HID supports these usages as a device-scoped
`power_supply` battery with percentage and Charging/Discharging status. UPower can
consume that battery without a custom GATT reader. Linux **6.17+** also notifies
charging-state changes immediately; older kernels can need polling. The offset
capacity encoding also makes a genuine empty battery visible on older kernels.
Interrupt delivery requires an opened input/hidraw device, normally supplied by
the desktop input stack; headless hosts may need a reader holding it open. macOS
and Windows battery UI support is not established. This is not a UPS interface.

**Generic Linux HID/UPower can retain the last right-half reading after a split
disconnect.** Suppressing reports cannot invalidate that host cache, and the two
USB battery interfaces remain enumerated while the left USB cable is connected.
Native desktop entries are therefore not proof that the right half is still
connected or charging. Consumers needing strict freshness must read Feature 2's
validity; native UPower does not consume that vendor field. Both entries may use
the same keyboard product name; distinguish them by their USB/HID interface path.

The existing `host/waybar-zmk-charging.sh` remains **BLE-only**. For USB, use the
kernel battery/UPower path. After building and flashing **both halves**, keep the
internal split connected and disable only the host's Bluetooth connection:

```sh
for battery in /sys/class/power_supply/hid-*-battery; do
    [ -d "$battery" ] || continue
    printf '%s\n' "$battery"
    cat "$battery/capacity" "$battery/status" "$battery/scope"
done
upower --enumerate
```

Find both keyboard device batteries, then inspect their UPower paths with
`upower --show-info <path>`. The left should report wired power while its USB data
cable is connected. Connect/disconnect the **right** charging cable independently:
only the right charging flag should change, with its own percentage. Check typing,
mouse reports and Studio still work. Check suspend/resume, unplug/replug, and split
disconnect/reconnect. Inspect Feature 2 when testing missing/stale right data, not
just a cached desktop value.

Both USB batteries **disappear when the left data cable is unplugged**; they cannot
report over a removed USB link. BLE reporting is still needed for wireless host
status, and its existing charging characteristic still describes the central only.
At 100%, UPower may display FullyCharged even while a VBUS-based flag remains set.
A sleeping peripheral stops its telemetry; the cache then expires. Polling raw VBUS
does not itself add peripheral sleep prevention or a new wake source.

Local regression tests compile the actual module against API stubs and parse its
HID descriptor (no firmware toolchain or external Python packages needed):

```sh
python3 -B -m unittest discover -s tests/usb_battery -v
python3 -B -m unittest discover -s tests/split_battery -v
# Or, when Python/GCC are not installed:
nix shell nixpkgs#python3 nixpkgs#gcc --command python3 -B -m unittest discover -s tests/usb_battery -v
```

See `tests/usb_battery/README.md` and `tests/split_battery/README.md` for coverage,
limitations and FHS/Nix environment troubleshooting. These tests do not replace a
Zephyr firmware build or USB enumeration on hardware.

### Limitations

- **The BLE charging characteristic remains central-only.** USB additionally exposes
  one peripheral via the private input-split relay. More than one peripheral is
  deliberately rejected at compile time instead of merging their states.
- **Right-half VBUS sensing is not hardware-verified.** Reading the nRF POWER
  register bypasses ZMK's central-only USB dependency, but it cannot establish that
  a particular board physically routes its charging connector to that detector.
- **"Charging" means VBUS present.** No board here has a charger-status interface, so
  a full battery left plugged in still reads as charging. Same signal the on-keyboard
  indicator uses.
- **Qi charging is not firmware-detectable.** The BLE wireless-power-source field
  reports `UNKNOWN`, not "not connected". The USB Charging bit only reflects VBUS;
  it cannot encode a separate Qi/unknown state.
- **USB reports require a central data connection**, not a charge-only cable. The
  right half additionally requires a live internal split link. The separate BLE
  central signal remains available without a USB host connection.

## Build pipelines

`build-all.yml` is the only automatic entry point for pushes and pull requests.
It checks `.github/build-filters.yml` and calls each affected board workflow once:

- Shared layouts, behaviors, combos, manifest, charge-state sources
  (`src/charge_state.c`, `src/usb_battery.c`, `src/split_battery.c`), split telemetry
  behavior, headers, bindings and `config/telemetry/**`, module metadata,
  build-system files, or `.github/**` changes → all three boards, including Hatsu.
- Board-only changes → only affected boards, even when several boards change together.
- Shared plus board-only changes → all three boards, with no duplicate board jobs.
- Unrelated changes → path check only; no firmware builds.

Board workflows are reusable and also support manual `workflow_dispatch` runs:

- `build-hatsu.yml` → `build-hatsu.yaml` (includes Hatsu `settings_reset` images).
- `build-zen.yml` → `build-zen.yaml`.
- `build-tractyl.yml` → `build-tractyl.yaml`.

Selected board pipelines run sequentially: Hatsu, then Zen, then Tractyl. ZMK's
upstream artifact merge reads run-wide inputs, so parallel board pipelines can
interfere with each other's artifact downloads. Each board waits for the previous
pipeline, including its merge job. Skipped boards do not block selected boards;
a failed pipeline stops later boards to avoid merging incomplete leftovers.
Builds within one board's matrix can still run in parallel.

Manual **Build All** selects all three board workflows. `build-hatsu-debug.yml` and
`build-reset.yml` remain manual-only. Manually requested runs are independent;
they are not deduplicated against automatic runs. `build.yaml` is a legacy Zen +
Tractyl matrix; the automatic workflow uses the three board-specific matrices.

Each board workflow uses `zmkfirmware/zmk/.github/workflows/build-user-config.yml@main`
with `build_matrix_path` + `config_path: config`. Artifacts keep their board-specific
names: `hatsu-firmware`, `zen-firmware`, and `tractyl-firmware`.

Local routing tests (Python with PyYAML):
`python3 -m unittest discover -s tests -v`. Check workflow syntax with
`actionlint .github/workflows/*.yml`.

Local history from `zmk-config-hatsu` (MiaoBreak credits), `zmk-config-zen-2`, `zmk-config-tractyl` merged.

## Credits

Hatsu: [MiaoBreak](https://github.com/hlord2000/MiaoBreak) (hlord2000 / ashleymcdonald) — AW20216S, CW2015, bootloader. See `bootloader/` and `AGENTS.md` gotchas (≤8 char behavior names, `GPREGRET 0x57`, `CONFIG_SETTINGS` both halves, `zmk,physical-layout`).

Zen: Corneish Zen v2 low-profile.

Tractyl: [badjeff pmw3610](https://github.com/badjeff).

## Flashing

- Hatsu: hold **SYSTEM + E (left) / I (right)** → `AM_HATSU` drive → copy UF2. `settings_reset` UF2 wipes bonds if pairing stuck.
- Zen/Tractyl: double-tap reset → copy UF2 (see upstream docs).
- If `CI` fails: `gh run view <id> --log-failed | grep -i error`.
