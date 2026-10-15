#!/usr/bin/env bash
# Waybar custom module: charging state of a ZMK keyboard over BLE.
#
# Reads the BAS 1.1 "Battery Level Status" characteristic (0x2BED) that
# src/charge_state.c populates. Requires CONFIG_ZMK_CHARGE_STATE_REPORTING=y on
# the central half.
#
# Goes straight to GATT over D-Bus rather than through UPower: BlueZ's battery
# plugin only reads the BAS 1.0 level (0x2A19) and has no charging property on
# org.bluez.Battery1 (per-component battery objects that do are still behind
# BlueZ's experimental D-Bus flag).
#
# Reports the central half only — the half you actually plug the cable into.
#
# Usage: waybar-zmk-charging.sh [name-substring]
# Env:   ZMK_SHOW_IDLE=1  keep the module visible when not charging

set -uo pipefail

BLS_UUID="00002bed-0000-1000-8000-00805f9b34fb"
FILTER="${1:-}"

emit() { # $1 = text, $2 = tooltip, $3 = class
    printf '{"text":"%s","tooltip":"%s","class":"%s"}\n' "$1" "$2" "$3"
}

objects=$(busctl --json=short call org.bluez / \
    org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null) ||
    { emit "" "bluez unavailable" absent; exit 0; }

# Connected devices matching $FILTER, as "path<TAB>name"
devices=$(jq -r --arg f "$FILTER" '
    .data[0] | to_entries[]
    | select(.value["org.bluez.Device1"].Connected.data == true)
    | (.value["org.bluez.Device1"].Name.data // "") as $n
    | select($f == "" or ($n | ascii_downcase | contains($f | ascii_downcase)))
    | "\(.key)\t\($n)"' <<<"$objects")

chars=$(jq -r --arg u "$BLS_UUID" '
    .data[0] | to_entries[]
    | select(.value["org.bluez.GattCharacteristic1"].UUID.data == $u)
    | .key' <<<"$objects")

charging=0
found=0
name=""

while IFS=$'\t' read -r devpath devname; do
    [ -n "$devpath" ] || continue
    while read -r charpath; do
        case "$charpath" in "$devpath"/*) ;; *) continue ;; esac
        # 3-byte value: flags, then power_state as uint16 LE.
        state=$(busctl --json=short call org.bluez "$charpath" \
            org.bluez.GattCharacteristic1 ReadValue 'a{sv}' 0 2>/dev/null |
            jq -r '.data[0] as $b
                   | if ($b | length) < 3 then empty
                     else (($b[2] * 256) + $b[1]) end')
        [ -n "$state" ] || continue
        found=1
        name="${devname:-keyboard}"
        # power_state bits 5-6 = charge state; 1 == charging.
        [ $(((state >> 5) & 3)) -eq 1 ] && charging=1
    done <<<"$chars"
done <<<"$devices"

if [ "$found" -eq 0 ]; then
    emit "" "no ZMK keyboard reporting charge state" absent
    exit 0
fi

if [ "$charging" -eq 1 ]; then
    emit "󰂄" "$name: charging" charging
elif [ -n "${ZMK_SHOW_IDLE:-}" ]; then
    emit "󰂎" "$name: on battery" discharging
else
    emit "" "$name: on battery" discharging
fi
