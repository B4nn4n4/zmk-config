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
- `config/west.yml` — `zmk@main` + `zmk-pmw3610-driver@main` for Tractyl trackball.

Base layers (runtime toggle, not separate UF2s):
- `0 BASE_QWERTY`, `1 BASE_COLEMAK` (plain: `Q W F P G / A R S T D`), `2 LOWER/NUM`, `3 RAISE/NAV`, `4 SYSTEM`, `5 MOUSE`, `6 GAMING/FUSION`, `7 RESERVED`
- Toggle: **SYSTEM layer** → `&tog BASE_COLEMAK` / `&tog BASE_QWERTY` (Hatsu: `BT_NXT` row; Zen/Tractyl SYSTEM likewise). Future: save via `CONFIG_ZMK_STUDIO` or combo.

Other shared: `NUM` F-keys/numbers, `NAV` arrows/media, `SYSTEM` BT, `MOUSE` `&mkp/&mmv/&msc`. Tractyl retains `trackball_listener` + combos; Hatsu retains `bat`/`bootl`/`lc` layer-color relay.

## Build pipelines (4 workflows)

- `build-hatsu.yml` → `build-hatsu.yaml` — triggers on `config/hatsu*`, `boards/**`, `dts/**`, `drivers/**`, `src/**`
- `build-zen.yml` → `build-zen.yaml` — triggers on `config/corneish_zen.*`
- `build-tractyl.yml` → `build-tractyl.yaml` — triggers on `config/tracktyl*`, `config/boards/shields/tracktyl/**`
- `build-all.yml` → `build.yaml` (all boards + `settings_reset`) — **unified trigger** for central changes: `config/layouts/**`, `config/behaviors/**`, `config/west.yml`, `build*.yaml`, `.github/workflows/**`

Each uses `zmkfirmware/zmk/.github/workflows/build-user-config.yml@main` with `build_matrix_path` + `config_path: config`. `workflow_dispatch` available.

Local history from `zmk-config-hatsu` (MiaoBreak credits), `zmk-config-zen-2`, `zmk-config-tractyl` merged.

## Credits

Hatsu: [MiaoBreak](https://github.com/hlord2000/MiaoBreak) (hlord2000 / ashleymcdonald) — AW20216S, CW2015, bootloader. See `bootloader/` and `AGENTS.md` gotchas (≤8 char behavior names, `GPREGRET 0x57`, `CONFIG_SETTINGS` both halves, `zmk,physical-layout`).

Zen: Corneish Zen v2 low-profile.

Tractyl: [badjeff pmw3610](https://github.com/badjeff).

## Flashing

- Hatsu: hold **SYSTEM + E (left) / I (right)** → `AM_HATSU` drive → copy UF2. `settings_reset` UF2 wipes bonds if pairing stuck.
- Zen/Tractyl: double-tap reset → copy UF2 (see upstream docs).
- If `CI` fails: `gh run view <id> --log-failed | grep -i error`.
