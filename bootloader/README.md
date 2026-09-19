# Hatsu UF2 Bootloader Installation (one-time, requires hardware debugger)

Flashes the Adafruit nRF52 UF2 bootloader (`angry_miao_hatsu_bootloader-0.8.0_nosd.hex`)
to each Hatsu half. After this, all future firmware updates are copy-a-`.uf2`-to-a-USB-drive.

> **WARNING: This permanently erases the Angry Miao OEM firmware.** The stock firmware
> cannot be dumped (the nRF52840's APPROTECT blocks reads), so there is no way back.

## Requirements

- OpenOCD (`apt install openocd` / `brew install openocd`)
- A **J-Link (clone)** — recommended. Newer Hatsu firmware enables APPROTECT, which the
  ST-Link/OpenOCD `dapdirect_swd` path cannot recover from
  (`Could not find MEM-AP to control the core`). The J-Link path lets OpenOCD's
  `nrf52_recover` mass-erase and unlock the chip automatically.
- ST-Link V2 clones work only on units without APPROTECT.
- 3x DuPont wires + 3 male header pins, case opened (battery disconnected!)

## Wiring

J-Link 20-pin header:
- pin 7 SWDIO, pin 9 SWCLK, pin 4/6 GND -> Hatsu SWD through-holes (tilt pins for contact)
- **jumper VTref (pin 1) to the J-Link's own 3.3V (pin 2)** — required voltage reference

SWD pin locations on both halves (marked G / IO / CLK, left half also has +3V):

<p align="center">
  <img src="pinout.png" alt="Hatsu SWD pinout, left and right halves" />
</p>

Then power the Hatsu half via its USB-C, and plug the J-Link into USB.

## Flash (per half)

```
./flash.sh          # auto-detects jlink/stlink
./flash.sh jlink    # or explicitly
```

If the chip is protected you will see `nRF52 device has AP lock engaged` followed by
`device has been successfully erased and unlocked` — that is the recovery doing its job.
Finish by power-cycling the board; a USB drive named `AM_HATSU` appears.

Do this for **both halves** while the case is open.

## Recovery notes

- Right half has a boot-magic-key: hold the **top-right key** while plugging in USB
  (only works on a true power cycle — i.e. before the battery is connected).
- The settings partition (bonds) lives at `0xd4000`, size `0x20000`. If halves refuse
  to pair, erasing it via debugger fixes it:
  `openocd -f interface/jlink.cfg -c "transport select swd; source [find target/nrf52.cfg]; init; halt; flash erase_address 0xd4000 0x20000; reset; shutdown;"`
