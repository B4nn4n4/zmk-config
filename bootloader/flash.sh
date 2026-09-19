#!/bin/bash
# Usage: ./flash.sh [jlink|stlink]
# Defaults to auto-detecting a connected debugger.

set -e

adapter="$1"

if [ -z "$adapter" ]; then
    if lsusb | grep -qi "1366:"; then
        adapter="jlink"
    elif lsusb | grep -qi "0483:3748"; then
        adapter="stlink"
    else
        echo "No supported debugger found. Plug in a J-Link or ST-Link." >&2
        exit 1
    fi
    echo "Auto-detected debugger: $adapter"
fi

if [ "$adapter" = "jlink" ]; then
    # Newer Hatsu firmware enables APPROTECT (access port protection).
    # nrf52_recover (run inside flash_bootloader) mass-erases the chip and
    # clears APPROTECT. This destroys the OEM firmware permanently.
    openocd -f "interface/jlink.cfg" -f "angry_miao.cfg" \
        -c "transport select swd; source [find target/nrf52.cfg]; init; sleep 2000; flash_bootloader;"
elif [ "$adapter" = "stlink" ]; then
    # NOTE: ST-Link with dapdirect_swd cannot recover a chip with APPROTECT
    # enabled ("Could not find MEM-AP to control the core"). If you hit that
    # error, use a J-Link instead.
    openocd -f "interface/stlink-dap.cfg" -f "angry_miao.cfg" \
        -c "transport select dapdirect_swd; source [find target/nrf52.cfg]; init; sleep 2000; flash_bootloader;"
else
    echo "Unknown adapter '$adapter'. Use 'jlink' or 'stlink'." >&2
    exit 1
fi
