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

## Run 1 (11 November): the chain

`xnu-flashed-slot_a.img` without a payload, flashed to `boot_a` (the whole 64 MB, vbmeta moved, footer rewritten). Android
came up through the loader; asked for XNU once through `/dev/pmsg0`, the loader said, in `console-ramoops-0` after the next
Android boot:

    boot: Darwin / XNU (requested)
    == mtkloader: Darwin/XNU on POCO X6 Pro 5G (duchamp, MT6897) ==
    LK dtb 0x47c80000
    LK framebuffer 0xfcc2f000 + 0x33d0000, 6000 fps, panel n11a_42_02_0a_dsc_vdo_lcm_drv
    ramoops 0x48090000 + 0xe0000: console 0x4811f000 + 0x40000, pmsg 0x48160000 + 0x10000
    ...
    ERROR: no XNU payload in this image
    starting Android

So: LK starts the loader (relocated or not), the loader reads LK's tree (x0 = `0x47c80000`), finds the framebuffer and
ramoops there, takes the request from pmsg, writes to pstore, and gives Android back its own Image at `0x40000000`. Step 2 of
the guide is done. (`atag,videolfb`'s fps is in hundredths: 6000 = 60 Hz. The CONNSYS report stops at its 48-node cap before
the consys node itself; to be raised for the Wi-Fi step.)

## Step 3, prepared (11 November): the kernel and what of it is still the MT6768's

The kernel is the common tree's branch `mtk` (the one that took the X6826B to its home screen), built on a Mac by
`mac/hakios-kernel.sh` (`xnu/apply.sh`, `xnu-deps/setup.sh`, `./xnu-build.sh -k`, as that branch's `docs/from-a-clone.md`).
`tools/mkimage.sh` packs it: `XNU_ARGS="cpus=1"`, back after 45 s, the watcher on core 7 at 285 s, and as md0 the branch's
`prebuilt/userland/hello`, one program of raw system calls that the kernel's mockfs presents as `/sbin/launchd`. No storage
is needed for that; done when the log shows the kernel's version line, its page counts and hello's own words.

Read through that kernel for addresses of the MT6768 that would be touched without a boot-arg:

- Every driver of `redwood_input_init()` is off without its own boot-arg (`touchscreen=`, `buttons=`, `mali=`, `mtkbatt=`,
  `connsys_step=`, `scp_step=`, `mtkcam=`, `cpufreq=`, ...), and the frame flip of `MiniIOMobileFramebuffer` (OVL0 at the
  MT6768's `0x1400b000`) is off without `fb_flip=1`. None is given.
- The GIC comes from the tree the loader builds (`/arm-io/gic` `reg`); the MT6768's addresses are only the fallback.
- The one that is not: `GenericARMPE.cpp`'s early console, `mt6577_uart_putc`, writes the MT6768's console UART at a
  constant `0x11002000`, and `loader_log_putc` calls it for every character. On the MT6897 that is uart1 behind a clock gate
  nobody opened. The loader now patches that function to `ret` (by symbol, like the PL011 ones): the phone has no serial
  port, and the log reaches pstore anyway.
- The redistributors: the kernel walks them `0x20000` apart and stops at `GICR_TYPER.Last`. Read beforehand as GICv4.1
  frames `0x40000` apart; run 3 showed them `0x20000` apart, as on the MT6768, so the walk is simply right.
- What LK does with a kernel larger than Android's own is still unknown: the step-3 image unpacks to `0x3200000` bytes
  against Android's own Image of `0x21caa00` (the check in `tools/chainboot.py` is LK's tree at `0x47c80000`). Option 1 of
  `mac/hakios-paso3.sh` waits for Android before XNU is ever asked for, so a refusal shows there.

## Run 2 (11 November): XNU to user space

The image of step 3 (kernel of branch `mtk` a962798 built on Isaac's Mac, `hello` as md0, `cpus=1`), flashed and run from
Windows (`windows/hakios-paso3.cmd`). LK took the larger image (`0x3200000` unpacked) and Android came up through it. Asked
for XNU once:

    ICC_CTLR_EL1 0x88402 (EOImode set: clearing) -> 0x88400          <- the same firmware habit as the MT6768's
    watcher on cpu7: PSCI CPU_ON 0, running
    [15.385388]: gic: v3, distributor 0xfffffff140770000, cpu 0's redistributor 0xfffffff1407b0000, virtual timer on PPI 27
    [15.385438]: Darwin Kernel Version 22.5.0: Wed Nov 11 03:09:12 -05 2026; ike:xnu-8796.121.2 ...
    [15.408439]: vm_page_bootstrap: 256631 free pages, 5385 wired pages
    [15.481621]: MiniIOMobileFramebuffer: display 1125x2436 on a panel of 1220x2712 (4992 bytes per row)
    [15.581897]: BSD root: md0, major 1, minor 0
    [15.585091]: load_init_program: attempting to load /sbin/launchd
    Hello from user space on Darwin!
    panic(cpu 0 caller ...): initproc failed to start -- no exit reason available -- (signal 0, exit status 0 )
    === XNU returned to mtkloader: reason 0 (end) code 0x0
    starting Android

The panic is the expected end of `hello`, which exits: process 1 may not. So on the MT6897 with nothing of the phone's: the
loader's tree, the GIC-600 (distributor and redistributor from `/arm-io/gic`), the virtual timer at 13 MHz and its interrupts
(threads were scheduled), the VM over 4 GiB at `0x130000000`, IOKit, the framebuffer console at LK's pitch, mockfs on md0,
`exec` and system calls. From the kernel's first line to `/sbin/launchd`: 200 ms. Step 3 of the guide is done; the screen
showed the loader's white text, nothing of XNU's (no `-v`), the loader's yellow text after it, then Android.

Next (`docs` of the reference, step 7): `cpus=4` (cores 0, 7, 6, 5; the watcher on core 4), which also tests the
redistributor walk above core 0. Then the root: a storage driver for UFS (step 5 of the order above).

## Run 3 (11 November): four cores

The same with `cpus=4` (cores 0, 7, 6, 5 offered; the watcher on core 4):

    psci: SMP initialized via SMC (CPU_ON = 0xc4000003)
    gic: v3, distributor 0xfffffff118170000, cpu 1's redistributor 0xfffffff118290000, virtual timer on PPI 27
    arm_cpu_init(): cpu 1 online      ... cpu 2, cpu 3 the same
    GenericARMPE: 4 of 4 processors are running
    cpu 0 (phys id 0x0) is a slower core: the scheduler uses it after cpu 3 (phys id 0x500)
    panic(cpu 2 caller ...): initproc failed to start          <- hello ran on an A715, and exited as before
    psci: cpu 2 ends the system: the return to the boot loader is the boot processor's to make

Redistributors of cores 0x700, 0x600, 0x500 at GICR + `0xe0000`, `0xc0000`, `0xa0000`: `0x20000` apart (platform.h
corrected). Every core took timer interrupts (each idles in the same place in the panic's CORE lines).

Next: the storage. `-DUFS_PROBE=1` (asked for like XNU) prints the UFS host controller as LK leaves it, read only:
whether it runs the doorbell list or MCQ, where LK's list is, its last command. The kernel's UFS path on branch `mtk`
(`redwood_ufshc.c`) swaps a list of its own into a controller that firmware brought up and never resets it; whether that
applies here depends on those answers.

## Run 4 (11 November): the UFS controller as LK leaves it

`-DUFS_PROBE=1`, read only:

    CAP 0x55c7051f      32 transfer slots, 8 task slots, 64-bit addressing, MCQ supported
    MCQCAP 0x80b0207    8 queues, their registers at 0x1000
    VER 0x400           UFSHCI 4.0
    HCE 0x0  HCS 0x0    the host controller is not enabled; no device present, no list ready
    UTRLBA 0x0, CONFIG 0x0 (no MCQ), AHIT 0x0, IS 0x0
    MTK_IP_VER 0x1440000, MTK_REFCLK_CTRL 0x3, MTK_XOUFS_CTRL 0x102

LK disables the host before it starts the kernel: Android's ufs-mediatek brings it up from nothing. So the branch `mtk`
route (a list of our own swapped into a controller that firmware left running) does not apply, and the read path is a
whole UFS bring-up: HCE, link startup (UIC DME_LINKSTARTUP), MediaTek's reference clock and M-PHY setup, the device's
NOP OUT / fDeviceInit, then READ(10) on the LUN that holds the system image. To be done in the loader first, read only,
one stage per run, each stage printing what it is about to do before it does it.

## Run 5 (11 November): host enabled, no link

`-DUFS_PROBE=2`: HCE to 1 and three DME_LINKSTARTUP, nothing else.

    HCE <- 1            host enabled; HCS 0x8 (UIC ready), IS 0x0
    DME_LINKSTARTUP x3  result 0x1 (FAILURE) each time, HCS 0x8 (no device present), IS 0x4 (UIC error)

The host answers UIC commands, so UniPro's side of it runs; the link does not start. What Android does before its first
DME_LINKSTARTUP, read off the phone's own vendor modules (`ufs-mediatek-mod.ko`, `phy-mtk-ufs.ko`,
`clk-mt6897-peri.ko`) and its device tree:

- clocks: ufscfg_ao `0x112b8000` gates UniPro's TX symbol, RX symbol 0 and 1, and system clocks (bits 0-3),
  ufscfg_pdn `0x112bb000` the UFSHCI and its AES engine (bits 0-1); set/status/clear at `+4`/`+8`/`+0xc`
- device reset: SiP call `0xc2000276` (MTK_SIP_UFS_CONTROL), `x1 = 2`, `x2 = 0` then `1`, the host off between,
  10-15 ms after
- host reset: ufscfg_pdn set `0x48` / clear `0x4c`, bit 0 UniPro, 1 crypto, 2 HCI (the tree's `ti,reset-bits`, flags
  `0x1c` = ASSERT_SET | DEASSERT_SET | STATUS_NONE); XOUFS_CTRL bit 7; HCE
- before the link: DME_SET `0xd0a8` (VS_UniproPowerDownControl) 0, `0x155e` (PA_Local_TX_LCC_Enable) 0,
  `0xd0a6` (VS_SavePowerControl) bit 6 cleared
- the M-PHY: nothing. The tree says `mphy-ver = 1` and the vendor PHY driver skips its whole power-on when that is set.
- link state for diagnosis: debug select `0x22c0` = `0x820820` and `0x22d0`-`0x22dc` (IP 4.4), state in bits 31:28 of
  `0x22c8` (0 disabled, 1 down, 2 up, 3 hibern8, 4 lost)

`-DUFS_PROBE=3` does exactly that (stage A2) and, if the link comes up, stage B in the same run: NOP OUT, fDeviceInit,
then on LUNs 0-7 READ CAPACITY(10) and READ(10) of block 1, and the partition names of the first GPT it finds. Read
only; the watchdog is armed meanwhile (31 s) with the marker at DONE, so a hang resets the board and its log is kept.

## Run 6 (11 November): the link, the device, the GPT

`-DUFS_PROBE=3`, the sequence above, read only:

    link        up at the first DME_LINKSTARTUP: 2 lanes each way, PWM-G1 (the device can do HS-G5), UniPro 2.0
    NOP OUT     answered (NOP IN)
    fDeviceInit set, cleared by the device at once
    LUN 0, 1    4 MB each: the boot logical units (preloader)
    LUN 2       256 GB, blocks of 4 KB, the GPT: 90 partitions (boot_a at block 260096, 16384 blocks; super at 503808;
                userdata at 2945024, 61628408 blocks)
    LUN 3-7     not there

The first command after the reset is answered with response 1 and a check condition (unit attention, the reset itself);
what counts is the SCSI status in the response UPIU, not its response byte.

## Where the system image goes

`lpdump` of 11 November: super is 9663676416 bytes, virtual A/B, every `_b` partition empty, the `_a` ones up to sector
16626840 of 18874368, so about 1.1 GB is free at its end. A system image of about 2.4 GB does not fit there. userdata
has metadata encryption, so a file put in it cannot be read raw from the kernel either (checked 11 November with no
screen lock set: `ro.crypto.state` encrypted, `ro.crypto.metadata.enabled` true, and the raw partition's byte 1024 on
is noise, not f2fs's magic -- the lock only guards the keys, which stay in the TEE). What is left is a partition
(`xnusys`) cut from the end of userdata, which wipes the phone: not done yet, by the owner's choice. The kernel's UFS
driver is first tested on what is there already.

## The kernel's UFS driver, and its test

`xnu/files/bsd/dev/redwood_mtkufs.c`: a block device after the tree's eMMC one (`redwood_emmc.c`), read only, one
request at a time through slot 0 and a 128 KB bounce buffer. It does not bring the UFS up: mtkloader does (as in run 6),
stops its request lists, leaves the host enabled with the link up, and says where the partition is with
`ufs_lun= ufs_lba= ufs_blocks=` on the command line. The kernel takes the controller over with its own lists.

- `ufs_root=1` (with `XNU_ROOT_UFS=1` in the loader, partition `xnusys`): `/dev/ufs0` is the root, as long as the APFS
  containers at the start of the partition
- `ufs_test=1` (`-DXNU_UFS_TEST=1`, partition `boot_a`): md0 stays the root; the loader prints a sum of the
  partition's first 4 MB before the kernel starts, the kernel reads the same 4 MB through its driver and prints its own
  (FNV-1a over 64-bit little-endian words), then reads up to 16 MB for the speed. The two sums must be the same.

The driver is not in the common tree: `xnu/duchamp-overlay.py` copies `xnu/files/` into it and hooks it in where the eMMC
driver is (`bsd/conf/files`, `bsd/kern/bsd_init.c`, `iokit/bsddev/IOKitBSDInit.cpp`), once. `mac/hakios-kernel.sh`
carries both at its end (`tools/embed-overlay.py` puts them there after any change under `xnu/`) and applies them before
the build.

## Run 7 (11 November): the kernel reads the UFS

`-DXNU_UFS_TEST=1` with the kernel built with the driver:

    loader   boot_a, block 260096, 16384 blocks; starts "ANDROID!"; sum of its first 4096 KB 0x9f9c4591ed4acbf7 in 6025 ms
    kernel   controller taken over (lists at 0x135500000), LUN 2 ready; starts "ANDROID!";
             4096 KB in 5120 ms, sum 9f9c4591ed4acbf7; 16 MB in 20466 ms, 160 reads, 0 errors

The same bytes on both sides, and XNU went on to md0 and user space as before. About 0.8 MB/s: the link is still in
PWM-G1, its slowest gear, which no root file system can live with. Next: the power mode change to HS, as Android makes it.

## The HS gear (prepared 11 November)

What Android does after fDeviceInit, read off `ufs-mediatek-mod.ko` (`ufs_mtk_pwr_change_notify`,
`ufs_mtk_adjust_sync_length`) and `ufshcd_xiaomi-core.ko` (`ufshcd_change_power_mode`), and what mtkloader's
`ufs_hs_gear()` now does after `ufs_utp_start()` when XNU uses the UFS:

1. PA_TxHsG1..G5SyncLength (`0x1552 0x1554 0x1556 0x15d0 0x15d6`) at least `0x48`, ours and the device's (peer)
2. the tree says `mediatek,ufs-pmc-via-fastauto`: HS-G1 rate B in FASTAUTO first (`PA_PWRMode` `0x44`), TX adapt none
3. the gear in FAST mode (`0x11`): gears, active lanes, terminations, PA_HSSeries 2 (rate B), PA_TxHsAdaptType 1
   (INITIAL) from G4 on, PA_PWRModeUserData0-5 and DME_Local{FC0,TC0,AFC0} timeouts at 8191/65535/32767
4. done when IS.UPMS comes and HCS.UPMCRS is 1 (PWR_LOCAL)

Android asks for G5 (host and device both say G5) and drops to G4 when it scales its clocks down; mtkloader asks for G4
(`UFS_HS_GEAR`, 0 = stay in PWM-G1) and leaves ufs_sel as LK left it. If the change or a read after it fails, the whole
bring-up runs again and the reads happen in PWM-G1, as in run 7.

## Run 8 (11 November): HS-G4

The same kernel, the loader with `ufs_hs_gear()`:

    sync lengths      0x4a/0x4b 0x4c/0x4b 0x4b/0x4b 0x4d/0x4d 0x4e/0x4e (all at 0x48 or more already)
    HS-G1 B FASTAUTO  status 1 (done)
    HS-G4 B FAST      status 1 (done); PA_PWRMode 0x11, gears 4/4, rate B; no UIC errors; link up
    loader            4096 KB summed in 774 ms (its own reads, MMU off, 24 KB at a time)
    kernel            4096 KB in 49 ms, the same sum; 16 MB in 184 ms (about 87 MB/s), 160 reads, 0 errors

A hundred times run 7, with the kernel still reading 128 KB at a time through one slot and a bounce buffer: more slots,
longer transfers and reading straight into the caller's pages are where the rest is. The storage path is no longer what
stops a system image from being the root; where the image goes is (above: a partition cut from userdata).

## Eight cores (prepared 11 November)

The cores: cpu@0..3 Cortex-A510 (MPIDR 0x000-0x300), cpu@100..102 Cortex-A715 (0x400-0x600), cpu@200 the fastest A715
(0x700). LK leaves the loader on core 0, an A510, so that is the core XNU boots on. The kernel's types are the tree's
own: the loader marks the A510s `small-core`, and the kernel (osfmk/arm64/machine_routines.c,
`ml_choose_slow_core_partners`) makes each the slower sibling of an A715 of its own, so the scheduler puts a thread on
an A510 only when no A715 is free; GenericARMPE starts them 28 s after the others (`small_cores_after=`, 0 = at once).
Real E/P clusters (`cluster-type`) are not for this kernel: its configuration (BCM2837) allows one cluster.

`XNU_CORES=8` needs the watcher off (it takes a core of its own): `XNU_WATCH=0 XNU_ARGS="cpus=8 small_cores_after=0"
tools/mkimage.sh`. Without the watcher, a kernel that hangs waits for the power key. With `cpus=7` the watcher keeps
core 3, an A510, and the kernel has all four A715s.

Next, for speed rather than count: the boot processor is the slowest core (an A510 at 1.3 GHz, where XNU's own
start-up runs and where interrupts go), and nothing sets the cores' frequencies yet.

## Run 9 (11 November): eight cores, and the direct reads

`cpus=8 small_cores_after=0`, no watcher, the kernel with the direct-read driver (bfa56f5):

    GenericARMPE: 8 of 8 processors are running
    slower cores paired: 0x0 with 0x400, 0x100 with 0x500, 0x200 with 0x600, 0x300 with 0x700
    mtkufs: 4096 KB bounced in 49 ms, sum 7fa0e140d6794121 (the loader's)
    mtkufs: direct: 4096 KB in 5 ms, the same sum; 64 MB in 37 ms (1803 MB/s); 0 errors

XNU went on to md0 and user space as before. From 0.8 MB/s in run 7 to about 1.8 GB/s: PWM-G1 to HS-G4, then the
bounce buffer out of the way. (The 64 MB are read into the same 1 MB again and again; their bytes were checked only for
the first 4 MB.)

## The boot core on the fastest A715 (prepared 11 November)

`XNU_BOOT=7 tools/mkimage.sh` (`-DXNU_BOOT_CORE=7`): before writing the device tree the loader starts core 7 with PSCI
CPU_ON (start.S `boot_core_entry`, its own stack at `_start - 0x28000`). That core reports its exception level and its
clock, and waits with its MMU off; the tree then lists it as cpu0, `state = running`, and the kernel picks its boot
processor by that property (`ml_is_boot_cpu`). At the jump core 0 hands over the kernel's entry and boot-args address,
waits until core 7 has taken them, and then either becomes the watcher (when the watcher's turn in the order is core 0,
i.e. `cpus=7`) or switches itself off with PSCI CPU_OFF, so that the kernel can start it as a secondary (`cpus=8`).
If core 7 does not answer within a second, XNU starts on core 0 as before. The order is now 7, 6, 5, 4, 1, 2, 3, 0.

What follows from it: everything the kernel does on its boot processor (its whole start, and every interrupt, since
`gic_v3_spi_attach()` routes an SPI to the processor that attaches the driver) runs on the A715. The way back needs
nothing new: `loader_return` is made by the boot processor after the others are off, and `xnu_returned()` ends in a
reset, which brings Android up on core 0 as always. (The watcher's PC sampling still looks at core 0's PMU.)

First run: `XNU_BOOT=7 XNU_ARGS="cpus=7 small_cores_after=0" XNU_SECS=90 tools/mkimage.sh -DXNU_UFS_TEST=1`, so the
watcher (core 0 itself) brings a log back even if the new start hangs; the UFS test's bounced read (a copy loop) is the
comparison with run 9.

## CPU frequencies: what there is to set (11 November)

Android does not set a CPU clock itself on this SoC. `mediatek-cpufreq-hw.ko` (vendor_ramdisk) writes an OPP index
into one of three performance domains in the MCUPM's shared SRAM, and the MCUPM firmware sets PLL and voltage. From
the DT (`performance-controller@11bc00`, the cpus' `performance-domains`) and the module's `cpufreq_mtk_offsets`
(0x0, 0x84, 0x88, 0x8c, 0x90, 0x114):

| Domain | Base | Cores |
|---|---|---|
| 0 | 0x11bc10 | the four A510s |
| 1 | 0x11bd30 | A715 cores 4-6 |
| 2 | 0x11be50 | A715 core 7 |

Per domain: +0x00 the table (32 words, MHz in bits 11:0, highest first, as the module's probe reads it), +0x84
"enable" (`mtk_cpufreq_hw_cpu_init` writes 1), +0x88 the index asked for (`mtk_cpufreq_hw_target_index` writes it),
+0x8c status (cpu_init waits up to 300 ms for bits 0, cpufreq hardware, and 1, SVS), +0x114 the transition latency. The module also has a "fast DVFS" path, used when the word at the cpuhvfs node's third
region (ESRAM, 0x118000) is 0xabcd0001.

`CPUFREQ_SHOW` (on by default) prints all of it before the jump, reading only. Setting a frequency is then one write per
domain, but only if the enable bit says the MCUPM is listening; and nothing in XNU would bring the clock down again when
the phone gets hot, so a fixed maximum is for short test runs only.

## Run 10 (11 November): XNU on core 7

`nucleo-rapido.zip`: `XNU_BOOT=7`, `cpus=7`, the watcher on core 0 itself, the UFS test.

    boot core cpu7: PSCI CPU_ON 0, waiting at EL1, running at 1196 MHz
    cores for XNU: 7 6 5 4 1 2 3
    GenericARMCPU: cpu 0 (phys id 0x700) registered ... GenericARMPE: 7 of 7 processors are running
    gic: v3, ... cpu 0's redistributor 0xfffffff130490000
    mtkufs: test: 4096 KB in 9 ms (run 9: 49 ms), sums equal; bounced: 16 MB in 29 ms (run 9: 181 ms); direct 1800 MB/s

The copy loop runs about six times faster on the A715; the direct reads stay where the device holds them. LK leaves core 7
at 1196 MHz, below core 0's 1299 MHz: the frequency tables (`CPUFREQ_SHOW`) are the next run's. XNU reached md0 and user
space as before, and came back by itself.

Next image, `ocho-rapido.zip`: `XNU_BOOT=7 XNU_ARGS="cpus=8 small_cores_after=0" XNU_WATCH=0`, the UFS test, `CPUFREQ_SHOW`.

## Run 11 (11 November): eight cores from core 7, and the frequency tables

`ocho-rapido.zip`: `XNU_BOOT=7`, `cpus=8`, no watcher.

    cpufreq domain 0 (A510 x4):    enable 0x101  status 3  asked 9  (1300 MHz)  table 2200 ... 480, 18 entries
    cpufreq domain 1 (A715 x3):    enable 0x0    status 3  asked 20 (1300 MHz)  table 3200 ... 400, 30 entries
    cpufreq domain 2 (A715 core 7): enable 0x0   status 3  asked 22 (1200 MHz)  table 3350 ... 400, 31 entries
    cpufreq fdvfs signature (0x118000) 0x0
    psci: cpu 7 (MPIDR 0x0) was started 5 seconds ago and has not joined: no sign of it since the firmware took the request
    GenericARMPE: 7 of 8 processors are running

(The run's loader printed the status word as "in force 3": +0x8c is not an index.) The tops are the SoC's 3.35 / 3.2 /
2.2 GHz; LK leaves all three near the bottom, and the counted clocks agree with the indices asked (core 0 1299 MHz,
core 7 1196). The firmware is up in every domain (status 3) and no fast-DVFS path (signature 0).

Core 0 switched itself off after the hand-over (XNU's CPU_ON was accepted, so the firmware had it off), but it never
reached the kernel's reset vector. Not understood yet; `core0_off` now records what CPU_OFF answered and `xnu_returned()`
prints it. Until then: `cpus=7`, core 0 the watcher.

Next: `frecuencia-maxima.zip`, `-DCPUFREQ_SET=1` (enable 1 and index 0 in each domain whose status is 3, then the tables
again; the boot core counts its clock after the change), `cpus=7`, the watcher, the UFS test.


## Run 12 (11 November): the top frequencies

`frecuencia-maxima.zip`: `-DCPUFREQ_SET=1`, `XNU_BOOT=7`, `cpus=7`, the watcher on core 0.

    cpufreq domain 0 (A510 x4):     enable 0x1 status 0x3 asked 0 = 2200 MHz latency 1
    cpufreq domain 1 (A715 x3):     enable 0x1 status 0x3 asked 0 = 3200 MHz latency 1
    cpufreq domain 2 (A715 core 7): enable 0x1 status 0x3 asked 0 = 3350 MHz latency 1
    boot core cpu7: ... running at 3328 MHz            (run 10: 1196)
    this core is running at 2183 MHz                    (core 0; run 10: 1299)
    pmap_startup() init/release time: 2989 microsec    (run 10: 7330)
    mtkufs: direct: 4096 KB in 2 ms; 64 MB in 33 ms (1995 MB/s); bounced: 16 MB in 24 ms; 0 errors

The firmware takes the request as Android's module makes it, and the clocks counted agree with the tables. XNU ran its
90 s and came back as before. The bounced read barely moved (29 to 24 ms): it is bound by memory and the device now,
not by the core. Nothing lowers the clock again: `CPUFREQ_SET` stays a switch for test runs until the kernel has a
governor and a thermal limit.


## The long read: 12 GB (prepared 11 November)

The root will be read by the kernel's driver for as long as the system runs, in reads of every size; so far it had read
at most 64 MB in one go. `-DXNU_UFS_TEST=2` makes the loader name `super` instead of `boot_a` (9 GB, Android's system:
the one big partition whose contents do not change while Android runs, and are not encrypted) and pass `ufs_test=2`.
After the usual test the kernel (`test_big` in bsd/dev/redwood_mtkufs.c) reads 12 GB (`ufs_big_mb=` changes it):

1. all of `super` from its start, 1 MB per READ(10) straight into kernel pages: its SHA-256 (libkern's, minicrypto)
   and a sum of every 64 KB. Android's `sha256sum` of the same partition has to give the same digest.
2. the remaining 3 GB as re-reads of random places, 64 KB to 1 MB each, in four shapes: one READ(10) into a buffer
   that starts on any cache line of a page; pieces of 4 KB to 256 KB; pieces of any length (part blocks, the bounce
   buffer); all of it to an odd address. Every 64 KB has to match the first pass (a stale cache line, a wrong PRDT
   entry or a bounce copy off by some bytes shows up as a different sum, and the first eight are printed).

Its image: `XNU_SECS=150`, so the minute or so it takes fits before the kernel hands the log back. On Android, as root:
`sha256sum /dev/block/by-name/super` (the whole partition, 9216 MB).

Android, 11 November 12:08: `sha256sum /dev/block/by-name/super` = `e313cd34fdc3ef45e1008dbcdd16556487d37f023d8b364eb63ac091ace24c43`.


## Run 13 (11 November): 12 GB read back

`lectura-12gb.zip`: Isaac's kernel with the driver d89dd2c4, `-DXNU_UFS_TEST=2`, `-DCPUFREQ_SET=1`, `XNU_BOOT=7`, `cpus=7`,
`XNU_SECS=150`.

    partition super: block 503808, 2359296 blocks (9216 MB)
    mtkufs: test: 4096 KB in 7 ms, sum 77cca9397cf5d424 (the loader's: the same); direct 64 MB in 33 ms (2005 MB/s)
    mtkufs: big: 9216 MB read in 5013 ms (1927 MB/s), summed in 42131 ms
    mtkufs: big: SHA-256 of the first 9216 MB (9663676416 bytes): e313cd34fdc3ef45e1008dbcdd16556487d37f023d8b364eb63ac091ace24c43
    mtkufs: big: re-reads: 3072 MB in 7001 ms, 5716 reads (1420 one read, 1414 4 KB pieces, 1458 odd pieces,
                 1424 odd address), 0 of 49157 x 64 KB not the same
    mtkufs: big: PASSED: 12288 MB read in all, 0 errors

The digest is Android's. Every shape of read the root's file system can ask for came back as the first pass had it.
The reading is a ninth of the time; the rest is libkern's SHA-256 in C on one core (219 MB/s). XNU then went on to
user space and its 150 s as before (the hello program's exit is the usual `initproc failed to start` panic).


## Core 0 kept for the kernel (prepared 11 November)

Run 11: core 0 switched itself off (PSCI CPU_OFF) after the hand-over, the kernel's CPU_ON for it was answered 0, and
it never reached the reset vector's first stage. Rather than guess at what the firmware does with the core LK booted on,
it no longer goes off. With `cpus=8` (`XNU_CORE0_PARK`, on when `XNU_CORES` is 8) core 0 waits in the loader, MMU and
caches off, on a mailbox of 64 bytes (`core_park`: "PARK", its MPIDR, the entry, -, its state) whose physical address
goes on the command line as `cpu_park=`. The kernel's `psci_smp_start_cpu()`, patched by xnu/duchamp-overlay.py, looks
there before it calls CPU_ON: a matching core waiting gets the reset vector written into the mailbox and an SEV, and
takes it (start.S `park_jump`: EL1h, DAIF masked, both timers off, SCTLR M/C/I off, I-cache and TLB invalidated, x0 = 0)
as CPU_ON would have brought it. Its data side is the one runs 1-9 started the whole kernel from on this same core. The
kernel says `psci: MPIDR 0x0 was kept by the boot loader (cpu_park): sent to the reset vector`, the loader on the way
back `core 0, kept for the kernel (cpu_park): ...`. At the end the kernel switches it off with CPU_OFF like the rest:
to the firmware it was never off.


## xnusys (prepared 11 November)

The owner chose the carve (userdata stays encrypted without a screen lock, see above). `windows/hakios-xnusys.ps1`:

1. Option 1 reads only: LUN 2's disk node from `/dev/block/by-name/userdata` (its logical block must be 4096), the first
   and last 64 blocks of the LUN (both GPTs), userdata's start and length as Android's kernel has them, and copies the
   calibration partitions (proinfo, nvram, nvcfg, nvdata, persist, protect1/2) to `backup\calibracion` on the PC, which
   stay there. `backup\gpt-duchamp.zip` (the two regions and info.txt) comes back to the thread.
2. `tools/gpt-xnusys.py` checks both headers, their CRCs, that the arrays are the same and that userdata is where the
   kernel says, then shortens userdata by 8 GiB (1 MiB aligned) and puts `xnusys` (Linux data type, a new GUID) into the
   first empty entry. Nothing else in either table changes. It writes `lun2-start.new` (blocks 1 .. the array's end),
   `lun2-end.new` (the backup array .. the LUN's last block) and `plan.txt` (SHA-256 of both, the sizes).
3. Option 2 checks the files against plan.txt, the phone's tables against what option 1 read, asks for BORRAR, writes the
   backup table then the primary (dd, conv=notrunc, sync, flushbufs), reads both back, reboots to the bootloader and
   only if `getvar partition-size:xnusys` is the planned size erases `metadata` and `userdata` (Android makes them again
   on its first boot, as after Xiaomi's own flash_all).

Then: system.apfs, built by the owner with the HakiOS tools, into xnusys; and `XNU_ROOT_UFS=1`.

What option 1 found on duchamp (17:14): LUN 2 is 64577536 blocks of 4 KB, the disk GUID is all zeros, and the array has
exactly 90 entries for 90 partitions (3 blocks at LBA 2, room for 96; blocks 5..7 and the backup's spare blocks are
zeros). `userdata` (entry 88) is 2945024..64573431 and `flashinfo` (entry 89, 16 MB) sits after it, at the LUN's end.
With no empty entry the tool now grows the array by one inside its own blocks: the headers say 91, `xnusys` is entry
90, the array's place and the first/last usable blocks stay. The plan: userdata 2945024..62476031 (227.1 GB), xnusys
62476032..64573431 (2097400 blocks, 8 GiB + 992 KB, up to flashinfo). Checked with util-linux `fdisk -b 4096` on a
sparse image of the LUN: "No errors detected", 91 of 91, every other partition the same; the byte diff is the two
header CRCs, NumberOfPartitionEntries, the two array CRCs, userdata's last LBA and entry 90. The one thing no test here
can answer is whether MediaTek's preloader and LK read the entry count from the header (they must: 90 is this phone's
own count, not a constant); option 2's `getvar` check is the first place that shows it, before anything is erased.

Done 11 November, 17:25: both tables written and read back, and the phone came up in fastboot, so the preloader and LK
take the 91-entry table. Option 2 then stopped by design: `getvar partition-size:xnusys` was empty. `getvar all` shows
why: Xiaomi's LK lists only boot, init_boot, vendor_boot, dtbo, vbmeta*, lk, super and userdata, so no other partition
is visible to fastboot either. It reports userdata as 0x38c5f00000, the new size exactly (a single `getvar` right
after the reboot had said 0x36c5f00000, 8 GiB less, and did not repeat). `erase metadata` and `erase userdata` were run
by hand. xnusys is never written with fastboot: from Android (adb, root) or found by the loader, which reads the entry
count from the header (`ufs_find_part`).

Android, after its first boot on the new table: `blockdev --getsize64 /dev/block/by-name/xnusys` = 8590950400. Stage 2
of the plan is done. The system image goes in with `windows/hakios-sistema.cmd` (the common tree's
`tools/system-flash.sh` for Windows: the image through /data/local/tmp, dd into xnusys, xnusys read back and its SHA-256
compared with the PC's file; it refuses unless the target's PARTNAME is xnusys, it is not userdata, and the image starts
with NXSB and fits). The image itself is the owner's build (`userland/build.sh` of the common tree: `system.apfs` and the
small `root.apfs`, which becomes md1 with `XNU_ROOT_UFS=1`). `mac/hakios-construir-sistema.sh` runs that build on the
owner's Mac in the tree `hakios-kernel.sh` left (no pull, so the kernel and the user space stay of one commit): it
stops if the iOS files step of the guide (the owner's, never done by these scripts) is not there or is older than the
tree's selection, builds with `SHARE=1` (no Wi-Fi passphrase in root.apfs), checks both images with the tree's own
`tools/apfs/apfs.py` (NXSB, `/sbin/launchd`, no `/fw/network`, system.apfs within 8590950400 bytes) and leaves
system.apfs for the PC and `hakios-root-duchamp.zip` (root.apfs, both SHA-256) for the thread. Tried here on a mock
tree with stand-ins for the macOS commands: the good path and five refusals.


## Run 14 (11 November): eight of eight from core 7

`ocho-nucleos.zip`: Isaac's kernel with driver/overlay 41f7e37d (cpu_park), `cpus=8`, `XNU_BOOT=7`, no watcher, the UFS
test, `CPUFREQ_SET=1`.

    boot-args: ... cpus=8 ... cpu_park=1074327744
    cpu_start() cpu: 7
    gic: v3, ... cpu 7's redistributor 0xfffffff1483b0000, virtual timer on PPI 27
    arm_cpu_init(): cpu 7 online
    psci: MPIDR 0x0 was kept by the boot loader (cpu_park): sent to the reset vector
    GenericARMPE: 8 of 8 processors are running
    core 0, kept for the kernel (cpu_park): the kernel sent it to its reset vector       (the loader, afterwards)

Core 0 came in through the reset vector from the loader's mailbox, joined the scheduler as the slow partner of core 7,
and went off with the rest at the end: XNU returned (reason 0) and Android started. UFS test as before (1993 MB/s, 0
errors). All eight cores, the kernel on the fastest: stage 1 of the roadmap is done.


## Run 15 (prepared 11 November): the root on xnusys

The owner's build on his Mac (`mac/hakios-construir-sistema.sh`, tree a962798, the kernel's own commit): `system.apfs`
2476732416 bytes (sha256 439b8aa9...), `root.apfs` 4 MB (sha256 964d0468..., read here with `tools/apfs/apfs.py`: one
volume `redwood`, 80 files, no `/fw/network`). system.apfs goes into xnusys with `windows/hakios-sistema.cmd`.
`raiz-xnusys.zip`: kernel 69d439e6, root.apfs as the payload's RAM disk (md1), `-DXNU_ROOT_UFS=1` (the loader finds
xnusys and passes `rd=md0 ufs_root=1 ufs_lun=2 ufs_lba= ufs_blocks=`), `cpus=8 small_cores_after=0`, `XNU_BOOT=7`, no
watcher, 180 s, no `CPUFREQ_SET` (three minutes is too long at full clocks with no thermal control in XNU). What to look
for: `mtkufs: the system image at block 62476032 of LUN 2: 2361 MB in its APFS containers` (if all of it is one container), then the root mounted from
`ufs0` and `load_init_program: attempting to load /sbin/launchd`.

Run 15a (11 November, 19:48), before system.apfs was written: the loader found `partition xnusys: block 62476032,
2097400 blocks (8192 MB)`, HS-G4 rate B, 8 of 8 processors, md1 = root.apfs (4 MB), `mtkufs: controller taken over`,
then `no APFS container at block 62476032 of LUN 2 (no NXSB)` and the planned panic in IOFindBSDRoot; XNU came back
to the loader and Android started. Everything up to the root's mount works on the new partition.

Run 15 (11 November, about 19:50), system.apfs written: the screen stayed on the kernel's Apple logo and the owner
held the power key; pstore then had no loader log. A long press is the PMIC's cold reset, which does not keep DRAM, so
with no watcher (cpus=8 needs `XNU_WATCH=0`) a hang leaves nothing. Run 16, `raiz-xnusys-vigilante.zip`: the same with
`cpus=7`, the watcher on core 0 at 240 s (snapshot of the panic string, the message buffer and every core, then PSCI
SYSTEM_RESET, which keeps DRAM), and `-DXNU_VERBOSE=1` (`-v`, the kernel's text console on the screen instead of the
logo).

Run 16 (11 November, 20:04), `raiz-xnusys-vigilante.zip`: XNU returned by itself, with a panic far before the root:
`Unhandled implementation specific error` on the boot core right after `minicrypto: registered`, backtrace
`sched_init_thread -> thread_block_reason -> ml_set_interrupts_enabled -> fleh_serror -> arm64_platform_error`: an
SError (asynchronous external abort) that was pending and was taken the first time the boot thread unmasked it. Run 15a
(same kernel, `cpus=8`, no `-v`) went past this point, and runs 10-12 had `cpus=7` with the watcher and no fault; `-v`
is the one thing no run before had, so it is the suspect. Run 17, `raiz-xnusys-7.zip`: run 16 without `-v` (7 cores,
watcher at 240 s, the root on xnusys).

Run 17 (11 November, 20:20), `raiz-xnusys-7.zip`, no `-v`: **iOS from xnusys.** `mtkufs: the system image at block
62476032 of LUN 2: 2361 MB in its APFS containers`, 4 MB in 3 ms, `BSD root: ufs0`, APFS volume "System" (45044 files),
`/sbin/launchd` (stage0) -> `/redwood/sbin/init`: `=== 36 checks, 0 failed ===`, then iOS's own launchd
(`launchd.ios`, "Early boot complete. Continuing system boot."), SpringBoard, backboardd, containermanagerd, cfprefsd,
logd, lsd and our test jobs. backboardd put up its boot UI (`[BootUI] Current bootUI is an Apple Logo`) and
MiniIOMobileFramebuffer showed it (`swap #1`, a 1125x2436 surface scaled onto the 1220x2712 panel): the Apple logo on
the phone is iOS's. About 10 s after the kernel started, in the second swap, everything stopped: the message buffer ends
at 24.696 s, the kernel's heartbeat (a thread call every 10 s) stayed at 2, the `loader_return_after` thread call never
ran, and the watcher took its snapshot at 240 s and reset the board (the log came back through pstore). Run 15 (8 cores)
stopped on the same logo. PC sampling in the snapshot reads 0 (the MT6897's external debug blocks are not where the
loader looks), so where each core was is not known. Run 18, `raiz-xnusys-1nucleo.zip`: the same with `cpus=1`, to tell
a multi-core hang (an IPI or a TLB shootdown that a core never answers) from one in the display path.


Run 18 (11 November, 20:53), `raiz-xnusys-1nucleo.zip`, `cpus=1`: the same stop at the same place. The message buffer
ends at 26.453 s with backboardd's second swap (selectors 6, 4, 5: surface 5, 1125x2436 BGRA), `swap 2 (#1)` is never
logged, the heartbeat stayed at 2 and the watcher's screen shows the Apple logo. One core is enough for it: not an IPI
or a shootdown left unanswered. What is between that SwapEnd and the line `swap 2` in MiniIOMobileFramebuffer's
`show()`: `MiniIOSurfaceDrawn()` (no GPU is up here: `redwood_gpu_ready()` is the stub's false and Mali needs
`mali=1`, so at most 500 ms of `delay(200)`), the wait for the panel's beat (the first `clock_delay_until()` the
presenter makes: the first swap sets the beat and does not wait), and `fLock`. None of them can stop a kernel on its
own; with `cpu_idle_latency` 0 every delay blocks on a timer, so a timer that never fires would stop all of them and
the heartbeat's thread call with them. One thread call group shares one delayed timer, armed on the processor that
armed it, which is also why with 7 cores a single stuck processor stops both the heartbeat and `loader_return_after`.

The watcher could not say which: its PC sampling and its GIC lines are the MT6768's core 0 (`0x0e010000`, GICR frame 0),
and XNU runs on core 7; and it printed only the first 256 KB of the kernel's 282 KB log, so the end of it was never
seen. Run 19, `raiz-xnusys-19.zip` (image 0e623267...), is run 18 again with a new watcher (same kernel, `cpus=1`, 240 s):

- every XNU processor from its `cpu_data` (through `CpuDataEntries` and XNU's page tables): the thread on it, its timer
  interrupts counted over one second, the next timer pop against the counter, the decrementer, pending AST and IPI, the
  interrupted PC if it is in an interrupt, and the frame chain found in the running thread's stack;
- every thread from the global `threads` list: tid, state, the processor it is on, process and thread name, wait event,
  continuation or the frames from its saved context;
- the GIC's redistributor of each core (pending and active bits, PPI 27 being the virtual timer);
- the CoreSight ROM table found in that window, for the PC sampling of the right core later;
- the last 32 KB of the message buffer and the end of the kernel's log (its first 4 KB and as much of the end as the
  256 KB ramoops console has room for).

The offsets come from this kernel's disassembly (listed in `mtkloader.c` above `watch_xnu_cpus`) and are checked against
nothing else: another kernel needs them read again.

Run 19 (11 November, 21:29), `raiz-xnusys-19.zip`, `cpus=1`: the same stop, and this time where. **XNU's processor
stopped receiving its timer interrupt at 27.33 s.** At 254 s cpu0 had counted 140897 timer interrupts and none in the
last second; its next timer pop was 27329 ms, 227 s overdue. In core 7's redistributor (GICR frame 7, affinity 0x700)
the virtual timer, PPI 27, is enabled and pending (ISENABLER0 0x0c00dcff, ISPENDR0 0x08000000) and nothing is active
(ISACTIVER0 0): the timer fired, the GIC holds it and never hands it to the core. The processor runs its idle thread,
which spins (`cpu_signal` still has `SIGPdisabled`, its value until the first IPI, so `cpu_idle` returns without WFI) and
opens the interrupt window every round: a pending IRQ would be taken. Every thread that waits waits on a time: the
presenter (`frames to the panel`) in `show+0x1f0`, the first beat wait of the second swap, whose deadline is that
27329 ms pop; lsd in `delay_for_interval` under `read_cmd < read_bytes < ufs_strategy`, holding `ufs_lock`; and every
other thread that reads the disk after it, in `lck_mtx_lock_contended` under `ufs_strategy`. That is the whole system
stopping, and it explains 7 cores as well as one. The message buffer goes on to 27.403 s (threads run without a timer
until they block), after the second swap's selector 5 at 27.327612. The first swap's copy into the panel ran from 27.3136
to 27.3257 s.

Runs 17 and 18 had read `GICD_CTLR 0x10` in their snapshots: affinity routing (ARE_NS) on and Group 1 (EnableGrp1NS, bit
1) off. XNU's `gic_v3_irq_cpu_init` sets bit 1 if it is clear, and nothing in XNU writes GICD_CTLR again; interrupts
worked for 27 s. So something cleared it -- the secure world or another master, or a stray write from XNU into the
distributor's page (a zero written there reads back as 0x10: ARE_NS cannot be cleared once set). Run 19's watcher did
not read the distributor.

Run 20, `raiz-xnusys-20.zip` (image ed4b15d1...), is run 19 with the watcher also keeping an eye on the GIC while XNU
runs (the snapshot at 240 s is as before, with GICD_CTLR, IIDR, and each core's PWRR and IGROUPR0 added):

- every 0.25 ms it reads GICD_CTLR and the boot core's GICR_CTLR, WAKER, PWRR, IGROUPR0, IGRPMODR0, ISENABLER0 and
  PPI 27's priority, and logs each change with the counter's time (XNU's log uses the same) and the thread XNU's
  processor 0 runs; when Group 1 goes off, also which of the distributor's settings changed since it was last seen on;
- it follows cpu0's timer interrupts and next pop (through XNU's page tables, once `cpu_ttep` is set); when the pop is
  100 ms overdue it logs the GIC (both, plus the SPIs pending or active), the kernel's `gic_active_iar` and the end of
  the message buffer;
- if Group 1 is off then, it turns it back on (as `gic_v3_irq_cpu_init` does) and says how many timer interrupts came in
  the second after; after that first time, as soon as it sees it off again. If the timer comes back, iOS boots on past
  the logo, and at 180 s XNU returns to the loader with its whole log; the watcher's lines are repeated after that log,
  which wraps the 256 KB ramoops console;
- nothing goes to ramoops before XNU has its page tables (the loader may still be writing there), and the watcher writes
  no variable the boot core also writes: XNU's return stub stores with the MMU off, and a dirty line of this core's
  cache could be written back over it.

The page-table walker now refuses tables and pages outside XNU's memory (`0x130000000 + 0xffe00000`), since it reads
tables that XNU is still changing.

Run 20 (11 November, 21:52), `raiz-xnusys-20.zip`: **the cause.** GICD_CTLR read 0x12 when the watcher started (LK
leaves Group 1 on). At 26.328 s it went to 0x10, and the distributor's register at 0x34 (implementation defined) from 0
to 0x60000000; the watcher turned Group 1 back on at 26.433 s, 100 ms after the timer was due, and XNU's timer came
straight back. It went off twice more, at 26.445 and 26.464 s, and was turned on again at once each time. The three
times are the ends of the copies of swaps 1, 2 and 3 into the panel (26.3159-26.3280, 26.4337-26.445, 26.4524-26.464):
each copy writes the panel's bottom row last. The SPIs read active in the pattern 0xff000000 in words 4 to 30 (an opaque
black pixel), and not in words 1 to 3. With Group 1 kept on, iOS went on: usermanagerd, containermanagerd, the test jobs,
SpringBoard (whose main thread then waited 20 s on an XPC reply, a matter of its own). At 49.786 s backboardd blanked
the display (`Set Blanked: YES`, backlight factor 0), the presenter cleared the panel with `bzero` (swap 4, no surface)
and the kernel panicked: `Unaligned kernel data abort` in `bzero`, FAR 0xfffffff131570000, ESR 0x96000061 (a write,
alignment fault). That address is the GIC's distributor: `gic: v3, distributor 0xfffffff131570000`.

XNU's `io_map()` (osfmk/arm/io_map.c), before its VM is up, maps `round_page(size + offset)` and moves
`virtual_space_start` on by `round_page(size)` only. `initialize_screen` maps the framebuffer that early: LK's is at
0xfcc2f000, 0x3000 into a 16 KB page, 1220 x 2712 rows of 4992 bytes = 0xce9400, so 0xcec000 is reserved and 0xcf0000
mapped, and the next early mapping, the distributor, gets the framebuffer's last page. The panel's last 0x390 bytes (the
right end of its bottom row) are GICD 0x000-0x38f: GICD_CTLR (Group 1 off), 0x34, ISENABLER/ICENABLER and
ISPENDR/ICPENDR (each set, then cleared), ISACTIVER words 1-30 set and ICACTIVER words 1-3 cleared again -- which is the
pattern read. `bzero` uses `DC ZVA`, which on device memory is an alignment fault. On the MT6768 the framebuffer was
on a page boundary, and Apple's always are.

Fixes: in the kernel, `virtual_space_start += alloc_size` (xnu/duchamp-overlay.py, embedded again in
mac/hakios-kernel.sh); until a kernel with it is built, the loader tells XNU a framebuffer of 1220 x 2715, the first
height for which both roundings agree (`xnu_fb_height()`): three rows below the panel, inside LK's framebuffer
(0x33d0000 bytes), never shown. Run 21, `raiz-xnusys-21.zip` (image 7acff270...): run 20 with that height; the watcher
stays, and should log no change of GICD_CTLR at all.

Run 21 (11 November, 22:14), `raiz-xnusys-21.zip`, `cpus=1`: **iOS's lock screen on the POCO X6 Pro.** XNU was told
1220 x 2715; GICD_CTLR stayed 0x12 the whole run (the watcher never had to turn Group 1 on), and XNU came back by itself
at 180 s with no panic and its whole log (1.4 MB). backboardd's boot UI (swaps 1-3, 26.2-26.8 s), SpringBoard's main
thread waiting 20 s on an XPC reply (38 s, again; it went on afterwards), the display blanked at 49.6 s and lit again at
56.1 s, then SpringBoard's wallpaper, status bar, icons and the lock screen's widgets (Calendar, Weather, World Clock,
Batteries, Screen Time...): from 57 s MiniIOMobileFramebuffer showed 10 frames a second, about 20.6 ms each in the
driver on the one core, a picture of mean colour 130/163/159 and 99-100% not black. At 120 s iOS blanked the display
(auto-lock: no touch yet) and the system went idle until the return. 60 processes; 3.5 GB available. Stage 5 of the
roadmap (the home screen, by CPU) is reached as far as the lock screen; the home screen needs touch.

Run 22, `raiz-xnusys-22.zip` (image 5bae9aa2...): run 21 with `cpus=7` (the watcher on the eighth core), as run 17 was
before the fix: whether 7 cores now go as far, and the frame copy shared out between 3 of them (`fb_threads`, default 3).

Run 22 (11 November, 22:27), `raiz-xnusys-22.zip`, `cpus=7`: the same as run 21 on 7 cores. 7 processors running,
GICD_CTLR 0x12 throughout, no panic, back by itself at 180 s. The lock screen came up at 50.6 s (57 s on one core);
copying a frame into the panel took 7.2-9.3 ms (20.6 ms on one core) with `fb_threads` 3, and the frame rate stayed at
10 a second (one burst of 18): what limits it is not the copy but how often SpringBoard and backboardd hand over a new
frame. SpringBoard's main thread still waits 20 s on an XPC reply at 36.9 s; MailWidgetExtension and
PassbookWidgetsExtension crashed (corpses), as widget extensions do here. Auto-lock blanked the display at 111.9 s.

## Touch: both chips (prepared 11 November)

duchamp comes with one of two touch controllers, and Android's tree describes both on one node (dtbo, `xiaomi_touch@0`
under `spi2@11012000`): a Goodix GT9916 ("Berlin-D", compatible `xiaomi,touch-spi`, vendor driver `goodix_9916`) or a
FocalTech FT3683G (`focaltech,n11a-3683g-spi`, `focaltech_3683g`). Isaac's phone has the Goodix (`GTP` in its Android
log: touch report at 0x10308, panel 1220 x 2712, coordinates up to 19520 x 43392). The vendor drivers are public
(github.com/mt6897-devs/android_kernel_device_modules_6.1, `drivers/input/touchscreen/`), and so are the MT6897 pin
table, the MT6368 regulators and the PMIF driver beside them; everything below is read from them. Both chips keep
their firmware in their own flash (the FT3683G's type, 0x56720090, has no "IDC" bit): nothing is downloaded, and no
firmware of anyone's is in this repository.

| | |
|---|---|
| I/O supply | MT6368 (SPMI slave 5) LDO VTP, 1.8 V: enable 0x1c87 bit 0, voltage index 0x1da5[3:0] = 4; through PMIF's software channel 2 (0x1cc04000 + 0x880..0x8a8), extended-register commands |
| analog supply | GPIO73 high (`goodix,avdd-gpio`, read by the FocalTech driver too) |
| reset, interrupt | GPIO60 (low = reset), GPIO9 (low = a report waits) |
| SPI | GPIO11 clock, 12 CS, 13 MISO, 14 MOSI (function 1 = SPI2), mode 0 for both chips; driven by hand here |
| Goodix | read `F1` + 32-bit address + `FF FF FF`; write `F0` + address; version 0x10014, description 0x10070 |
| FocalTech | register, `A0`, 16-bit length, 3 dummy bytes, data + CRC-16/Kermit; ID 0xA3/0x9F = 56 72; report at 0x01, 84 bytes |

The vendor drivers choose between them by two panel ID lines named by Linux's global GPIO numbers (385, 391), whose
base is only known at run time; here both chips are asked, FocalTech first (to a Goodix chip that is a frame not
starting with F0/F1, which it ignores; a Goodix read would reach a FocalTech chip as a write to its register 0xF1).

Step 1, `TOUCH_PROBE=1` (mtkloader/mtktouch.h): asked for like XNU (paso3, option 2), the loader powers the chip in
the vendor drivers' order (reset held; VTP, only if slave 5 says 0x68 and VTP is off; 10 ms; AVDD; 15 ms; reset let
go; 300 ms), asks both chips, then for 40 s draws a dot where each finger is and logs each touch, gives the SPI pins
back their functions and starts Android. `tacto-1.zip` (image 4f185edf...). Tested on a computer against simulated
chips of both kinds.

Step 2, prepared: `XNU_TOUCH=1` runs the same bring-up on the way to XNU and puts `mtktouch=1|2 mtktouch_at=
mtktouch_x= mtktouch_y=` on the kernel's command line; `bsd/dev/redwood_mtktouch.c` (in the overlay, so in
mac/hakios-kernel.sh) reads the chip on the same pins every 8 ms and posts to `/dev/redwood-touch`, which backboardd's
libredwood (the common tree's userland) turns into HID events. `mtktouch_flip=1|2|3` mirrors the axes if step 1 says
they need it.

Touch, step 1 (11 November, 23:40), `tacto-1.zip`: **the Goodix chip answers and reports.** LK leaves the SPI pins at
function 1, reset and AVDD low, VTP off (enable 0x00, voltage index already 4); slave 5 said 0x68 and VTP came on
(0x01). The FocalTech question got zeros (the Goodix chip ignored it, as meant); the Goodix version block: chip
"BERLIN", firmware "9916R", 05 80 02 e6, its sum right; the description put the report at 0x10308 (as Android's log),
commands at 0x10174, panel 1220 x 2712, coordinates up to 19520 x 43392. In 40 s: 5000 reads, 3699 reports, 3681
finger positions, 6 touches, 0 bad; x ran 31-19515 and y 156-40732, so the dots reached every edge. While a finger
was down nearly every 8 ms look found a new report (240 a second from the chip), but the interrupt line was low in
only 614 looks, 333 of them in the first five seconds with no finger: it is no guide, and the kernel's driver now
reads the report every look instead of waiting for the line (one 34-byte transfer when there is nothing).

Run 23 (12 November, 00:17 UTC; Isaac's kernel of 18:50 with the driver, `ios-tacto-23.zip`, image eb47756f...,
cpus=7, 300 s, watcher): **iOS takes touches: unlocked, home screen, an app opened and closed.** (A first try at 00:03
flashed run 22's image by mistake: every zip holds `xnu-flashed-slot_a.img` and the new one had gone into a
subfolder; the log's `epoch=` and the missing `mtktouch=` gave it away, and hakios-paso3.ps1 now prints the image's
date and the first 8 hex digits of its SHA-256 before flashing.) The loader found the Goodix chip as in step 1 and
passed `mtktouch=1 mtktouch_at=66312 mtktouch_x=19520 mtktouch_y=43392`; the kernel's driver started ("read every
8000 us") and `/dev/redwood-touch` was there for backboardd. Lock screen at ~50 s; touches from 56 s: swipes up
from the bottom (y 0.83, then from the very edge, y 0.99 and 1.00) and SpringBoard drew the home screen. A tap at
(0.90, 0.79) at 61.8 s started Preferences (Settings, pid 80) 0.1 s later; after a swipe home, a second tap on the
same spot (0.87, 0.78) at 140.6 s started it again (pid 83), so taps land on the icon under them (Settings sits in
the fourth column of the fifth row). Inside Settings (frames turn white, 241/241/244) came more taps and drags
(backboardd drew 16x88 scroll indicators), one at the top left (0.16, 0.08), where a back button sits; backboardd's own count: 12 down, 11 up, touches
11-1B to Preferences and 1C to SpringBoard, 2 from the edge stolen by the system gesture (the swipe home), which
put Preferences' scene from foreground to inactive; it exited at 18:54:28. In all: 30 touches, 1302 reports in
22500 reads, 0 bad, 0 reset requests; no flip needed. Frames 10-33 a second (the copy 5-8 ms). Auto-lock blanked
the screen at 18:55:30 (backboardd asked the digitizer for wake-on-touch, mode 0xfe, which the driver doesn't do
yet), and XNU came back at 300 s with no panic. Stage 5 of the roadmap (the home screen) is reached.

GPU probe 1 (12 November; `gpu-sonda-1.zip`, image aaf662c1, `-DGPU_PROBE=1`, no XNU): run 24 showed the CPU clock is not
what limits the frames (cores 1.8-3.1 GHz, 63% of busy frames within 18 ms against 53-62% at 1.3 GHz), so the GPU is next.
Android drives the Mali-G615 in EB mode (`gpueb-support = 1`): the GPUEB at `0x13c00000` switches rails, PLLs and power
domains. `mtkloader/mtkgpu.h` reads, and writes nothing: VGPU (MT6368 VBUCK2) and VSRAM (MT6363 VSRAM_MDFE), the SPM's
MFG0 status, and only with MFG0 on the GPUEB's PC/LR/SP and footprints and the MFG power controller; the Mali's own
registers only with MFG1 on and its PLL running.

GPU probe 1 result (12 November, 01:02 UTC): MFG0 on and the GPUEB idle in its wait loop (PC 0x14716 five times, IRQs
enabled, gpufreq footprint 0xafaf0025); MFG1-14 off (MFG1_PWR_CON 0x3312: isolated, clock off, SRAM down), the GPU's EMI
bus protection held, VGPU and VSRAM off. GPU probe 2 (`gpu-sonda-2.zip`, image 6d94daeb, `-DGPU_PROBE=2`, approved by the
owner): CMD_INIT_SHARED_MEM with LK's gpueb_mem_addr, then CMD_POWER_CONTROL on, the Mali's registers, and off again,
over the GPUEB mailbox as Android's gpufreq does in EB mode; the GPUEB picks the voltages.

GPU probe 2 result (12 November, 01:14 UTC): the GPUEB answers. CMD_INIT_SHARED_MEM in 3.5 ms (LK's gpueb memory
0x7fe30000 + 0x180000; the status page then starts 0xbabadada), CMD_POWER_CONTROL on in 3.1 ms with power count 1: both
rails on, the EMI bus protection released, MFG1 on (0xc000220d), MFGPLL running. The Mali's own registers: GPU_ID
0xb8a31030 = architecture 11.8, product major 3 = **Mali-G615** (Linux panthor_hw.c names it so), r1p3; CSF_ID 0x4100412;
SHADER_PRESENT 0x130013 (six cores, MC6), one L2, AS_PRESENT 0xff, MCU_STATUS 0 (no firmware yet). Off again: count 0,
MFG1 back to 0x3312, Android booted. Next: the CSF firmware (mali_csffw.bin, windows/hakios-firmware-gpu.cmd) and the MCU.

GPU probe 3 (12 November; `gpu-sonda-3.zip`, image e3a9a763, `-DGPU_PROBE=3`, payload with one entry, `gpufw` = the
owner's own `mali_csffw.bin`, never committed): as probe 2, and while the GPU is on, the CSF boot of Linux's panthor. The
firmware (CSF header 0.3, 8 IFACE sections, the protected-mode one skipped) is copied to free DRAM at `0x180100000`, a
4-level LPAE page table at `0x180000000` (9 pages) maps each section at its MCU address, non-cacheable (memattr 0x4c, the
GPU is not coherent, the loader's stores are uncached); soft reset, COHERENCY_PROTOCOL = NONE, L2 on, AS0 = that table
(AARCH64_4K, 48-bit VA), MCU_CONTROL = AUTO, wait up to 1 s for the global interface interrupt, then the global control
block at `0x4000000` (version, groups, input and output), one GLB_PING, a halt, MCU disabled, soft reset (AS0 forgotten)
and the GPUEB powers it off. Build: `mkpayload.py -o build-payload.bin gpufw=fw-gpu/mali_csffw.bin`, then
`mtkloader/build.sh -DXNU_WDT_SECONDS=0 -DGPU_PROBE=3`.

GPU probe 3 result (12 November, 01:36 UTC): **the MCU boots**. Reset, L2 ready, AS0 set (TRANSCFG 0x10001c6), the global
interface interrupt 2.3 ms after MCU_CONTROL = AUTO, MCU_STATUS 1. Global control block: CSF interface 2.5.0, features
0x10, input at 0x4002000 and output at 0x4006000, 8 command stream groups (stride 0xa0), perfcnt size 0x36, instr features
0x71. GLB_PING acknowledged after 5.8 ms; halt (MCU_STATUS 2), disable, reset and the GPUEB's power off all fine, Android
booted. To look into: MMU_INT_RAWSTAT 0x10000 (AS0's bus-error bit) seen at the halt, with AS0_FAULTSTATUS 0.

GPU probe 4 (12 November; `gpu-sonda-4.zip`, image 782a6fe7, `-DGPU_PROBE=4`, same payload): after the ping, panthor's global
setup (core_en_mask = shader_present, timers, ALLOC_EN/POWEROFF/PROGRESS acknowledged), AS1 given the MCU's table, then
command stream group 0 with one queue (ring buffer, input and output pages and suspend buffer at MCU/GPU VA 0x10000000),
started as panthor's scheduler does, and a five-instruction stream (Mesa genxml v10 encodings: MOVE48, 2x MOVE32,
STORE_MULTIPLE, WAIT) that stores 0xc0ffee01 0x600dcafe into our page; then the group is terminated and all is stopped as in
probe 3. MMU_INT_RAWSTAT is now printed after each step (probe 3 left 0x10000 at the halt). Mesa has no v11 genxml (v10,
v12, v13) and no G615 (0xb8a3) model: the user side will need G615 treated as v10, to be checked.

GPU probe 4 result (12 November, 01:54 UTC): global setup acknowledged, group 0 started (CSG ack 0x11; suspend size
0x6740, 8 streams, stream features 0x7105f), but the stream never started (CS ack 0) and the target stayed 0: the MCU took a
translation fault on AS0 (FAULTSTATUS 0x7cd002c3 = level-3 translation fault, read, at 0x1000) and hung (no halt). 0x1000 is
the queue's input page 0x10001000 cut to 28 bits: **the MCU addresses 28 bits**, which is why panthor puts the queues'
pages in the shared region (0x4000000-0x8000000). Also: MMU_INT_RAWSTAT bit 16 goes up at the AS0 UPDATE itself, so it is
not a fault of the boot. Probe 4b (`gpu-sonda-4b.zip`, image 17632135): the job's pages at 0x5000000.

GPU probe 4b result (12 November, 02:00 UTC): **the GPU ran our command stream.** Group 0 started (CSG ack 0x11), the
queue started (CS ack 1), and 1.2 ms after the doorbell the target held 0xc0ffee01 0x600dcafe; queue extract 0x28 (all five
instructions), no fault, no fatal. Group terminated, MCU halted and disabled, GPU off, Android booted. MMU_INT_RAWSTAT
bits 16-17 rise at each AS UPDATE (AS0, AS1) with no fault status: benign. The whole path below Mesa (GPUEB power,
firmware, MCU, global interface, groups, queues, doorbells, the GPU's MMU) works from bare metal; next is a kernel driver.

Kernel GPU driver (12 November): `xnu/files/bsd/dev/redwood_mtkgpu.c`, boot-arg `mtkgpu=1`, hooked by the overlay after
the touch driver. It does in a kernel thread what probes 2-4b proved: GPUEB shared memory and power on, the firmware's
sections and the MCU's page table in 64 MB of GPU memory at `0x230000000` (System RAM above XNU's window and the loader's
2 MB, mapped uncached with ml_io_map, accessed 32 bits at a time), reset, L2, AS0, MCU boot, ping, global setup, group 0
in AS1 and the store stream; then the group is terminated and the GPU left on with its MCU running. sysctl
`kern.redwood.mtkgpu` = 6 when all worked; the log says it again every minute. mtkloader `-DXNU_GPU=1` copies the payload's
`gpufw` entry to `0x230000000` and adds `mtkgpu=1 mtkgpu_fw=<size> mtkgpu_eb=<gpueb_mem_addr>`; tools/mkimage.sh puts
`XNU_GPU_FW=<file>` in the payload. Host-checked: the firmware loader and page tables against the real file.

Run 25 (`ios-gpu-25.zip`, image 2271afee): Isaac's kernel f571a090 with redwood_mtkgpu.c (the first build stopped on
`struct section`, which mach-o/loader.h already defines; renamed, and the file's names prefixed `mg_`), run 23's settings
(`XNU_BOOT=7 XNU_ARGS="cpus=7" XNU_SECS=300`, the watcher, `-DXNU_ROOT_UFS=1 -DXNU_TOUCH=1`, root.apfs as md0) and
`-DXNU_GPU=1 XNU_GPU_FW=fw-gpu/mali_csffw.bin`.

Run 25 result (12 November, 02:52 UTC): panic at 15.94 s, before iOS: `kmem_alloc(..., 67108864, ...): failed with 3`
from the GPU driver's thread: ml_io_map of the 64 MB GPU memory found no room (device mappings come from a small map).
The loader side worked (firmware copied, `mtkgpu=1 mtkgpu_fw=290816 mtkgpu_eb=2145583104` given). Fixed: the driver maps
3 MB (firmware 0x60000, page tables, sections from 0xa0000, job pages; 0x254000 used) and 0xa0000 of registers.

Run 25b (`ios-gpu-25b.zip`, image 9f499853, kernel 0f563a75; 12 November, 03:18 UTC): **the GPU driver came up to step 6
of 6 inside iOS.** The GPUEB powered the GPU (count 1, MFG1 0xc000220d), GPU_ID 0xb8a31030, the firmware's 8 sections and 10
page-table pages loaded, the MCU booted (CSF 2.5.0, 8 groups), and the command stream ran: target 0xc0ffee01 0x600dcafe, no
fault. iOS booted as in run 23 beside it, and XNU came back at 300 s with no panic. Next: a user client for the GPU and
Mesa (panthor-style backend, G615 treated as v10).
