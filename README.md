# hakios-device-duchamp

English · [Español](README.es.md)

HakiOS for the **POCO X6 Pro 5G (duchamp)**, also sold as the Redmi K70E: MediaTek Dimensity 8300 Ultra (**MT6897**),
GPU **Mali-G615 MC6**, UFS. A device repository after the project's guide
([Adapting HakiOS to your device](https://hackios-web.vercel.app/en/guide/your-device)), started from the first MediaTek
port, [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768).

**State: bring-up not started on hardware.** Nothing here has run on a phone yet. What is done: the MT6768 loader carried
over to this SoC's addresses and building, a boot image tool for this phone's header v4 (GKI) image, the probes that read
off the phone what its shipped device tree cannot say, and the device layer that `tools/hakios-device.py check duchamp`
accepts. [docs/porting-mt6897.md](docs/porting-mt6897.md) has the address map, what differs from the MT6768 and the order of
work.

## What works

| | | |
| --- | --- | --- |
| boot | not run | mtkloader with MT6897 addresses (GIC-600, UART, TOPRGU), relocates itself, chains Android's GKI Image |
| display | not run | LK's framebuffer from `chosen/atag,videolfb`; 1220x2712 DSC video panel |
| CPU | not run | 8 cores offered A715-first (`cpus=4`: three big cores) |
| storage | missing | UFS (UFSHCI `0x112b0000`); the payload's RAM disk is the root |
| touch, buttons, GPU, sensors, Wi-Fi, Bluetooth, audio, battery, camera, haptics, cellular | missing | |

## What is here

- `mtkloader/` -- the bare-metal loader (MIT), hakios-mt6768's: in `boot_a` in front of Android's kernel, starts XNU once when
  Android asks, Android comes back with XNU's log in pstore. `platform.h` holds every address of this phone and says where
  each came from; `mtkfdt.h` reads LK's framebuffer and ramoops from LK's own device tree.
- `tools/` -- `chainboot.py` (header v4 boot image), `duchamp-probe.sh` (what the phone says), `backup-boot.sh`,
  `boot-xnu.sh`, `xnu-cycle.sh` (one run), and the reference's log readers.
- `configs/`, `soc/` -- the kernel's configuration for this phone and the mt6897 SoC layer, for the common tree.
- `device.conf` -- the phone and the state of each component.
- `pull-firmware.sh` -- the phone's firmware out of **your** phone into `fw-*/` (root), for the later steps.

## Not here, by design

No file of iOS, no firmware of the phone, no device tree or partition of the phone, no screenshot (LEGAL.md).

## Running (once the [phone] values of platform.h are checked)

What it needs: a POCO X6 Pro with an unlocked bootloader and Magisk; a Mac for the kernel and the font; the common tree
([hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)) with this repository in `devices/duchamp`.

```sh
export HAKIOS_SERIAL=...                 # adb devices
tools/backup-boot.sh                     # backup/boot_a.img and the rest: how the phone is put back
tools/duchamp-probe.sh                   # logs/probe/summary.txt -> mtkloader/platform.h
HAKIOS_ROOT=../.. mtkloader/build.sh     # mtkloader.bin
python3 mtkloader/build_flashed.py       # xnu-flashed-slot_a.img (no payload: the chain alone, step 2)
XNU_SECS=120 XNU_ARGS="cpus=4" tools/xnu-cycle.sh    # with build-payload.bin: one XNU run, the log in logs/
```

## Licenses

`kernel/` (none yet) is APSL 2.0 (`LICENSE.APSL`); everything else is MIT (`LICENSE`). See `NOTICE` and `LEGAL.md`.
