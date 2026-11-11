# From the MT6768 to the MT6897: what carries over, what does not

The reference MediaTek port is [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) (Infinix Hot 20, Helio G85). This
page is what changes on the POCO X6 Pro 5G (duchamp, Dimensity 8300 Ultra) and in what order to bring it up, following the
project's [Adapting HakiOS to your device](https://hackios-web.vercel.app/en/guide/your-device) guide.

**State: nothing here has run on a phone yet.** The loader builds, the tools are tested against the shipped kernel image and
device tree, and `tools/hakios-device.py check duchamp` passes. Every address marked *[phone]* in `mtkloader/platform.h` must
be checked against `tools/duchamp-probe.sh` before the first flash.

## Where the numbers come from

- `dtb/mt6897.dtb` and `dtbo.img` of [mt6897-devs/device_xiaomi_duchamp-kernel](https://github.com/mt6897-devs/device_xiaomi_duchamp-kernel),
  merged with `fdtoverlay` (the dtbo has one entry). Read for register addresses only.
- `BoardConfig.mk` of [mt6897-devs/device_xiaomi_duchamp](https://github.com/mt6897-devs/device_xiaomi_duchamp): boot image
  header v4, `Image.lz4`, 64 MB boot partition, kernel base `0x3fff8000`, tags (DTB) offset `0x07c88000`, GKI.
- The module lists there: which drivers this phone actually loads (touch, panel, Wi-Fi, GPU).

What the shipped tree cannot say, LK fills in at every boot: DRAM size, ramoops, the framebuffer, most reserved memory. The
loader now reads the framebuffer (`chosen/atag,videolfb`) and ramoops (`compatible = "ramoops"`) from LK's own tree at run time
(`mtkloader/mtkfdt.h`) instead of hard-coding them as the MT6768 loader did.

## Address map, side by side

| Block | MT6768 (X6826B) | MT6897 (duchamp) | Node in the duchamp tree |
| --- | --- | --- | --- |
| GIC distributor | `0x0c000000` | `0x0c400000` | `interrupt-controller` (`arm,gic-v3`) reg 0 |
| GIC redistributors | `0x0c040000` | `0x0c440000` (8 x `0x40000`) | same, reg 1 |
| Console UART | `0x11002000` | `0x11001000` | `soc/serial@11001000` |
| Watchdog (TOPRGU) | `0x10007000` | `0x1c00a000` | `soc/watchdog@1c00a000` (`mediatek,mt6589-wdt`: same registers) |
| Keypad | `0x10010000` | `0x1c00e000` | `soc/kp@1c00e000` (volume up only) |
| PMIC bus | PWRAP `0x1000d000` | SPMI/PMIF `0x1cc04000` | `soc/spmi@1cc04000` (MT6363, MT6368) |
| Storage | eMMC, MSDC0 `0x11230000` | **UFS**, UFSHCI `0x112b0000`, PHY `0x112a0000` | `soc/ufshci@112b0000` |
| Touch bus | SPI2 `0x11012000` | SPI2 `0x11012000` | `spi2` / `xiaomi_touch@0` |
| GPU | Mali-G52 MC2 `0x13040000` | **Mali-G615 MC6** `0x13000000`, GPUEB `0x13c00000` | `soc/mali@13000000` (`arm,mali-valhall`) |
| Display mutex | `0x14001000` | `0x14001000` | `soc/disp-mutex0@14001000` |
| OVL0 | `0x1400b000` | `0x14402000` (in ovlsys) | `soc/disp-ovl0@14402000` |
| DSI0 | | `0x1400d000` | `soc/dsi@1400d000` |
| Sensor hub (SCP) | | `0x1c400000` | `scp@1c400000` |
| CPU | 6x A55 + 2x A75 | 4x A510 (MPIDR `0x000`-`0x300`) + 4x A715 (`0x400`-`0x700`) | `cpus` |

## What the loader changed (mtkloader/)

- All of the above moved to `platform.h`; `mtkloader.c` and `mtkdbg.h` use it.
- Core order `0, 7, 6, 5, 4, 1, 2, 3`: with `cpus=4` iOS gets the core LK left us on plus the three fastest A715s; the
  watcher takes the next one. `small-core` is set on cores 0-3.
- Framebuffer and ramoops from LK's tree (`mtkfdt.h`, tested on a synthetic tree laid out like the X6826B's: it gives back
  that phone's known console and pmsg addresses). Without them the loader prints nothing to the screen or to pstore but still
  runs.
- The OVL is not touched by default (`PLAT_OVL_TOUCH 0`): a read of a MediaTek block whose clock is off can hang the bus, and
  which OVL scans LK's buffer on this SoC is not known yet.
- Left out, because they are the MT6768's: the TD4160 touch firmware upload, the eMMC (MSDC) reader and the system image read
  from it, the Mali-G52 / camera / CONNSYS probes, the A75 PLL step. The payload's RAM disk is the root (`rd=md0`).
- `start.S` moves the image to its link address if LK put it elsewhere: a header v4 boot image carries no load address, and
  Android's GKI Image says `text_offset 0`.
- Android is restarted at `PLAT_ANDROID_PA` (2 MB aligned, as a `text_offset 0` Image needs), no longer at the loader's own
  address.

## The boot image (tools/chainboot.py)

duchamp's `boot_a` is header v4: header page, `Image.lz4` (LZ4 legacy frames, 8 MB blocks), an empty ramdisk (the ramdisks are
in `vendor_boot` and `init_boot`, which are not touched) and the GKI boot signature. The tool unpacks the kernel, puts mtkloader
in front at `CHAIN_OFFSET` and the XNU payload behind, packs it again the same way and refuses an image over 64 MB or one whose
unpacked size would reach LK's device tree. Tested with the shipped `Image.lz4` (35 MB unpacked) and a 20 MB payload: 16.9 MB
image, 54 MB once unpacked.

On an unlocked Xiaomi, a boot image that no longer matches its AVB hash boots in orange state, as the X6826B does. Magisk on
this phone patches `init_boot`, not `boot`, so a rooted phone's `boot_a` is the stock one.

## Order of work, after the guide

0. **Before anything**: unlocked bootloader, Magisk root, `tools/backup-boot.sh` (keeps `boot_a`, `vendor_boot_a`,
   `init_boot_a`, `dtbo_a`, `vbmeta_a`). Ask in the project's Telegram channel whether someone already works on duchamp.
1. **Read the phone**: `tools/duchamp-probe.sh`, then set the *[phone]* values of `platform.h` from `logs/probe/summary.txt`:
   `PLAT_KERNEL_PA` / `PLAT_ANDROID_PA` from the "Kernel code" line, `PLAT_XNU_PHYS_BASE` / `PLAT_XNU_MEM_SIZE` from free
   System RAM clear of every reserved region, `PLAT_MARKER_PA` and `PLAT_PANIC_LOG_BASE` inside a range nothing else writes.
   Check that `/dev/pmsg0` exists (the "boot XNU once" request travels in it) and that `atag,videolfb` is there.
2. **Chain only**: build with no payload (`mtkloader/build.sh`, `mtkloader/build_flashed.py`), flash `boot_a`, and see
   Android come up through the loader. Then `tools/boot-xnu.sh` with no payload: the loader should print its banner, LK's
   framebuffer and ramoops, say there is no payload and go back to Android, with its lines in
   `/sys/fs/pstore/console-ramoops-0`. This proves the load address, the relocation, the chain and pstore.
3. **Boot with nothing of the phone's**: `make duchamp_defconfig && make kernel` (on a Mac), the RAM disk payload,
   `tools/xnu-cycle.sh`. Done when the log reaches launchd and the phone comes back to Android by itself. If it stops early in
   the kernel: GIC, UART and timer addresses (`TIMER_PROBE=1` exists for the last).
4. **Buttons** (keypad at `0x1c00e000` for volume up; power and volume down need the PMIC over SPMI), **touch** (FocalTech
   FT3683G or Goodix on SPI2: which one is this phone's is in `logs/probe/dmesg.txt`; neither is the TD4160 the reference
   drives, though the common tree has a Goodix driver and a FocalTech firmware header).
5. **UFS, read only**: the common tree's `hakios.ufshc.c` is a plain UFSHCI (JESD223) driver that only swaps in its own
   transfer list and never resets the link; pointed at `0x112b0000` it should apply, and the loader can read through LK's
   controller state the same way. This replaces the MT6768's eMMC path (`ufs_root=1` in place of `emmc_root=1`).
6. **GPU**: the Mali-G615 is Valhall with a CSF and is powered by GPUEB, not by the SPM as the G52 was; Mesa's driver for it
   is panthor, not panfrost, and it needs `mali_csffw.bin` (`pull-firmware.sh` fetches it). This is the largest single piece
   of new work.
7. **Wi-Fi / Bluetooth / sensors**: CONNAC3 gen4m (`wlan_drv_gen4m_6897`), `bt_drv_6897`, the SCP sensor hub. The reference's
   step-1 report of CONNSYS nodes in LK's tree (`mtkconnsys.h`) is kept and runs at every boot.

## Safety, as for the reference

Flash only `boot_a`; never slot b, never any other partition. Always pass the serial. Keep the backup. Short runs, one at a
time, no voltage changes. If the phone does not come back, hold power and volume down for fastboot and flash
`backup/boot_a.img`.

## Checked against another MT6897 port (Lenovo TB375FC)

[billyham07/UniXNU-tb375fc](https://github.com/billyham07/UniXNU-tb375fc) (read on 11 November, commit `cc4da0e`) runs XNU on
the same SoC, by another route: a UEFI port (Mu-Silicium) and the UniXNU framework with iPadOS 27 (xnu-12377), after an
earlier HakiOS 16.5 route with a stage0 loader in front of LK. Its numbers come from that tablet's live FDT and from runs on
it, so they are a second witness for this page.

| | TB375FC | here | |
| --- | --- | --- | --- |
| Cores, MPIDR | A510 `0x000`-`0x300`, A715 `0x400`-`0x700`, all eight up through PSCI (SMC) | same | agrees |
| GIC | GICD `0x0c400000`/`0x40000`, GICR `0x0c440000`/`0x200000` | same | agrees |
| Timer | 13 MHz, PPIs 13/14/11/10 | read from `CNTFRQ_EL0` at run time | agrees |
| Watchdog | TOPRGU `0x1c00a000`, RESTART key `0x1971` | same | agrees |
| UFS | UFSHCI `0x112b0000`; LK leaves the link at PWM-G1 and single 4 KiB READ(10)s work without touching it; data on LUN 2 | `0x112b0000`, UFS step not written yet | agrees, and the plan of step 5 is what worked there |
| XNU memory | `0x130000000`-`0x23bc00000`, the first System RAM run with no reserved holes | was `0xc0000000` + `0xfe000000` (the MT6768's) | **corrected**: now `0x130000000` + 4 GiB |
| Watchdog mode | stage0 arms TOPRGU keeping LK's mode bits; the kernel only ever writes RESTART | wrote the key alone (clears every mode bit) | **corrected**: only the enable bit changes |

Also worth knowing from there, though it was seen on xnu-12377 and may not apply to 16.5: `use_contiguous_hint=0` was needed
(an `arm_vm_init.c` hint assertion in `machine_lockdown`), and with no RTC driver the kernel waits 30 s for IORTC, so a run
must not let the watchdog fire during it.

## What the phone said (probe of 11 November)

Isaac's duchamp: 2311DRK48G, HyperOS OS3.0.6.0.WNLMIXM (Android 16), slot a, orange state (unlocked), Magisk.

- **DRAM** 8 GB, `0x40000000` + `0x200000000`. LK's reserved memory fills everything below `0x130000000` (the TEE alone is
  `0xc0000000` + `0xe000000`, where the MT6768 loader put XNU; `0x80000000`, the MT6768's run marker, is `mm_ccu_reserved`).
  System RAM `0x12fe70000`-`0x23dffffff` is free in one run: XNU `0x130000000` + `0xffe00000`, the marker and panic log in the
  2 MB above it. The watcher's identity map now reaches the top of DRAM (it stopped at 8 GiB of address before).
- **Android's Image** at `0x40000000` (Kernel code `0x40010000`), LK's tree at `0x47c80000`, its ramdisk at `0x66f00000`:
  the BoardConfig's offsets hold.
- **ramoops**: no `ramoops` node; LK passes `ramoops.mem_address=0x48090000 mem_size=0xe0000 console_size=0x40000
  pmsg_size=0x10000` in `bootargs` (record and ftrace 4096, Linux's defaults). `mtkfdt.h` now reads them there: console
  `0x4811f000`, pmsg `0x48160000` -- checked by running it on this phone's own tree. `/dev/pmsg0` exists.
- **Framebuffer**: `atag,videolfb` base `0xfcc2f000` size `0x33d0000`, 60 fps, panel `n11a_42_02_0a_dsc_vdo` (1220 x 2712).
- **CPU**: parts `0xd46` (A510) x4 at 2.2 GHz, `0xd4d` (A715) x4, cpu7 at 3.35 GHz: the core order holds.
- **Touch**: Goodix (`GTP` in dmesg) through `xiaomi_touch`.
- `boot_a` 64 MB, `init_boot_a` 8 MB.
