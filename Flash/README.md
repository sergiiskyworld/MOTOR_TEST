# Prebuilt firmware

Firmware **v1.0.0** for the F405 flight controller (STM32F405RGT6), built
from the tagged source with `pio run -d firmware`.

Flash usage is 44 588 bytes of the 1 MB on the chip; RAM at rest is 6 328
bytes of 128 KB.

## Which file do I want

| File | Use it with | Load address |
|---|---|---|
| `firmware-v1.0.0.hex` | STM32CubeProgrammer, ST-Link, most GUI flashers | carried inside the file |
| `firmware-v1.0.0.bin` | `dfu-util`, and anything that wants a raw image | you must supply `0x08000000` |
| `firmware-v1.0.0.elf` | gdb, OpenOCD, disassembly | not for flashing |

If you are flashing by hand and are not sure, take the `.hex`. It carries
its own load address, so there is no way to put it at the wrong offset. The
`.bin` has no address in it: give the wrong one and the board will not boot.

There is no DfuSe `.dfu` here. The `dfu-prefix` tool shipped with dfu-util
only writes TI Stellaris prefixes, not DfuSe ones, and an untested flashing
file is worse than no file. STM32CubeProgrammer reads `.hex` and `.bin`
over DFU perfectly well.

## Entering the bootloader

Hold the **BOOT** button on the board and, while holding it, plug in USB.
There is no software way to do it on this board; what was tried and why each
attempt failed is in [docs/FLASHING.md](../docs/FLASHING.md).

## Flashing

With dfu-util, which is what the repository's own script uses:

```bash
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D firmware-v1.0.0.bin
```

With STM32CubeProgrammer, pick USB, connect, open the `.hex` and press
"Download". Leave the start address alone; the file sets it.

Building and flashing from source in one step, which also takes a backup of
whatever is on the board first:

```bash
./scripts/flash.sh
```

## Checksums

```
c73391bebd7cf07b9a37ef6589d4b863d117b360e0fee036042dc5b797f07fac  firmware-v1.0.0.bin
dd3f2bd08c0f228bc8412cd78ba6c706b51cc800303e62f255c74ae9d8e6bc94  firmware-v1.0.0.elf
ed1e733fc1a83c33a51bc64965438648df03d8a4ccaaa6709d7c09efc5912ae6  firmware-v1.0.0.hex
```

Verify with `shasum -a 256 -c` against this list, or `sha256sum -c` on Linux.

## What flashing this destroys

The image is written from the very start of flash, so it overwrites whatever
was on the board before — Betaflight, ArduPilot or the vendor's own build —
together with that firmware's bootloader. The ROM DFU bootloader in system
memory is not touched and cannot be erased, so the board is always
recoverable with the BOOT button.
