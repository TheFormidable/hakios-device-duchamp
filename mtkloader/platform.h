/*
 * platform.h: what mtkloader needs to know of the POCO X6 Pro 5G (duchamp, Xiaomi), MediaTek Dimensity 8300 Ultra (MT6897).
 *
 * The reference loader (hakios-mt6768, Infinix Hot 20, MT6768) had these as constants spread over mtkloader.c and mtkdbg.h.
 * Here they are in one place, each with where it came from:
 *
 *   [dtb]    read from the SoC's device tree that LineageOS ships for this phone
 *            (github.com/mt6897-devs/device_xiaomi_duchamp-kernel, dtb/mt6897.dtb, with dtbo.img applied):
 *            a register address of a node, nothing copied.
 *   [phone]  not in that tree -- LK fills it in at every boot (memory, ramoops, the framebuffer) or it is only seen on a
 *            running phone. The loader reads it from LK's own device tree at run time where it can (mtkfdt.h); the value
 *            here is only the fallback, and tools/duchamp-probe.sh prints the real one from Android. Every [phone] value
 *            must be checked against that output before the first run.
 *   [ref]    kept from the MT6768 loader because nothing says it differs; to be confirmed on the phone.
 *
 * Nothing of this has run on a duchamp yet (state in ../device.conf).
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#define PLAT_NAME               "POCO X6 Pro 5G (duchamp, MT6897)"
#define PLAT_BOARD              "duchamp"

/* ---- the console UART: uart0, serial@11001000, "mediatek,mt6577-uart" [dtb] (the MT6768's was 0x11002000) ---- */
#define PLAT_UART_BASE          0x11001000ULL

/* ---- the watchdog: TOPRGU, watchdog@1c00a000, "mediatek,mt6897-wdt", "mediatek,mt6589-wdt" [dtb] ----
 * mt6589-wdt compatible: the same MODE/LENGTH/RESTART/SWRST registers and keys as the MT6768's at 0x10007000. */
#define PLAT_TOPRGU_BASE        0x1c00a000ULL

/* ---- GIC-600 (GICv3): interrupt-controller, "arm,gic-v3" [dtb] ----
 * GICD 0x0c400000 (0x40000), GICR 0x0c440000 (0x200000: eight redistributors, 0x40000 each -- GICv4.1 frames).
 * The MT6768's were 0x0c000000 and 0x0c040000 (0x20000 apart). */
#define PLAT_GICD_BASE          0x0c400000ULL
#define PLAT_GICD_SIZE          0x40000ULL
#define PLAT_GICR_BASE          0x0c440000ULL
#define PLAT_GICR_SIZE          0x200000ULL
#define PLAT_GICR_STRIDE        0x40000ULL
/* what the kernel's arm-io node covers: 0x0c000000 to 0x20000000, as on the MT6768 (every block below is inside) */
#define PLAT_IO_BASE            0x0c000000ULL
#define PLAT_IO_SIZE            0x14000000ULL

/* ---- the cores [dtb]: cpu@0..3 Cortex-A510 (MPIDR 0x000..0x300), cpu@100..102 Cortex-A715 (0x400..0x600),
 * cpu@200 the Cortex-A715 of its own cluster that runs fastest (0x700). Eight in all, like the MT6768.
 * The order the kernel is offered them: the one LK leaves us on (0) first, then the four A715s from the fastest, then the
 * other A510s -- so cpus=4 already gives iOS three big cores. The watcher takes core_order[XNU_CORES]. */
#define PLAT_CORE_ORDER         { 0, 7, 6, 5, 4, 1, 2, 3 }
#define PLAT_FIRST_BIG_CORE     4       /* cores below are the small ones (Cortex-A510) */
#define PLAT_MPIDR(core)        ((uint32_t)(core) << 8)

/* ---- the display ----
 * The panel: 1220 x 2712, DSC video mode (panel-n11a-42-02-0a-dsc-vdo / -41-02-0b, two suppliers) [dtb]. Video mode is what
 * matters: the display path keeps scanning LK's buffer, so whatever is written there shows on the next frame.
 * OVL: on the MT6897 the overlay engines moved to their own block, ovlsys at 0x14400000; disp-ovl0@14402000 [dtb].
 * The MT6768's OVL0 was at 0x1400b000 (on the MT6897 that address is the DP interface). The layer registers are assumed to
 * be where the MT6768's were (L0_CON 0x30, L0_PITCH 0x44, L0_ADDR 0xf40) [ref]: display_find() only believes what it reads
 * there if it lies inside LK's framebuffer, so a wrong guess costs nothing but the fallback.
 * The mutex: disp-mutex0@14001000 [dtb], as on the MT6768. */
#define PANEL_W                 1220u
#define PANEL_H                 2712u
#define PLAT_FB_SCALE           2       /* the loader's text at 2x: at 1x it is 152 columns on a 6.67" screen */
#define PLAT_OVL0_BASE          0x14402000ULL
/* 0: the loader neither reads nor writes the OVL (a read of a block whose clock is off can hang MediaTek's bus); it trusts
 * atag,videolfb alone and leaves LK's alpha and layers as they are. 1 once tools/duchamp-probe.sh has shown which OVL scans
 * LK's buffer. */
#define PLAT_OVL_TOUCH          0
#define PLAT_DISP_MUTEX_BASE    0x14001000ULL
/* LK's framebuffer: chosen/atag,videolfb in LK's device tree gives its base and size at run time (mtkfdt.h). These are only
 * the fallback [phone]: no buffer is assumed unless one of the two is found. */
#define PLAT_LK_FB_BASE         0x0ULL
#define PLAT_LK_FB_SIZE         0x0ULL
/* LK aligns the width to 32 pixels on the MT6768 (720 -> 736) [ref]: 1220 -> 1248 */
#define PLAT_LK_FB_PITCH        (1248u * 4u)
#define PLAT_FB_PITCH_MAX       (2048u * 4u)

/* ---- DRAM [phone] ----
 * /memory in the shipped tree is LK's placeholder (0x40000000 + 1 GB); the phone has 8 or 12 GB from 0x40000000.
 * Most of the MT6897's reserved memory is placed by LK at boot (alloc-ranges up to 0x1_3000_0000), so the range below must
 * be checked against /proc/iomem and /sys/firmware/fdt's reserved-memory on the phone (tools/duchamp-probe.sh) before the
 * first run: XNU must get a range that nothing else uses. The MT6768's was 0xc0000000 + 0xfe000000. */
#define PLAT_XNU_PHYS_BASE      0xc0000000ULL
#define PLAT_XNU_MEM_SIZE       0xfe000000ULL

/* Where LK starts the boot image's kernel (this program) [phone]. The MT6768's LK used 0x40080000 (base 0x40000000 + the
 * Image header's text_offset 0x80000). duchamp's BoardConfig has kernel base 0x3fff8000 with the default kernel offset
 * 0x8000, i.e. 0x40000000 -- but a header v4 boot image carries no addresses, so what LK does is its own: read it from
 * Android's /proc/iomem ("Kernel code") or LK's log in /proc/bootprof; link.ld and start.S's text_offset follow it. */
#define PLAT_KERNEL_PA          0x40080000ULL
/* Where Android's own Image is moved to and started [phone]: its header says text_offset 0, so it must sit on a 2 MB
 * boundary; LK's own choice for a plain boot is the address to use (the same "Kernel code" line of /proc/iomem). The copy goes
 * downwards over this program, which is safe (chain_stub runs from PLAT_CHAIN_STUB_PA). */
#define PLAT_ANDROID_PA         0x40000000ULL
#define PLAT_CHAIN_STUB_PA      0x4a000000ULL   /* [ref] free DRAM for the copy stub, clear of the 50 MB Android Image */
/* run state kept across the watchdog reset, and XNU's panic log [ref]: must be inside memory nothing else writes */
#define PLAT_MARKER_PA          0x80000000ULL
#define PLAT_PANIC_LOG_BASE     0x80100000ULL

/* ---- ramoops [phone] ----
 * LK adds the ramoops node at boot ("ramoops" in reserved-memory); mtkfdt.h finds it and works out the zones as Linux's
 * ramoops does (dump records first, then console, ftrace, pmsg at the end). These are only the fallback: 0 = no pstore. */
#define PLAT_RAMOOPS_CONSOLE    0x0ULL
#define PLAT_RAMOOPS_CONSOLE_SIZE 0x0u
#define PLAT_RAMOOPS_PMSG       0x0ULL
#define PLAT_RAMOOPS_PMSG_SIZE  0x0u

/* ---- what is not used yet, for the next steps (docs/porting-mt6897.md) [dtb] ---- */
#define PLAT_UFSHCI_BASE        0x112b0000ULL   /* ufshci@112b0000, "mediatek,mt8183-ufshci": the storage (UFS, not eMMC) */
#define PLAT_UFSPHY_BASE        0x112a0000ULL
#define PLAT_SPI2_BASE          0x11012000ULL   /* spi2: the touch controller (FocalTech FT3683G or Goodix, xiaomi_touch@0) */
#define PLAT_KEYPAD_BASE        0x1c00e000ULL   /* kp@1c00e000: volume up (hw key 0 -> KEY_VOLUMEUP); power, volume down: PMIC */
#define PLAT_SPMI_BASE          0x1cc04000ULL   /* spmi@1cc04000 (PMIF): MT6363 + MT6368 PMICs. No PWRAP as on the MT6768 */
#define PLAT_MALI_BASE          0x13000000ULL   /* mali@13000000, "arm,mali-valhall": Mali-G615 MC6, CSF; powered by GPUEB */
#define PLAT_GPUEB_BASE         0x13c00000ULL
#define PLAT_SCP_BASE           0x1c400000ULL   /* the sensor hub */
#define PLAT_DSI0_BASE          0x1400d000ULL

#endif /* PLATFORM_H */
