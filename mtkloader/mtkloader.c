/*
 * mtkloader: boots XNU on the POCO X6 Pro 5G (duchamp, MT6897) from boot_a, and brings Android
 * back afterwards with XNU's log in pstore.
 *
 * The loader of hakios-mt6768 (Infinix Hot 20, X6826B, MT6768), carried over: what is the phone's or the SoC's is in
 * platform.h, LK's framebuffer and ramoops are read from LK's device tree (mtkfdt.h), and what only the MT6768 had (the
 * TD4160's firmware over SPI, the eMMC through MSDC0, the Mali-G52, the camera, the A75s' PLL) is left out. The notes below
 * are the reference phone's where they speak of it; nothing here has run on a duchamp yet.
 *
 * The boot image's kernel (tools/chainboot.py) is this program, then Android's own kernel Image
 * at CHAIN_OFFSET, then the XNU payload ("XNUP": kernel, ramdisk). Each boot, by the marker:
 *
 *   none     RUN: load XNU, give it LK's framebuffer, arm the watchdog, start it.
 *   RUNNING  XNU was cut short by the watchdog: copy its log from DRAM into ramoops, start
 *            Android.
 *   DONE     XNU came back to us (panic, early exception, orderly end) and its log was saved
 *            then: start Android.
 *
 * The log ends up in /sys/fs/pstore/console-ramoops-0 under Android. See mtkdbg.h for the
 * screen and ramoops output.
 *
 * Display: the panel is video mode and keeps scanning OVL0 layer 0 (LK's framebuffer,
 * 0x787b0000, pitch 2944 = 736 pixels). LK enables per-pixel alpha on that layer, and XNU's
 * console writes 0x00rrggbb pixels: fully transparent. So per-pixel alpha is switched off, and
 * XNU's color table gets alpha 0xff besides.
 */
#include <stdint.h>
#include <stddef.h>
#include "mtkdbg.h"              /* platform.h: this phone's addresses */
#include "identity.h"             /* xnuloader/identity.h of the HakiOS tree (build.sh: -I) */

/* XNU's view of memory: one contiguous range that nothing else (LK, Android, TEE, ATF, the
 * framebuffer) uses; 0xc0000000-0xfe000000 is free DRAM on this board. */
#define XNU_PHYS_BASE       PLAT_XNU_PHYS_BASE
#define XNU_MEM_SIZE        PLAT_XNU_MEM_SIZE   /* platform.h: to be checked against the phone's /proc/iomem */
/* Where the system image is put, clear of the kernel, as xnuloader does it (RESIDENT_RD_OFFSET). */
#define SYSTEM_PA           (XNU_PHYS_BASE + 0x08000000ULL)
#define SYSTEM_PART         "xnusys"
/* 0: all of it. Smaller is for trying the path out: packfs will not mount a half image. */
#ifndef SYSTEM_MAX_MB
#define SYSTEM_MAX_MB       0
#endif
/* boot_args.virtBase, as xnuloader/loader.c has it: the kernel's text is 48 MB above it. */
#define KERNEL_VIRT_BASE    0xfffffff004000000ULL
#define XNU_SLIDE           (XNU_PHYS_BASE - KERNEL_VIRT_BASE)
#define PAGE_SIZE_MAX       0x4000ULL

/* Seconds XNU may run before the watchdog resets into Android; 0: run until reset by hand. */
#ifndef XNU_WDT_SECONDS
#define XNU_WDT_SECONDS     0
#endif
/* Seconds after which the second core takes a snapshot of XNU and resets; 0: no watcher. */
#ifndef XNU_WATCH_SECONDS
#define XNU_WATCH_SECONDS   0
#endif
/*
 * How many processors the device tree offers the kernel. It takes them from the head of
 * core_order below, so what is left of that list is nobody's but ours -- the watcher takes the
 * next one. Leaving all eight in the tree and trusting "cpus=N" to stop the kernel sooner is a
 * guess about its parsing; this is not. XNU_EXTRA_ARGS should carry the same number as "cpus=".
 *
 * A watcher on a core the kernel wants is worse than none: PSCI CPU_ON answers "already on", the
 * kernel counts a processor that never checks in, and every rendezvous between cores waits for
 * it -- which is how a 15 minute run stayed on the Apple logo.
 */
#ifndef XNU_CORES
#define XNU_CORES           4
#endif
/* All eight only without the watcher (mkimage.sh: XNU_WATCH=0): then a kernel that hangs waits for the power key. */
_Static_assert(XNU_CORES >= 1 && XNU_CORES <= (XNU_WATCH_SECONDS ? 7 : 8),
    "one core at least, and one left for the watcher (XNU_WATCH_SECONDS=0 for all eight)");

/*
 * The core XNU starts on. LK leaves this program on core 0, a Cortex-A510 at 1.3 GHz, and until now the kernel
 * started there too: its whole start ran on the slowest core, and so does every interrupt a driver takes afterwards,
 * since gic_v3_spi_attach() routes an interrupt to the processor that attaches it -- the boot processor. With
 * XNU_BOOT_CORE=7 (the fastest Cortex-A715) this program starts that core early (boot_core_start()), it waits, and at
 * the jump it is that core that enters XNU, at the same address with the same arguments; this one then switches
 * itself off (PSCI CPU_OFF), so that the kernel can start it like any other, or becomes the watcher if the watcher
 * is to be core 0. If core 7 does not answer, XNU starts here as before.
 */
#ifndef XNU_BOOT_CORE
#define XNU_BOOT_CORE       0
#endif
_Static_assert(XNU_BOOT_CORE >= 0 && XNU_BOOT_CORE < 8, "a core of the eight");

/*
 * The processors in the order the kernel is offered them: the boot core first, then the rest of PLAT_CORE_ORDER (the
 * Cortex-A715s before the Cortex-A510s), and core 0, the one this program runs on, last if it is not the boot core.
 * So "cpus=4" gives iOS the four fast cores. order_cores() writes it once the boot core is known.
 */
static const uint8_t plat_core_order[8] = PLAT_CORE_ORDER;
static uint8_t core_order[8];
static int boot_core;                   /* the core XNU starts on: XNU_BOOT_CORE once it has checked in, else 0 */
#define WATCH_CORE          core_order[XNU_CORES < 8 ? XNU_CORES : 0]   /* (no watcher with all eight) */
/* Checkpoint for XNU to return at (osfmk/arm/arm_init.c LOADER_CHECKPOINT), 0 = none. */
#ifndef XNU_CHECKPOINT
#define XNU_CHECKPOINT      0
#endif
/* loader_return_after: XNU comes back by itself after that long, with its log (loader_return.c) */
#ifndef XNU_RETURN_AFTER
#define XNU_RETURN_AFTER    "0"
#endif
/* 1: always start XNU (tools/xnu-cycle.sh); 0: Android unless asked once (tools/boot-xnu.sh). */
/* 1: look at the touch screen's SPI and report, then go back to Android (no XNU). */
#ifndef TOUCH_PROBE
#define TOUCH_PROBE         0
#endif
/* 1: on the way to XNU, power the touch screen and ask which chip it is, and tell the kernel (mtktouch.h) */
#ifndef XNU_TOUCH
#define XNU_TOUCH           0
#endif
/* 1: read what of the GPU (Mali-G615, GPUEB, rails) is powered, writing nothing (mtkgpu.h); 2: also ask the GPUEB to
 * power it on and off; 3: and, while on, load the CSF firmware (payload entry "gpufw") and boot its MCU. Then Android. */
#ifndef GPU_PROBE
#define GPU_PROBE           0
#endif
/* 1: read the camera's pins, rails and I2C controller and report, if Android asked for it (tools/cam-probe.sh); then Android. */
/* 1: give the kernel's GPU driver (bsd/dev/redwood_mtkgpu.c) the CSF firmware (payload entry "gpufw") and the GPUEB's memory:
 * boot-args mtkgpu=1 mtkgpu_fw= mtkgpu_eb= (mtkgpu.h, gpu_for_xnu). */
#ifndef XNU_GPU
#define XNU_GPU             0
#endif
#ifndef CAM_PROBE
#define CAM_PROBE           0
#endif
/* 1: read the eMMC and report, then go back to Android (no XNU). */
#ifndef MSDC_TEST
#define MSDC_TEST           0
#endif
/* 1: check at EL1 that the timer interrupts arrive at all (diagnostics, leaves GIC state). */
/* the reference's Wi-Fi step 1, a list of Android's tree nodes that look like CONNSYS: on duchamp 48 nodes of mostly other
 * things (camera sensors, scpsys power domains), 7 KB at the head of every log. Back on with -DCONNSYS_REPORT=1 for Wi-Fi. */
#ifndef CONNSYS_REPORT
#define CONNSYS_REPORT      0
#endif
#ifndef TIMER_PROBE
#define TIMER_PROBE         0
#endif
#ifndef MENU_DEFAULT_XNU
#define MENU_DEFAULT_XNU    0
#endif
/*
 * 1: the kernel's text console on the screen (-v). It is a slow way to run -- scrolling reads
 * the whole framebuffer back -- and iOS cannot draw while it is on, so a real boot goes without
 * it and the log comes back through the loader instead (v_display = 1 says "graphics mode").
 */
#ifndef XNU_VERBOSE
#define XNU_VERBOSE         0
#endif
/* More for the kernel's command line: cores, the clock (epoch=), whatever a run wants to try. */
/* The serial number iOS is told (the device tree's serial-number): a placeholder, not any real device's. */
#ifndef XNU_SERIAL
#define XNU_SERIAL          "HAKIOS0000000000"
#endif
#ifndef XNU_EXTRA_ARGS
#define XNU_EXTRA_ARGS      "cpus=1"
#endif
/*
 * The root. On the MT6768 the system image stayed on the eMMC and the kernel read it as it needed it (emmc_root=1). This
 * phone's storage is UFS (ufshci@112b0000), which no driver of this loader or of the MediaTek kernel drivers reads yet: until
 * one does (docs/porting-mt6897.md, step 3), the payload's RAM disk is the root, md0 -- the guide's "boot with nothing of the
 * phone's", which ends at launchd. 1 is kept for the day the UFS driver lands (ufs_root=1, the common tree's hakios.ufshc.c
 * pointed at this controller).
 */
#ifndef XNU_ROOT_UFS
#define XNU_ROOT_UFS        0
#endif
#define XNU_ROOT_EMMC       XNU_ROOT_UFS        /* (the reference loader's name, used further down) */
/*
 * XNU_UFS_TEST=1: the loader brings the UFS up (as UFS_PROBE=3 found it works), finds XNU_UFS_PART on LUN 2, prints a sum
 * of its first 4 MB and hands the controller to the kernel with ufs_test=1, which reads the same 4 MB (and more) through
 * its own driver (bsd/dev/redwood_mtkufs.c) and prints its sum; md0 stays the root. Read only on both sides.
 * XNU_UFS_TEST=2: the same on super (9 GB, Android's system: what does not change while it runs), with ufs_test=2: the
 * kernel then reads 12 GB, all of super with its SHA-256 (Android's sha256sum of super must be the same) and 3 GB of
 * re-reads checked against the first pass.
 */
#ifndef XNU_UFS_TEST
#define XNU_UFS_TEST        0
#endif
#ifndef XNU_UFS_PART
#if XNU_UFS_TEST == 2
#define XNU_UFS_PART        "super"
#elif XNU_UFS_TEST
#define XNU_UFS_PART        "boot_a"            /* Android's boot partition: it is there on every phone, and read only */
#else
#define XNU_UFS_PART        "xnusys"            /* the system image's partition, made once the phone may be wiped */
#endif
#endif
#define XNU_UFS             (XNU_ROOT_UFS || XNU_UFS_TEST)
static int ufs_xnu_ok;                          /* the UFS is up for the kernel, the partition found: */
static uint64_t ufs_xnu_lba, ufs_xnu_blocks;    /* where it is, in 4 KB blocks of LUN 2 */
#if XNU_ROOT_UFS
#define XNU_ROOT_ARG        "rd=md0 ufs_root=1"
#elif XNU_UFS_TEST == 2
#define XNU_ROOT_ARG        "rd=md0 ufs_test=2"
#elif XNU_UFS_TEST
#define XNU_ROOT_ARG        "rd=md0 ufs_test=1"
#else
#define XNU_ROOT_ARG        "rd=md0"
#endif
#if XNU_VERBOSE
#define XNU_BOOT_ARGS       "debug=0x8 serial=3 " XNU_ROOT_ARG " -v " XNU_EXTRA_ARGS " loader_return_after=" XNU_RETURN_AFTER
#else
#define XNU_BOOT_ARGS       "debug=0x8 serial=3 " XNU_ROOT_ARG " " XNU_EXTRA_ARGS " loader_return_after=" XNU_RETURN_AFTER
#endif

#define CHAIN_OFFSET        0x40000u
#define KERNEL_PA           PLAT_KERNEL_PA
#define ANDROID_PA          PLAT_ANDROID_PA
#define CHAIN_STUB_PA       PLAT_CHAIN_STUB_PA

/* Run state, kept in DRAM across the watchdog reset (three copies: a bit may flip). */
#define MARKER_PA           PLAT_MARKER_PA
#define MARK_RUNNING        0x4e52584eu /* "NXRN" */
#define MARK_DONE           0x4e44584eu /* "NXDN" */

#define MH_MAGIC_64         0xfeedfacfu
#define LC_SEGMENT_64       0x19
#define LC_SYMTAB           0x2
#define LC_UNIXTHREAD       0x5
#define PAYLOAD_MAGIC       0x50554e58u /* "XNUP" */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

/* pexpert/pexpert/arm64/boot.h: virtBase comes first (start.s reads BA_VIRT_BASE, BA_PHYS_BASE). */
struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

_Static_assert(__builtin_offsetof(struct boot_args, virtBase) == 8, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, physBase) == 16, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, video) == 40, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, deviceTreeP) == 96, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, commandLine) == 108, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, bootFlags) == 720, "boot_args");

struct mach_header_64 {
	uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};

struct load_command {
	uint32_t cmd, cmdsize;
};

struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

struct symtab_command {
	uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize;
};

struct nlist_64 {
	uint32_t n_strx;
	uint8_t n_type, n_sect;
	uint16_t n_desc;
	uint64_t n_value;
};

struct payload_header {
	uint32_t magic;
	uint32_t size;
};

struct payload_entry {
	char name[16];
	uint32_t size;
	uint32_t reserved;
};

struct payload_info {
	const uint8_t *kernel;
	uint32_t kernel_size;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
};

/* The XNU symbols the loader works with (kernel virtual addresses, 0 if absent). */
struct xnu_syms {
	uint64_t stub_slot, vc_colors, log, log_len, log_ring_at, log_wrapped, panic_str, panic_args, iso_font,
	    msgbufp, cpu_ttep, live, uart_tx_ready, uart_tx_data, uart_rx_ready, uart_putc, uart_putc_options, log_putc,
	    nprocs, tasks_count, initproc, gic_cpu_init,
	    gic_unhandled_count, gic_last_unhandled, gic_active_iar, host_keep, host_keep_len, host_keep_dropped,
	    hk_keep, hk_keep_len, hk_keep_dropped, mt6577_putc, mt6577_puts,
	    cpu_data_entries, threads, threads_count, kernel_stack_size, proc_struct_size;
};

static struct xnu_syms xsyms; /* of the kernel that was started; kept for when it comes back */
static const uint8_t *xkernel;  /* its Mach-O file, for symbols */
/* XNU's x29, x30, TCR_EL1 and TTBR1_EL1 when it came back (start.S) */
uint64_t xnu_fp[4];
static volatile uint64_t xnu_back __attribute__((aligned(64))); /* set by xnu_returned() */
/* XNU_BOOT_CORE with cpus=8: core 0's PSCI CPU_OFF after the hand-over. 1: called and never came back (switched
 * off); 0x1xx: it came back with xx (a refusal). Printed by xnu_returned(), the only one left to say it. */
static volatile uint64_t core0_off;
/*
 * XNU_BOOT_CORE with cpus=8: core 0 kept for the kernel. Switched off with PSCI CPU_OFF it never came back when the
 * kernel asked for it with CPU_ON (run 11: the firmware said yes, the core never arrived), so instead it waits here, MMU
 * and caches off, until the kernel writes its reset vector into this mailbox: the kernel's psci_smp.c, as
 * xnu/duchamp-overlay.py patches it, looks for it where cpu_park= on the command line says. The words: the magic
 * "PARK", the core's MPIDR, the entry (the kernel's), -, the state (1 waiting, 2 gone into the kernel), -.
 */
#ifndef XNU_CORE0_PARK
#define XNU_CORE0_PARK      (XNU_CORES == 8)
#endif
#define CORE_PARK_MAGIC     0x4b524150u         /* "PARK" */
static volatile uint64_t core_park[8] __attribute__((aligned(64)));
extern void __attribute__((noreturn)) park_jump(uint64_t entry);

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16];
	int depth;
};

extern char _start[];
extern char chain_stub[], chain_stub_end[];
extern char xnu_return_stub[];
extern void jump_to_kernel(uint64_t entry_pa, uint64_t boot_args_pa);
extern void psci_system_reset(void);
extern int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context);
extern void watcher_entry(void);
extern void boot_core_entry(void);
extern int64_t psci_cpu_off(void);
extern void mmu_on(uint64_t l1_table);
extern void dcache_clean_range(uint64_t va, uint64_t len);
extern uint64_t irq_wait(uint64_t until_ticks);

/* set by the IRQ handler in start.S */
uint64_t irq_count, irq_intid;

static uint64_t lk_dtb;

/* ---- small libc (device memory: plain loops, word copies when aligned) ---- */

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	if (c == 0 && !((uintptr_t)d & 7)) {
		for (; n >= 8; n -= 8, d += 8) {
			*(volatile uint64_t *)d = 0;
		}
	}
	while (n--) {
		*(volatile uint8_t *)d++ = (uint8_t)c;
	}
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (!(((uintptr_t)d | (uintptr_t)s) & 7)) {
		for (; n >= 8; n -= 8, d += 8, s += 8) {
			*(volatile uint64_t *)d = *(const volatile uint64_t *)s;
		}
	}
	while (n--) {
		*(volatile uint8_t *)d++ = *(const volatile uint8_t *)s++;
	}
	return dst;
}

size_t strlen(const char *s)
{
	size_t len = 0;
	while (*s++) {
		len++;
	}
	return len;
}

/* 0 when the first n bytes are the same; device memory, so byte by byte. */
static int __attribute__((unused)) memcmp_n(const volatile void *a, const void *b, size_t n)
{
	const volatile uint8_t *x = a;
	const uint8_t *y = b;

	for (size_t i = 0; i < n; i++) {
		if (x[i] != y[i]) {
			return 1;
		}
	}
	return 0;
}

int strcmp(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* ---- output: screen + ramoops + UART ---- */

#define MT6768_UART_BASE PLAT_UART_BASE    /* (the name of the reference loader; platform.h) */

static void uart_putc(char c)
{
	uint32_t timeout = 20000;
	while (!(rd32(MT6768_UART_BASE + 0x14) & 0x20) && --timeout) {
	}
	wr32(MT6768_UART_BASE, (uint8_t)c);
}

static void out_putc(char c)
{
	if (c == '\n') {
		uart_putc('\r');
	}
	uart_putc(c);
	dbg_putc(c);
}

static void out(const char *s)
{
	while (*s) {
		out_putc(*s++);
	}
}

static void out_hex(uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i = 60;
	out("0x");
	while (i > 0 && !((v >> i) & 0xf)) {
		i -= 4;
	}
	for (; i >= 0; i -= 4) {
		out_putc(hex[(v >> i) & 0xf]);
	}
}

static void out_dec(uint64_t v)
{
	char buf[21];
	int n = 0;
	do {
		buf[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n) {
		out_putc(buf[--n]);
	}
}

#include "mtkconnsys.h"         /* Wi-Fi step 1: Android's device tree on CONNSYS, printed (uses out()) */
#include "mtkfdt.h"             /* LK's framebuffer and ramoops, from its device tree */

/* ---- run state ---- */

static void marker_set(uint32_t v)
{
	for (int i = 0; i < 3; i++) {
		wr32(MARKER_PA + 64 * i, v);
	}
}

static uint32_t marker_get(void)
{
	uint32_t a = rd32(MARKER_PA), b = rd32(MARKER_PA + 64), c = rd32(MARKER_PA + 128);
	return (a == b || a == c) ? a : (b == c ? b : 0);
}

/* ---- the display ---- */

static uint64_t fb_base;
static uint32_t fb_pitch;

/* LK's layer as OVL0 scans it, if it is where LK said; LK's values otherwise. */
static void display_find(void)
{
	fb_base = LK_FB_BASE;
	fb_pitch = LK_FB_PITCH;
#if PLAT_OVL_TOUCH
	uint32_t addr = rd32(OVL0_BASE + OVL_L0_ADDR);
	uint32_t pitch = rd32(OVL0_BASE + OVL_L0_PITCH) & 0xffff;

	if (fb_base && addr >= LK_FB_BASE && addr < LK_FB_BASE + LK_FB_SIZE && pitch >= PANEL_W * 4 && pitch <= PLAT_FB_PITCH_MAX) {
		fb_base = addr;
		fb_pitch = pitch;
	}
#endif
}

/* Opaque pixels only from here on: XNU's console writes alpha 0. */
static void display_alpha_off(void)
{
#if !PLAT_OVL_TOUCH
	out("OVL: left as LK set it (PLAT_OVL_TOUCH 0)\n");
	return;
#endif
	uint32_t con = rd32(OVL0_BASE + OVL_L0_CON);
	wr32(OVL0_BASE + OVL_L0_CON, con & ~(1u << 8));
	out("OVL0 L0_CON ");
	out_hex(con);
	out(" -> ");
	out_hex(rd32(OVL0_BASE + OVL_L0_CON));

	/*
	 * LK composes two layers: layer 0 is the picture, and layer 3 is its own overlay with the
	 * warning it prints on an unlocked phone ("Orange State: your device has been unlocked"),
	 * from a buffer of its own. Nothing of iOS writes to that buffer, so the words stayed on
	 * top of the home screen. Leave the overlay's layers out of what the hardware composes.
	 */
	uint32_t src = rd32(OVL0_BASE + OVL_SRC_CON);
	wr32(OVL0_BASE + OVL_SRC_CON, src & 1u);
	wr32(DISP_MUTEX_EN, 1);
	wr32(DISP_MUTEX_RUN, 1);
	out(", SRC_CON ");
	out_hex(src);
	out(" -> ");
	out_hex(rd32(OVL0_BASE + OVL_SRC_CON));
	out("\n");
}

/* ---- payload and the kernel's symbols ---- */

static const struct payload_header *payload_find(void)
{
	const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
	uint64_t off = hdr[6]; /* res3 */
	const struct payload_header *ph = (const struct payload_header *)((uint64_t)_start + off);

	if (off == 0 || (off & 15) || ph->magic != PAYLOAD_MAGIC) {
		return NULL;
	}
	return ph;
}

static int parse_payload(struct payload_info *pi)
{
	const struct payload_header *ph = payload_find();

	memset(pi, 0, sizeof(*pi));
	if (!ph) {
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		if (e->name[0] == 0) {
			break;
		}
		if (strcmp(e->name, "kernel") == 0) {
			pi->kernel = p + sizeof(*e);
			pi->kernel_size = e->size;
		} else if (strcmp(e->name, "ramdisk") == 0) {
			pi->ramdisk = p + sizeof(*e);
			pi->ramdisk_size = e->size;
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return pi->kernel != NULL && ((const struct mach_header_64 *)pi->kernel)->magic == MH_MAGIC_64;
}

/* One entry of the payload by its name (the kernel's not needed): its data and size, or NULL. */
static const uint8_t *payload_entry(const char *name, uint32_t *size)
{
	const struct payload_header *ph = payload_find();

	if (!ph) {
		return NULL;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		if (e->name[0] == 0) {
			break;
		}
		if (strcmp(e->name, name) == 0 && p + sizeof(*e) + e->size <= pend) {
			*size = e->size;
			return p + sizeof(*e);
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return NULL;
}

static void find_syms(const uint8_t *kernel, struct xnu_syms *s)
{
	/* in the order of struct xnu_syms: the loop below fills it field by field */
	static const char *const names[] = {
		"_loader_stub_slot", "_vc_colors", "_loader_log", "_loader_log_len", "_loader_log_ring_at",
		"_loader_log_wrapped", "_debugger_panic_str", "_debugger_panic_args", "_iso_font",
		"_msgbufp", "_cpu_ttep", "_loader_live",
		"_vmapple_uart_transmit_ready", "_vmapple_uart_transmit_data",
		"_vmapple_uart_receive_ready", "_uart_putc", "_uart_putc_options", "_loader_log_putc",
		"_nprocs", "_tasks_count", "_initproc", "_gic_v3_irq_cpu_init",
		"_gic_unhandled_count", "_gic_last_unhandled", "_gic_active_iar",
		"_redwood_host_keep", "_redwood_host_keep_len", "_redwood_host_keep_dropped",
		/* (the same in a kernel of the common tree, hakios-xnu-kernel's mtk/mt6768/drivers/gpu/hakios.mali.c) */
		"_hakios_host_keep", "_hakios_host_keep_len", "_hakios_host_keep_dropped",
		"_mt6577_uart_putc", "_mt6577_uart_puts",
		/* for the watcher's look at the processors and the threads */
		"_CpuDataEntries", "_threads", "_threads_count", "_kernel_stack_size", "_proc_struct_size",
	};
	_Static_assert(sizeof(struct xnu_syms) == sizeof(names), "every field needs its name, in order");
	uint64_t *slots = (uint64_t *)s;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	const struct symtab_command *st = NULL;

	memset(s, 0, sizeof(*s));
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		}
	}
	if (!st) {
		return;
	}
	const struct nlist_64 *sym = (const struct nlist_64 *)(kernel + st->symoff);
	const char *strs = (const char *)(kernel + st->stroff);
	for (uint32_t i = 0; i < st->nsyms; i++) {
		const char *n = strs + sym[i].n_strx;
		if (n[0] != '_' || (n[1] != 'l' && n[1] != 'v' && n[1] != 'd' && n[1] != 'i' && n[1] != 'm' && n[1] != 'c' && n[1] != 'g' && n[1] != 'n' && n[1] != 't' && n[1] != 'r' && n[1] != 'h' && n[1] != 'C' && n[1] != 'k' && n[1] != 'p')) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
			if (!slots[k] && strcmp(n, names[k]) == 0) {
				slots[k] = sym[i].n_value;
			}
		}
	}
}

static uint64_t kva_to_pa(uint64_t va)
{
	return va + XNU_SLIDE;
}


/* ---- device tree for XNU ---- */

static void dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, strlen(name));
	*(volatile uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)strlen(value) + 1);
}

static void dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];
	memset(field, 0, sizeof(field));
	memcpy(field, value, strlen(value));
	dt_prop(w, name, field, size);
}

static void dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void dt_end(struct dt_writer *w)
{
	w->depth--;
}


/* What the iPhone's own device tree says (model, compatible, product ...), from xnuloader. */
static void dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		/* the panel is sRGB, not the iPhone X's P3 (see xnuloader/loader.c) */
		if (strcmp(props[i].name, "artwork-display-gamut") == 0) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		/*
		 * The screen class iOS lays itself out for. The identity was generated with the panel's
		 * own height here, 1640, which no iPhone has: iOS takes an unknown class for a phone
		 * with a home button, and the Control Centre came up from the bottom edge as on an
		 * iPhone 8. The display driver reports the iPhone X's 1125 x 2436 whatever the panel,
		 * so the class has to be the iPhone X's too.
		 */
		if (strcmp(props[i].name, "artwork-device-subtype") == 0) {
			static const uint32_t subtype = 2436;

			dt_prop(w, props[i].name, &subtype, sizeof(subtype));
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* An empty NVRAM image in CHRP format (iokit/Kernel/IONVRAMCHRPHandler.cpp): IOKit needs one. */
#define NVRAM_BANK_SIZE 0x2000u
static uint8_t nvram_image[NVRAM_BANK_SIZE];

static void nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, strlen(name));
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* Panic log region (the device tree's "pram"): DRAM outside XNU's window, kept across a reset. */
#define PANIC_LOG_BASE PLAT_PANIC_LOG_BASE
#define PANIC_LOG_SIZE 0x80000u

/* As xnuloader/loader.c builds it for x6826b, minus what needs UEFI. */
static uint32_t build_device_tree(uint64_t dt_pa, uint64_t rd_pa, uint64_t rd_size,
    uint64_t sys_pa, uint64_t sys_size)
{
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = dbg_freq();
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(dbg_ticks() >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	dt_prop_padded(&w, "serial-number", XNU_SERIAL, 32);
	dt_prop_padded(&w, "mlb-serial-number", XNU_SERIAL "MLB", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop_str(&w, "darwin-board", PLAT_BOARD);
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", XNU_PHYS_BASE);
	dt_prop_u64(&w, "dram-size", XNU_MEM_SIZE);
	dt_prop_u32(&w, "debug-enabled", 1);
	dt_prop_u32(&w, "display-scale", 2);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", "smc");
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	/*
	 * "RAMDisk" becomes /dev/md0, which is the root; "RAMDisk1" becomes /dev/md1. With the big
	 * system image present it is the root, and the payload's small one is the second disk --
	 * the same way round as xnuloader has it.
	 */
	dt_begin(&w, "memory-map");
	if (sys_size != 0) {
		uint64_t sys_reg[2] = { sys_pa, sys_size };
		dt_prop(&w, "RAMDisk", sys_reg, sizeof(sys_reg));
		if (rd_size != 0) {
			uint64_t rd_reg[2] = { rd_pa, rd_size };
			dt_prop(&w, "RAMDisk1", rd_reg, sizeof(rd_reg));
		}
	} else if (rd_size != 0) {
		uint64_t rd_reg[2] = { rd_pa, rd_size };
		/* (with the system image on the eMMC the payload stays md1, where stage0 mounts it) */
		dt_prop(&w, XNU_ROOT_EMMC ? "RAMDisk1" : "RAMDisk", rd_reg, sizeof(rd_reg));
	}
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "options");
	dt_end(&w);

	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	dt_begin(&w, "cpus");
	for (int i = 0; i < XNU_CORES; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
		int core = core_order[i];

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", PLAT_MPIDR(core));
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (core < PLAT_FIRST_BIG_CORE) { /* cores 0-3 are the Cortex-A510s, 4-7 the A715s */
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", PLAT_BOARD);
	uint64_t ranges[3] = { 0, PLAT_IO_BASE, PLAT_IO_SIZE };
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { PLAT_GICD_BASE - PLAT_IO_BASE, PLAT_GICD_SIZE, PLAT_GICR_BASE - PLAT_IO_BASE, PLAT_GICR_SIZE };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);

	/* The console the kernel is told about; its driver is patched in run_xnu(). */
	dt_begin(&w, "uart0");
	dt_prop_str(&w, "device_type", "uart");
	dt_prop_str(&w, "compatible", "mediatek,mt6577-uart");
	uint64_t uart_reg[2] = { PLAT_UART_BASE - PLAT_IO_BASE, 0x1000 };
	dt_prop(&w, "reg", uart_reg, sizeof(uart_reg));
	dt_prop_u32(&w, "boot-console", 1);
	dt_end(&w);

	dt_end(&w);
	dt_end(&w);
	return (uint32_t)(w.cur - w.base);
}

/* ---- leaving: Android, or a reset ---- */

static void __attribute__((noreturn)) reset_board(void)
{
	psci_system_reset();
	wdt_reset();
}


static void __attribute__((noreturn)) chain_android(void)
{
	const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
	const volatile uint32_t *img = (const volatile uint32_t *)((uint64_t)_start + CHAIN_OFFSET);
	uint64_t image_size = (hdr[5] + 7) & ~7ULL; /* res2 */

	marker_set(0);
	if (image_size == 0 || img[14] != 0x644d5241u) { /* "ARM\x64" */
		out("no Android kernel in this image: reset\n");
		dbg_delay_ms(3000);
		reset_board();
	}
	out("starting Android\n");

	volatile uint32_t *dst = (volatile uint32_t *)CHAIN_STUB_PA;
	const volatile uint32_t *src = (const volatile uint32_t *)chain_stub;
	for (uint64_t i = 0; i < (uint64_t)(chain_stub_end - chain_stub) / 4; i++) {
		dst[i] = src[i];
	}
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	void (*stub)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = (void *)CHAIN_STUB_PA;
	/* Android's GKI Image says text_offset 0: it wants a 2 MB aligned base, which is not where this program runs */
	stub(lk_dtb, ANDROID_PA, (uint64_t)img, image_size, ANDROID_PA);
	__builtin_unreachable();
}

void loader_fault(uint64_t vec, uint64_t esr, uint64_t elr, uint64_t far)
{
	fbc.fg = 0xffff4040u;
	out("\n*** loader fault: vector ");
	out_hex(vec);
	out(" ESR ");
	out_hex(esr);
	out(" ELR ");
	out_hex(elr);
	out(" FAR ");
	out_hex(far);
	out("\n");
	dbg_delay_ms(5000);
	chain_android();
}

/* ---- XNU's log into ramoops ---- */

struct span {
	const volatile char *p;
	uint64_t n;
};

/* Append spans (in order) to the ramoops console: all of it if it fits, else its start and
 * as much of its end as there is room for. */
static void save_log(const struct span *sp, int nsp)
{
	uint64_t total = 0, room, head, tail, skip_to, at = 0;

	if (!RAMOOPS_CONSOLE) {
		return;                 /* no ramoops found in LK's tree: nowhere to keep it */
	}
	for (int i = 0; i < nsp; i++) {
		total += sp[i].n;
	}
	room = PLOG_CAP - PLOG_HDR->size;
	room = room > 1024 ? room - 1024 : 0;
	head = total <= room ? total : room / 4;
	tail = total <= room ? 0 : room - head;
	skip_to = total - tail;

	uint64_t fb = fbc.base;
	fbc.base = 0; /* the screen gets the tail, separately */
	for (int i = 0; i < nsp; i++) {
		for (uint64_t k = 0; k < sp[i].n; k++, at++) {
			if (at < head || at >= skip_to) {
				unsigned char c = (unsigned char)sp[i].p[k];
				/* text only: a panic log ends in the stackshot's compressed kcdata, which pstore would pass on as is */
				plog_putc(c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f) ? (char)c : c ? '.' : ' ');
			} else if (at == head) {
				dbg_puts("\n\n[... ");
				dbg_dec(skip_to - head);
				dbg_puts(" bytes of the XNU log left out here ...]\n\n");
			}
		}
	}
	plog_putc('\n');
	fbc.base = fb;
}

/*
 * The kernel's lines for the host (kern.redwood.host_keep, bsd/dev/redwood_mali.c) into ramoops' pmsg
 * zone, whole: Android shows them as /sys/fs/pstore/pmsg-ramoops-0, and tools/xnu-cycle.sh appends them
 * to the run's log. Called on each way back from XNU, after the boot request in that zone was taken.
 */
static void save_host_keep(const struct xnu_syms *s)
{
	static const char head[] = "=== kept for the host by the kernel: ";
	volatile struct ramoops_hdr *h = (volatile struct ramoops_hdr *)RAMOOPS_PMSG;
	volatile char *d = (volatile char *)(RAMOOPS_PMSG + sizeof(struct ramoops_hdr));
	uint32_t cap = RAMOOPS_PMSG_SIZE - sizeof(struct ramoops_hdr), at = 0, len, dropped;
	char num[12];
	int n;

	if (!RAMOOPS_PMSG || RAMOOPS_PMSG_SIZE <= sizeof(struct ramoops_hdr)) {
		return;
	}
	uint64_t keep_va = s->host_keep ? s->host_keep : s->hk_keep;
	uint64_t len_va = s->host_keep ? s->host_keep_len : s->hk_keep_len;
	uint64_t dropped_va = s->host_keep ? s->host_keep_dropped : s->hk_keep_dropped;

	if (!keep_va || !len_va) {
		return;
	}
	len = rd32(kva_to_pa(len_va));
	dropped = dropped_va ? rd32(kva_to_pa(dropped_va)) : 0;
	for (const char *c = head; *c; c++) {
		d[at++] = *c;
	}
	for (int part = 0; part < 2; part++) {
		uint32_t v = part ? dropped : len;
		n = 0;
		do {
			num[n++] = (char)('0' + v % 10);
			v /= 10;
		} while (v);
		while (n) {
			d[at++] = num[--n];
		}
		const char *tail = part ? " dropped) ===\n" : " bytes (";
		for (const char *c = tail; *c; c++) {
			d[at++] = *c;
		}
	}
	const volatile char *keep = (const volatile char *)kva_to_pa(keep_va);
	for (uint32_t i = 0; i < len && at < cap; i++) {
		d[at++] = keep[i];
	}
	h->sig = RAMOOPS_SIG;
	h->start = at;          /* not wrapped: Linux takes data[start..size) then data[0..start) */
	h->size = at;
	dcache_clean_range(RAMOOPS_PMSG, RAMOOPS_PMSG_SIZE);
	out("host_keep: ");
	out_dec(len);
	out(" bytes into pmsg, ");
	out_dec(dropped);
	out(" lines dropped\n");
}

/* The last lines of what is in ramoops, on the lower part of the screen. */
static void show_log_tail(uint32_t lines)
{
	uint32_t size = PLOG_HDR->size, start = PLOG_HDR->start, n = 0;
	uint32_t top = PANEL_H - (lines + 1) * 16;
	uint64_t i;

	if (!fbc.base || size == 0 || size != start) { /* only a log that has not wrapped */
		return;
	}
	for (i = size; i > 0 && n <= lines; i--) {
		n += PLOG_DATA[i - 1] == '\n';
	}
	fb_rect(0, top - 4, PANEL_W, PANEL_H - top + 4, 0xff000000u);
	fb_rect(0, top - 4, PANEL_W, 2, 0xffff3030u);
	fbc.top = fbc.y = top;
	fbc.x = 0;
	fbc.scale = 1;
	for (; i < size; i++) {
		fbcon_putc(PLOG_DATA[i]);
	}
}

/*
 * A kernel virtual address to physical through XNU's own page tables (TTBR1_EL1; 4K or 16K
 * granule), as they were when it came back. 0 if not mapped, and 0 for anything (a table, or
 * what it maps) outside XNU's memory: the watcher reads them while XNU still runs and changes
 * them, and a stray read of a device's registers can have effects.
 */
static int in_xnu_mem(uint64_t pa)
{
	return pa >= PLAT_XNU_PHYS_BASE && pa < PLAT_XNU_PHYS_BASE + PLAT_XNU_MEM_SIZE;
}

static uint64_t xnu_va_to_pa_in(uint64_t va, uint64_t tcr, uint64_t ttbr1)
{
	uint32_t t1sz = (tcr >> 16) & 0x3f, tg1 = (tcr >> 30) & 3;
	uint32_t g = tg1 == 2 ? 12 : tg1 == 1 ? 14 : 0;
	if (!g || t1sz < 16) {
		return 0;
	}
	uint32_t vabits = 64 - t1sz, per = g - 3;
	uint32_t levels = (vabits - g + per - 1) / per;
	uint64_t table = ttbr1 & 0x0000fffffffffffeULL & ~((1ULL << g) - 1);
	for (uint32_t l = 0; l < levels; l++) {
		uint32_t shift = g + per * (levels - 1 - l);
		uint64_t idx = (va >> shift) & ((1ULL << per) - 1);
		if (l == 0) {
			idx &= (1ULL << (vabits - shift)) - 1;
		}
		if (!in_xnu_mem(table)) {
			return 0;
		}
		uint64_t d = *(volatile uint64_t *)(table + idx * 8);
		if (!(d & 1)) {
			return 0;
		}
		uint64_t oa = d & 0x0000ffffffffffffULL & ~((1ULL << g) - 1);
		if (l == levels - 1 || !(d & 2)) { /* page, or block */
			uint64_t span = 1ULL << shift;
			uint64_t pa = (oa & ~(span - 1)) | (va & (span - 1));
			return in_xnu_mem(pa) ? pa : 0;
		}
		table = oa;
	}
	return 0;
}

static uint64_t xnu_va_to_pa(uint64_t va)
{
	return xnu_va_to_pa_in(va, xnu_fp[2], xnu_fp[3]);
}

static uint64_t xnu_rd64(uint64_t va)
{
	uint64_t pa = xnu_va_to_pa(va);
	return pa && !(pa & 7) ? *(volatile uint64_t *)pa : 0;
}

static char xnu_rd8(uint64_t va)
{
	uint64_t pa = xnu_va_to_pa(va);
	return pa ? *(volatile char *)pa : 0;
}

static void out_xnu_str(uint64_t va, int max)
{
	char c;
	for (int i = 0; i < max && (c = xnu_rd8(va + i)) != 0; i++) {
		out_putc(c);
	}
}

/* AAPCS64 va_list, as debugger_panic_args points at it */
struct xnu_va_list {
	uint64_t stack, gr_top, vr_top;
	int32_t gr_offs, vr_offs;
};

static uint64_t va_next(struct xnu_va_list *ap)
{
	uint64_t v;
	if (ap->gr_offs < 0) {
		v = xnu_rd64(ap->gr_top + (int64_t)ap->gr_offs);
		ap->gr_offs += 8;
	} else {
		v = xnu_rd64(ap->stack);
		ap->stack += 8;
	}
	return v;
}

/* The panic message: debugger_panic_str formatted with debugger_panic_args (integers in hex). */
static void show_panic_string(const struct xnu_syms *x)
{
	uint64_t fmt = x->panic_str ? xnu_rd64(x->panic_str) : 0;
	uint64_t argp = x->panic_args ? xnu_rd64(x->panic_args) : 0;
	struct xnu_va_list ap = { 0 };

	if (!fmt) {
		out("(no panic string)\n");
		return;
	}
	if (argp) {
		ap.stack = xnu_rd64(argp);
		ap.gr_top = xnu_rd64(argp + 8);
		ap.vr_top = xnu_rd64(argp + 16);
		uint64_t offs = xnu_rd64(argp + 24);
		ap.gr_offs = (int32_t)offs;
		ap.vr_offs = (int32_t)(offs >> 32);
	}
	out("panic: ");
	for (int i = 0; i < 1024; i++) {
		char c = xnu_rd8(fmt + i);
		if (!c) {
			break;
		}
		if (c != '%') {
			out_putc(c);
			continue;
		}
		/* flags, width, precision, length */
		do {
			c = xnu_rd8(fmt + ++i);
		} while (c && ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == ' ' || c == '#' || c == '.' ||
		    c == 'l' || c == 'h' || c == 'z' || c == 'j' || c == 't' || c == 'q' || c == '*'));
		if (!argp && c != '%') {
			out("<?>");
			continue;
		}
		switch (c) {
		case '%':
			out_putc('%');
			break;
		case 's':
			out_xnu_str(va_next(&ap), 200);
			break;
		case 'c':
			out_putc((char)va_next(&ap));
			break;
		case 'd':
		case 'i':
		case 'u': {
			uint64_t v = va_next(&ap);
			out_dec(v);
			out("(");
			out_hex(v);
			out(")");
			break;
		}
		case 0:
			i = 1024;
			break;
		default:
			out_hex(va_next(&ap));
			break;
		}
	}
	out("\n");
}

/* The symbol at or below a kernel address, from the Mach-O symbol table: "name+0xoff". */
/* The kernel's symbol at or before addr (within 1 MB), or NULL. */
static const char *sym_find(uint64_t addr, uint64_t *base)
{
	const struct mach_header_64 *mh = (const struct mach_header_64 *)xkernel;
	const struct load_command *lc;
	const struct symtab_command *st = NULL;
	uint64_t best = 0;
	const char *best_name = NULL;

	if (!xkernel) {
		return NULL;
	}
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		}
	}
	if (!st) {
		return NULL;
	}
	const struct nlist_64 *sym = (const struct nlist_64 *)(xkernel + st->symoff);
	const char *strs = (const char *)(xkernel + st->stroff);
	for (uint32_t i = 0; i < st->nsyms; i++) {
		if ((sym[i].n_type & 0x0e) == 0x0e && sym[i].n_value <= addr && sym[i].n_value > best) { /* N_SECT */
			best = sym[i].n_value;
			best_name = strs + sym[i].n_strx;
		}
	}
	if (!best_name || addr - best >= 0x100000) {
		return NULL;
	}
	*base = best;
	return best_name;
}

static void out_sym(uint64_t addr)
{
	uint64_t base = 0;
	const char *name = sym_find(addr, &base);

	out_hex(addr);
	if (name) {
		out(" ");
		out(name);
		out("+");
		out_hex(addr - base);
	}
}

/* Shorter, for long lists: the symbol and the offset, or the address if there is none. */
static void out_fn(uint64_t addr)
{
	uint64_t base = 0;
	const char *name = sym_find(addr, &base);

	if (!name) {
		out_hex(addr);
		return;
	}
	out(name[0] == '_' ? name + 1 : name);
	if (addr != base) {
		out("+");
		out_hex(addr - base);
	}
}

static void show_backtrace(void)
{
	uint64_t fp = xnu_fp[0];

	out("TCR_EL1 ");
	out_hex(xnu_fp[2]);
	out(" TTBR1_EL1 ");
	out_hex(xnu_fp[3]);
	out(" fp ");
	out_hex(fp);
	out(" -> pa ");
	out_hex(xnu_va_to_pa(fp));
	out(" sym debugger_panic_str -> pa ");
	out_hex(xnu_va_to_pa(xsyms.panic_str));
	out(" (slide: ");
	out_hex(kva_to_pa(xsyms.panic_str));
	out(")\nbacktrace:\n  lr ");
	out_sym(xnu_fp[1]);
	out("\n");
	for (int i = 0; i < 24 && fp; i++) {
		uint64_t lr = xnu_rd64(fp + 8);
		if (!lr) {
			break;
		}
		out("  ");
		out_sym(lr);
		out("\n");
		fp = xnu_rd64(fp);
	}
}

/*
 * What XNU's text console shows, read back from the framebuffer: each 8x16 cell is looked up in
 * XNU's own font (iso_font, 16 bytes a character, bit 0 the leftmost pixel; video_console.c).
 * Proof that the text reached the screen, and a copy of it.
 */
static void read_screen(void)
{
	static uint32_t font_hash[256];
	uint64_t font = xsyms.iso_font ? kva_to_pa(xsyms.iso_font) : 0;
	const uint32_t cols = PANEL_W / 8, rows = PANEL_H / 16;
	uint32_t lit_cells = 0, known = 0, last_row = 0;
	static char line[PANEL_W / 8 + 1];

	if (!font || !fb_base) {
		return;
	}
	for (int ch = 0; ch < 256; ch++) {
		uint32_t h = 0;
		for (int r = 0; r < 16; r++) {
			h = h * 0x01000193u ^ *(volatile uint8_t *)(font + ch * 16 + r);
		}
		font_hash[ch] = h;
	}
	for (uint32_t row = 0; row < rows; row++) {
		for (uint32_t col = 0; col < cols; col++) {
			uint8_t bits[16];
			uint32_t h = 0, any = 0;
			for (uint32_t r = 0; r < 16; r++) {
				const volatile uint32_t *px = (const volatile uint32_t *)(fb_base + (uint64_t)(row * 16 + r) * fb_pitch) + col * 8;
				uint8_t b = 0;
				for (uint32_t x = 0; x < 8; x++) {
					uint32_t p = px[x];
					uint32_t lum = ((p >> 16) & 0xff) + ((p >> 8) & 0xff) + (p & 0xff);
					if (lum > 3 * 0x60) {
						b |= (uint8_t)(1u << x);
					}
				}
				bits[r] = b;
				any |= b;
			}
			char out_c = ' ';
			if (any) {
				lit_cells++;
				for (int inv = 0; inv < 2 && out_c == ' '; inv++) {
					h = 0;
					for (int r = 0; r < 16; r++) {
						h = h * 0x01000193u ^ (uint8_t)(inv ? ~bits[r] : bits[r]);
					}
					for (int ch = 0x21; ch < 0x7f; ch++) {
						if (font_hash[ch] == h) {
							out_c = (char)ch;
							known++;
							break;
						}
					}
				}
				if (out_c == ' ') {
					out_c = '#';
				}
			}
			line[col] = out_c;
		}
		line[cols] = 0;
		/* trailing spaces off; blank rows kept short */
		int n = (int)cols;
		while (n > 0 && line[n - 1] == ' ') {
			n--;
		}
		line[n] = 0;
		if (n) {
			last_row = row + 1;
		}
		dbg_puts(line);
		dbg_putc('\n');
	}
	dbg_puts("[screen: ");
	dbg_dec(lit_cells);
	dbg_puts(" cells lit, ");
	dbg_dec(known);
	dbg_puts(" read as characters, last text row ");
	dbg_dec(last_row);
	dbg_puts("]\n");
}

/*
 * XNU is back (start.S xnu_return_stub): MMU off, our vectors, our stack. Its log was
 * linearized and written back to RAM by XNU. Save it, show its end, then reset into Android.
 */
static void gk_replay(void);

void xnu_returned(uint64_t log_pa, uint64_t len, uint64_t reason, uint64_t code, uint64_t elr, uint64_t far)
{
	struct span sp = { (const volatile char *)log_pa, len };

	wdt_disable();
	xnu_back = 1;
	marker_set(MARK_DONE);
	uint64_t screen = fbc.base;
	fbc.base = 0; /* nothing drawn over XNU's console before it has been read */
	dbg_puts("\n=== what the screen showed when XNU came back ===\n");
	read_screen();
	fbc.base = screen;
	fbc.fg = 0xffffff40u;
	out("\n=== XNU returned to mtkloader: reason ");
	out_dec(reason);
	out(reason == 0 ? " (end)" : reason == 1 ? " (early exception)" : reason == 2 ? " (checkpoint)" : "");
	out(" code ");
	out_hex(code);
	if (reason == 1) {
		out(" ELR ");
		out_hex(elr);
		out(" FAR ");
		out_hex(far);
	}
	out("\n");
	if (core0_off) {
		out("core 0's CPU_OFF after the hand-over: ");
		if (core0_off == 1) {
			out("taken (core 0 went off)\n");
		} else {
			out("refused, answer -");
			out_dec(256 - (core0_off & 0xff));
			out(" (-1 not supported, -3 denied: a resident trusted OS?)\n");
		}
	}
	if (core_park[0] == CORE_PARK_MAGIC) {
		out("core 0, kept for the kernel (cpu_park): ");
		out(core_park[4] == 2 ? "the kernel sent it to its reset vector\n" : "the kernel never asked for it\n");
	}
	/* Did user space ever start? tasks_count is 1 while only the kernel's task exists. */
	if (xsyms.tasks_count) {
		out("tasks ");
		out_dec(rd32(kva_to_pa(xsyms.tasks_count)));
		if (xsyms.nprocs) {
			out(" processes ");
			out_dec(rd32(kva_to_pa(xsyms.nprocs)));
		}
		if (xsyms.initproc) {
			out(" initproc ");
			out_hex(*(volatile uint64_t *)kva_to_pa(xsyms.initproc));
		}
		out("\n");
	}
	if (reason != 1) {
		show_panic_string(&xsyms);
		show_backtrace();
	}
	out("=== XNU log (");
	out_dec(len);
	out(" bytes) ===\n");
	if (len > 4 * 1024 * 1024) {
		sp.n = 0;
	}
	save_log(&sp, 1);
	dbg_puts("=== end of XNU log ===\n");
	gk_replay();
	save_host_keep(&xsyms);
	show_log_tail(40);
	dbg_delay_ms(20000);
	reset_board();
}

/*
 * After a watchdog reset: what XNU had printed (its message buffer, through its own page tables:
 * cpu_ttep, 4K granule, 39-bit kernel addresses as TCR_EL1 had them), whether its clock ticked
 * (loader_live.heartbeat), and any panic string.
 */
static void rescue_state(void)
{
	if (xsyms.cpu_ttep) {
		xnu_fp[3] = rd32(kva_to_pa(xsyms.cpu_ttep)) | (uint64_t)rd32(kva_to_pa(xsyms.cpu_ttep) + 4) << 32;
		xnu_fp[2] = 0x22a51a251aULL;
	}
	out("cpu_ttep ");
	out_hex(xnu_fp[3]);
	if (xsyms.live) {
		out(" heartbeat ");
		out_dec(*(volatile uint64_t *)kva_to_pa(xsyms.live + 8));
	}
	out("\n");
	show_panic_string(&xsyms);
	if (!xsyms.msgbufp || !xnu_fp[3]) {
		return;
	}
	uint64_t mb = xnu_rd64(xsyms.msgbufp);
	uint64_t w0 = xnu_rd64(mb), w1 = xnu_rd64(mb + 8), bufc = xnu_rd64(mb + 16);
	int32_t size = (int32_t)(w0 >> 32), bufx = (int32_t)w1;
	out("msgbuf ");
	out_hex(mb);
	out(" size ");
	out_dec((uint32_t)size);
	out(" bufx ");
	out_dec((uint32_t)bufx);
	out("\n--- kernel message buffer (from DRAM) ---\n");
	if (size <= 0 || size > 1024 * 1024 || bufx < 0 || bufx > size) {
		return;
	}
	uint64_t fbsave = fbc.base;
	fbc.base = 0;
	for (int pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? bufx : 0, to = pass == 0 ? size : bufx;
		for (int i = from; i < to; i++) {
			char c = xnu_rd8(bufc + i);
			if (c) {
				dbg_putc(c);
			}
		}
	}
	fbc.base = fbsave;
	dbg_puts("\n--- end of message buffer ---\n");
}

/* The watchdog cut XNU short: its log is still in DRAM, at the addresses of its symbols. */
static void rescue_log(void)
{
	struct payload_info pi;
	struct xnu_syms s;

	out("\n=== mtkloader: XNU ran until the watchdog reset ===\n");
	if (!parse_payload(&pi)) {
		out("no XNU payload\n");
		return;
	}
	find_syms(pi.kernel, &s);
	if (!s.log || !s.log_len || !s.log_ring_at || !s.log_wrapped) {
		out("XNU log symbols not found\n");
		return;
	}
	/* osfmk/arm64/loader_return.c: a 256 KB head, then a 1 MB ring */
	const uint64_t head_sz = 256 * 1024, ring_sz = 1024 * 1024;
	const volatile char *log = (const volatile char *)kva_to_pa(s.log);
	uint32_t len = rd32(kva_to_pa(s.log_len));
	uint32_t ring_at = rd32(kva_to_pa(s.log_ring_at));
	uint8_t wrapped = *(const volatile uint8_t *)kva_to_pa(s.log_wrapped);
	struct span sp[3];
	int n = 0;

	out("loader_log_len ");
	out_dec(len);
	out(" ring_at ");
	out_dec(ring_at);
	out(" wrapped ");
	out_dec(wrapped);
	out("\n");
	if (ring_at > ring_sz) {
		ring_at = 0;
	}
	if (!wrapped) {
		if (len > head_sz + ring_sz) {
			len = head_sz + ring_sz;
		}
		sp[n++] = (struct span){ log, len };
	} else {
		sp[n++] = (struct span){ log, head_sz };
		sp[n++] = (struct span){ log + head_sz + ring_at, ring_sz - ring_at };
		sp[n++] = (struct span){ log + head_sz, ring_at };
	}
	xsyms = s;
	xkernel = pi.kernel;
	rescue_state();
	dbg_puts("=== XNU log (from DRAM; a few bits may have flipped in the reset) ===\n");
	save_log(sp, n);
	dbg_puts("=== end of XNU log ===\n");
	save_host_keep(&s);
	show_log_tail(40);
}

/*
 * Do the timers reach EL1 here? The virtual timer's PPI 27 is what XNU uses, and its interrupt
 * never arrives: the kernel idles for ever waiting for a root device that no timeout ever wakes.
 * Set up the GIC as XNU does, arm each timer in turn and see which one is taken.
 */
#define GICD_BASE   PLAT_GICD_BASE
#define GICR_SGI    (PLAT_GICR_BASE + 0x10000)

#if TIMER_PROBE
static void gic_cpu_prepare(void)
{
	uint64_t sre, ctlr;

	/*
	 * ICC_CTLR_EL1.EOImode: with it set, ICC_EOIR1_EL1 only drops the priority and the
	 * interrupt stays active until ICC_DIR_EL1 -- and XNU never writes that, so its first
	 * timer interrupt would also be its last. Clear it for whoever runs on this core next.
	 */
	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out("  ICC_CTLR_EL1 ");
	out_hex(ctlr);
	out(ctlr & 2 ? " (EOImode=1: EOI does not deactivate)" : " (EOImode=0)");
	__asm__ volatile("msr ICC_CTLR_EL1, %0; isb" : : "r"(ctlr & ~2ULL));
	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out(" -> ");
	out_hex(ctlr);
	out("\n");

	__asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
	__asm__ volatile("msr ICC_SRE_EL1, %0; isb" : : "r"(sre | 1));
	__asm__ volatile("msr ICC_AP1R0_EL1, xzr");
	__asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xffUL));
	__asm__ volatile("msr ICC_BPR1_EL1, xzr");
	__asm__ volatile("msr ICC_IGRPEN1_EL1, %0; isb" : : "r"(1UL));
}
#endif

/*
 * EOImode (ICC_CTLR_EL1 bit 1) is what ATF/GenieZone leaves set on this board: ICC_EOIR1_EL1
 * then only drops the running priority, and the interrupt stays *active* until a write to
 * ICC_DIR_EL1. XNU writes only EOIR, so with EOImode set it takes one timer interrupt and never
 * sees another: it idles for ever, waiting for timeouts that do not come. Clear it here, on the
 * core the kernel is about to run on.
 */
static void gic_fix_eoimode(void)
{
	uint64_t ctlr;

	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	out("ICC_CTLR_EL1 ");
	out_hex(ctlr);
	out(ctlr & 2 ? " (EOImode set: clearing)" : " (EOImode clear)");
	if (ctlr & 2) {
		__asm__ volatile("msr ICC_CTLR_EL1, %0; isb" : : "r"(ctlr & ~2ULL));
		__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
		out(" -> ");
		out_hex(ctlr);
	}
	out("\n");
}

#if TIMER_PROBE
static int timer_try(int ppi, int virt)
{
	uint64_t f = dbg_freq(), now, grp;
	uint32_t before;

	/* Group 1 NS, default priority, enabled: the same as XNU's gic_v3_irq_cpu_init(). */
	wr32(GICR_SGI + 0x080, rd32(GICR_SGI + 0x080) | (1u << ppi));
	wr32(GICR_SGI + 0xd00, rd32(GICR_SGI + 0xd00) & ~(1u << ppi));
	*(volatile uint8_t *)(GICR_SGI + 0x400 + ppi) = 0xa0;
	wr32(GICR_SGI + 0x100, 1u << ppi);
	grp = rd32(GICR_SGI + 0x080);
	wr32(GICR_SGI + 0x380, 1u << ppi); /* nothing of an earlier try left active */
	irq_count = 0;
	irq_intid = 0;

  again:
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
	if (virt) {
		__asm__ volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb" : : "r"(now + f / 10), "r"(1UL));
	} else {
		__asm__ volatile("msr cntp_cval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(now + f / 10), "r"(1UL));
	}
	before = rd32(GICR_SGI + 0x200);
	irq_wait(now + f / 2);

	out(virt ? "  virtual timer  PPI " : "  physical timer PPI ");
	out_dec((uint64_t)ppi);
	out(": IGROUPR0 ");
	out_hex(grp);
	out(grp & (1u << ppi) ? " (group 1)" : " (GROUP 0: not ours)");
	out(", ISPENDR0 ");
	out_hex(before);
	out("->");
	out_hex(rd32(GICR_SGI + 0x200));
	out(", ISACTIVER0 after EOI ");
	out_hex(rd32(GICR_SGI + 0x300));
	out(", irqs ");
	out_dec(irq_count);
	if (irq_count) {
		out(" intid ");
		out_dec(irq_intid);
	}
	out(", ISACTIVER0 ");
	out_hex(rd32(GICR_SGI + 0x300));
	out("\n");
	if (irq_count == 1) {
		goto again;     /* a second one proves the first was deactivated */
	}
	__asm__ volatile("msr cntv_ctl_el0, %0; msr cntp_ctl_el0, %0; isb" : : "r"(2UL));
	/* Leave this PPI as XNU must find it: disabled, not pending, not active. */
	wr32(GICR_SGI + 0x180, 1u << ppi); /* ICENABLER0 */
	wr32(GICR_SGI + 0x280, 1u << ppi); /* ICPENDR0   */
	wr32(GICR_SGI + 0x380, 1u << ppi); /* ICACTIVER0 */
	__asm__ volatile("dsb sy; isb");
	return irq_count != 0;
}

static void timer_probe(void)
{
	out("timer probe at EL1:\n");
	gic_cpu_prepare();
	timer_try(27, 1);
	timer_try(30, 0);
	/* and the CPU interface as it was, so nothing of ours is left in it */
	__asm__ volatile("msr ICC_IGRPEN1_EL1, xzr; msr ICC_AP1R0_EL1, xzr; isb");
	out("  GICR after cleanup: ISENABLER0 ");
	out_hex(rd32(GICR_SGI + 0x100));
	out(" ISPENDR0 ");
	out_hex(rd32(GICR_SGI + 0x200));
	out(" ISACTIVER0 ");
	out_hex(rd32(GICR_SGI + 0x300));
	out("\n");
}
#endif /* TIMER_PROBE */

/* ---- the watcher on the second core ---- */

static uint64_t watcher_l1[512] __attribute__((aligned(4096)));
static volatile uint64_t watcher_started __attribute__((aligned(64)));

static void plog_flush(void)
{
	if (RAMOOPS_CONSOLE) {
		dcache_clean_range(RAMOOPS_CONSOLE, RAMOOPS_CONSOLE_SIZE);
	}
	dcache_clean_range(MARKER_PA, 192);
}

/*
 * XNU never came back (no timer, a hang): with its caches visible through our own cacheable
 * mapping, take what it shows and has written, put it into ramoops and reset the board.
 */
/*
 * ---- the watcher's look at XNU's processors and threads, from its memory ----
 *
 * Run 17 and 18 stopped with no panic and with the heartbeat stuck, and the PC sampling above reads the
 * debug blocks of the MT6768's core 0, not the one XNU runs on. This reads, through XNU's own page
 * tables, what each processor was doing and where every thread was waiting.
 *
 * The offsets are this kernel's (tree a962798, DEVELOPMENT_ARM64 BCM2837), read off its disassembly:
 * machine_set_current_thread and Switch_context (cpu_data +0x30, thread +0x158 +0x1c8), machine_stack_attach
 * (+0xe8), thread_continue (+0xd8), waitq_assert_wait64_locked (+0x20), thread_mark_wait_locked (+0x1f0),
 * thread_create_internal (+0x400), get_threadtask (+0x420, +0x20), thread_tid (+0x4e8), get_bsdthread_info
 * and bsd_getthreadname (+0x580 +0x2a8), proc_selfname (+0x5c9), rtclock_intr and ml_set_decrementer (cpu_data
 * +0x6c +0xa8 +0xc8 +0x108 +0x238). Another kernel needs them checked again; whatever they read is bounded.
 */
#define XT_WAIT_EVENT   0x20
#define XT_CONT         0xd8
#define XT_KSTACK       0xe8
#define XT_KSTACKPTR    0x158           /* the context saved at the last switch: fp +0x50, lr +0x58, sp +0x60 */
#define XT_CPUDATA      0x1c8           /* the processor's cpu_data while the thread is on one, else 0 */
#define XT_STATE        0x1f0
#define XT_LINKS        0x400           /* the global list of threads: next, prev */
#define XT_TRO          0x420           /* thread_ro: +0 the thread, +0x20 its task */
#define XT_TID          0x4e8
#define XT_PTH_NAME     (0x580 + 0x2a8) /* the uthread after the thread: its name */
#define XP_NAME         0x5c9
#define XC_NUMBER       0x0
#define XC_ACTIVE       0x30
#define XC_SIGNAL       0x48            /* (and cpu_pending_ast at +0x4c) */
#define XC_BASE_TB      0x58
#define XC_DECR         0x68            /* (cpu_decrementer at +0x6c) */
#define XC_IDLE_POP     0xa8
#define XC_INT_STATE    0xc8            /* while in an interrupt, what it interrupted: fp +0xf0, lr +0xf8, sp +0x100, pc +0x108 */
#define XC_RTCPOP       0x108
#define XC_TIMER_CNT    0x238

static uint64_t xtext_lo, xtext_hi;     /* the kernel's code (__TEXT_EXEC) */

static void xnu_text_range(void)
{
	const struct mach_header_64 *mh = (const struct mach_header_64 *)xkernel;
	const struct load_command *lc;

	if (!xkernel) {
		return;
	}
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (strcmp(sg->segname, "__TEXT_EXEC") == 0) {
				xtext_lo = sg->vmaddr;
				xtext_hi = sg->vmaddr + sg->vmsize;
			}
		}
	}
}

static int is_text(uint64_t a)
{
	return xtext_lo && a >= xtext_lo && a < xtext_hi && !(a & 3);
}

/* a name from XNU's memory, printable characters only (what the offsets point at may be wrong) */
static void out_xnu_name(uint64_t va, int max)
{
	for (int i = 0; i < max; i++) {
		char c = xnu_rd8(va + i);
		if (c == 0) {
			break;
		}
		out_putc(c >= 0x20 && c < 0x7f ? c : '.');
	}
}

static uint64_t xnu_stack_size(void)
{
	uint64_t z = xsyms.kernel_stack_size ? xnu_rd64(xsyms.kernel_stack_size) : 0;
	return z >= 0x4000 && z <= 0x40000 ? z : 0x10000;
}

/* "proc/thread name" of a thread */
static void out_thread_name(uint64_t t)
{
	uint64_t tro = xnu_rd64(t + XT_TRO), task = 0, proc = 0, pss, name;

	if (tro && xnu_rd64(tro) == t) {
		task = xnu_rd64(tro + 0x20);
	}
	pss = xsyms.proc_struct_size ? xnu_rd64(xsyms.proc_struct_size) : 0;
	if (task && pss && pss < 0x4000) {
		proc = task - pss;
	}
	if (proc) {
		out_xnu_name(proc + XP_NAME, 17);
	} else {
		out("?");
	}
	name = xnu_rd64(t + XT_PTH_NAME);
	if (name) {
		out("/");
		out_xnu_name(name, 40);
	}
}

static int boring_frame(uint64_t lr)
{
	static const char *const names[] = {
		"_machine_switch_context", "_thread_invoke", "_thread_block_reason", "_thread_block",
		"_thread_block_parameter",
	};
	uint64_t base = 0;
	const char *n = sym_find(lr, &base);

	for (unsigned i = 0; n && i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcmp(n, names[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

/* The frame records from fp on, as long as they stay in the stack [lo, hi) and go up. */
static void out_frames(uint64_t fp, uint64_t lo, uint64_t hi, int most)
{
	int shown = 0;

	for (int i = 0; i < 32 && shown < most; i++) {
		if (fp < lo || fp + 16 > hi || (fp & 7)) {
			break;
		}
		uint64_t next = xnu_rd64(fp), lr = xnu_rd64(fp + 8);
		if (!is_text(lr)) {
			break;
		}
		if (!(shown == 0 && boring_frame(lr))) {
			out(" < ");
			out_fn(lr);
			shown++;
		}
		if (next <= fp) {
			break;
		}
		fp = next;
	}
}

/*
 * A thread that is running has no saved context to start from: its stack is searched for the
 * deepest frame record (fp, lr) whose chain goes up to the top. That may be a frame that has already
 * returned (what was below the stack pointer is not cleared), so the first ones can be stale.
 */
static void out_running_stack(uint64_t ks, uint64_t size)
{
	uint64_t top = ks + size;

	for (uint64_t a = ks; a + 16 <= top; a += 8) {
		uint64_t fp = xnu_rd64(a), lr = xnu_rd64(a + 8);
		if (!is_text(lr) || fp <= a || fp >= top || (fp & 7)) {
			continue;
		}
		/* does it go up to near the top in a few frames? */
		uint64_t f = a;
		int n = 0;
		while (n < 40 && f + 16 <= top && is_text(xnu_rd64(f + 8))) {
			uint64_t nf = xnu_rd64(f);
			n++;
			if (nf <= f || nf >= top) {
				break;
			}
			f = nf;
		}
		if (n >= 3 && top - f < 0x400) {
			out("    stack from +");
			out_hex(a - ks);
			out(" (");
			out_dec((uint64_t)n);
			out(" frames):\n     ");
			out_frames(a, ks, top, 24);
			out("\n");
			return;
		}
	}
	out("    no frame chain found in its stack\n");
}

static void watch_xnu_cpus(void)
{
	uint64_t cd[8] = { 0 }, cnt[8] = { 0 }, freq = dbg_freq(), now;
	uint64_t ssz = xnu_stack_size();

	if (!xsyms.cpu_data_entries || !xnu_fp[3]) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		cd[i] = xnu_rd64(xsyms.cpu_data_entries + (uint64_t)i * 16 + 8);
		if (cd[i]) {
			cnt[i] = xnu_rd64(cd[i] + XC_TIMER_CNT);
		}
	}
	dbg_delay_ms(1000);
	now = dbg_ticks();
	out("\n--- XNU's processors (counter now ");
	out_dec(now / (freq / 1000));
	out(" ms) ---\n");
	for (int i = 0; i < 8; i++) {
		if (!cd[i]) {
			continue;
		}
		uint64_t t = xnu_rd64(cd[i] + XC_ACTIVE), sig = xnu_rd64(cd[i] + XC_SIGNAL);
		uint64_t decr = xnu_rd64(cd[i] + XC_DECR), pop = xnu_rd64(cd[i] + XC_RTCPOP), base = xnu_rd64(cd[i] + XC_BASE_TB);
		uint64_t idle_pop = xnu_rd64(cd[i] + XC_IDLE_POP), is = xnu_rd64(cd[i] + XC_INT_STATE);
		uint64_t c2 = xnu_rd64(cd[i] + XC_TIMER_CNT);

		out("cpu");
		out_dec((uint64_t)(uint16_t)xnu_rd64(cd[i] + XC_NUMBER));
		out(": timer interrupts ");
		out_dec(c2);
		out(" (+");
		out_dec(c2 - cnt[i]);
		out(" in the last second); next timer pop at ");
		out_dec((pop - base) / (freq / 1000));
		out(" ms");
		if (pop == ~0ULL || pop == 0x7fffffffffffffffULL) {
			out(" (none)");
		} else if (pop - base < now) {
			out(" (");
			out_dec((now - (pop - base)) / (freq / 1000));
			out(" ms ago)");
		}
		out("; decrementer ");
		out_hex(decr >> 32);
		out(" idle_pop ");
		out_hex(idle_pop);
		out(" signal ");
		out_hex(sig & 0xffffffff);
		out(" ast ");
		out_hex(sig >> 32);
		out("\n  running: tid ");
		out_dec(t ? xnu_rd64(t + XT_TID) : 0);
		out(" ");
		if (t) {
			out_thread_name(t);
		}
		out(" state ");
		out_hex(t ? xnu_rd64(t + XT_STATE) & 0xffffffff : 0);
		out("\n");
		if (is) {
			out("  in an interrupt, of pc ");
			out_sym(xnu_rd64(is + 0x108));
			out(" lr ");
			out_fn(xnu_rd64(is + 0xf8));
			out(" cpsr ");
			out_hex(xnu_rd64(is + 0x110) & 0xffffffff);
			out("\n     ");
			out_frames(xnu_rd64(is + 0xf0), xnu_rd64(is + 0x100) & ~0xfffULL, (xnu_rd64(is + 0x100) & ~0xfffULL) + ssz, 16);
			out("\n");
		}
		if (t && xnu_rd64(t + XT_KSTACK)) {
			out_running_stack(xnu_rd64(t + XT_KSTACK), ssz);
		}
	}
}

static void watch_xnu_threads(uint32_t room_end)
{
	uint64_t head = xsyms.threads, t, ssz = xnu_stack_size();
	uint32_t n = 0;

	if (!head || !xnu_fp[3]) {
		return;
	}
	out("\n--- XNU's threads (");
	out_dec(xsyms.threads_count ? xnu_rd64(xsyms.threads_count) & 0xffffffff : 0);
	out("; state W wait, S suspended, R run, U uninterruptible, T terminating, I idle) ---\n");
	for (t = xnu_rd64(head); t && t != head && n < 3000; t = xnu_rd64(t + XT_LINKS), n++) {
		uint32_t st = (uint32_t)xnu_rd64(t + XT_STATE);
		uint64_t cpu = xnu_rd64(t + XT_CPUDATA), cont = xnu_rd64(t + XT_CONT), ks = xnu_rd64(t + XT_KSTACK);
		static const char flags[] = "WSRUT??I";

		if (PLOG_HDR->size > room_end) {
			out("[... the other threads left out: no room ...]\n");
			break;
		}
		out_dec(xnu_rd64(t + XT_TID));
		out(" ");
		for (int b = 0; b < 8; b++) {
			if (st & (1u << b)) {
				out_putc(flags[b]);
			}
		}
		if (cpu) {
			out(" ON cpu");
			out_dec((uint64_t)(uint16_t)xnu_rd64(cpu + XC_NUMBER));
		}
		out(" ");
		out_thread_name(t);
		if (st & 1) {
			out(" ev ");
			out_hex(xnu_rd64(t + XT_WAIT_EVENT));
		}
		if (cont && is_text(cont)) {
			out(" cont ");
			out_fn(cont);
		} else if (ks && !cpu) {
			uint64_t ctx = xnu_rd64(t + XT_KSTACKPTR);
			if (ctx >= ks && ctx < ks + ssz) {
				uint64_t lr = xnu_rd64(ctx + 0x58);
				if (!boring_frame(lr)) {
					out(" < ");
					out_fn(lr);
				}
				out_frames(xnu_rd64(ctx + 0x50), ks, ks + ssz, 9);
			}
		}
		out("\n");
	}
}

/* The GIC's view of every core: is the virtual timer (PPI 27) pending or active where XNU runs? */
static void watch_gic(void)
{
	out("GICD_CTLR ");
	out_hex(rd32(PLAT_GICD_BASE));
	out(" TYPER ");
	out_hex(rd32(PLAT_GICD_BASE + 0x4));
	out(" IIDR ");
	out_hex(rd32(PLAT_GICD_BASE + 0x8));
	out("\nGICR (each core): affinity, WAKER, PWRR, IGROUPR0, ISENABLER0, ISPENDR0, ISACTIVER0 -- bit 27 is the virtual timer\n");
	for (int f = 0; f < 8; f++) {
		uint64_t r = PLAT_GICR_BASE + (uint64_t)f * 0x20000, sgi = r + 0x10000;
		uint32_t typer = rd32(r + 0x8);
		out("  ");
		out_hex(rd32(r + 0xc));
		out(" ");
		out_hex(rd32(r + 0x14));
		out(" ");
		out_hex(rd32(r + 0x24));
		out(" ");
		out_hex(rd32(sgi + 0x80));
		out(" ");
		out_hex(rd32(sgi + 0x100));
		out(" ");
		out_hex(rd32(sgi + 0x200));
		out(" ");
		out_hex(rd32(sgi + 0x300));
		out("\n");
		if (typer & 0x10) {     /* Last */
			break;
		}
	}
}

/*
 * ---- run 20: the GIC, watched while XNU runs ----
 *
 * Run 19 (cpus=1): at 27.33 s the virtual timer stayed pending in the boot core's redistributor (ISPENDR0 bit 27,
 * enabled, nothing active) and was never taken again. XNU's processor spun in its idle loop, the presenter waited
 * for its beat and lsd held the UFS lock in a delay() for good. Runs 17 and 18 had read GICD_CTLR 0x10 in their
 * snapshots: affinity routing on, Group 1 off -- which XNU's gic_v3_irq_cpu_init turns on, and nothing in XNU
 * writes again. So, every quarter of a millisecond while XNU runs, this looks at the distributor and at the boot
 * core's redistributor and logs each change with the counter's time (the one XNU's own log uses). When XNU's timer
 * is overdue it logs what the GIC holds; and if Group 1 is off, it turns it back on and says whether the timer
 * comes back (after the first time: at once, with one line).
 *
 * All of it goes to ramoops only (no screen, no UART), and the watcher writes no variable that the boot core
 * also writes: XNU's return stub stores into xnu_fp and the log with the MMU off, so a line of those dirty in
 * this core's cache could later be written back over them. Its own state is one block of whole cache lines.
 */
#define GK_N            8
#define GK_MAX_BYTES    (16 * 1024)     /* of ramoops: the snapshot after it needs the rest */
#define GK_FULL_REPORTS 3
#define GK_MAX_FIXES    200
#define GK_MAX_CHANGES  60

struct gk_state {
	uint64_t frame;                 /* the boot core's redistributor */
	uint64_t tcr, ttbr;             /* XNU's tables, once cpu_ttep is set */
	uint64_t cd;                    /* XNU's processor 0 (cpu_data), virtual */
	uint64_t pa_cnt, pa_pop, pa_base;
	uint64_t cnt, cnt_moved_at;     /* its timer interrupts, and when the count last changed (ticks) */
	uint64_t next_attach, next_beat, check_at, cnt_at_fix;
	uint64_t bytes;
	uint32_t seen[GK_N];
	uint32_t gicd[1024];            /* the distributor's first 4 KB, as last known good */
	uint32_t have_gicd, changes, fixes, reports, stalled, g1_seen, mute;
	char text[GK_MAX_BYTES];        /* what it wrote, again: xnu_returned() repeats it after XNU's log, which wraps ramoops */
} __attribute__((aligned(64)));

static struct gk_state gk;

static const char *const gk_names[GK_N] = {
	"GICD_CTLR", "GICR_CTLR", "GICR_WAKER", "GICR_PWRR", "IGROUPR0", "IGRPMODR0", "ISENABLER0", "IPRIORITYR27",
};

static void gk_putc(char c)
{
	if (!gk.mute && gk.bytes < GK_MAX_BYTES) {
		plog_putc(c);
		gk.text[gk.bytes++] = c;
	}
}

static void gk_puts(const char *s)
{
	while (*s) {
		gk_putc(*s++);
	}
}

static void gk_hex(uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i = 60;

	gk_puts("0x");
	while (i > 0 && !((v >> i) & 0xf)) {
		i -= 4;
	}
	for (; i >= 0; i -= 4) {
		gk_putc(hex[(v >> i) & 0xf]);
	}
}

static void gk_dec(uint64_t v)
{
	char buf[21];
	int n = 0;

	do {
		buf[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n) {
		gk_putc(buf[--n]);
	}
}

/* "[  27.329]": seconds of the counter, as in XNU's log */
static void gk_time(uint64_t ticks)
{
	uint64_t ms = ticks / (dbg_freq() / 1000);
	uint64_t s = ms / 1000, f = ms % 1000;

	gk_putc('[');
	for (uint64_t p = 1000; p > 1 && s < p; p /= 10) {
		gk_putc(' ');
	}
	gk_dec(s);
	gk_putc('.');
	gk_putc((char)('0' + f / 100));
	gk_putc((char)('0' + f / 10 % 10));
	gk_putc((char)('0' + f % 10));
	gk_puts("] keeper: ");
}

/*
 * Around each burst of lines: nothing of ramoops stale in this core's cache before, nothing dirty after (and its
 * own state in RAM, for xnu_returned() on the boot core, MMU off). Once XNU is back the loader writes there: quiet.
 */
static void gk_begin(void)
{
	dcache_clean_range((uint64_t)&xnu_back, 8);
	if (xnu_back) {
		gk.mute = 1;
	}
	if (RAMOOPS_CONSOLE) {
		dcache_clean_range(RAMOOPS_CONSOLE, RAMOOPS_CONSOLE_SIZE);
	}
}

static void gk_end(void)
{
	plog_flush();
	dcache_clean_range((uint64_t)&gk, sizeof(gk));
}

/* (on the boot core, after XNU's log) */
static void gk_replay(void)
{
	uint64_t n = gk.bytes < GK_MAX_BYTES ? gk.bytes : GK_MAX_BYTES;

	if (!n) {
		return;
	}
	dbg_puts("=== the watcher's lines on the GIC, again ===\n");
	for (uint64_t i = 0; i < n; i++) {
		plog_putc(gk.text[i]);
	}
	dbg_puts("=== end of the watcher's lines ===\n");
}

static uint64_t gk_rd64(uint64_t va)
{
	uint64_t pa = gk.ttbr ? xnu_va_to_pa_in(va, gk.tcr, gk.ttbr) : 0;

	return pa && !(pa & 7) ? *(volatile uint64_t *)pa : 0;
}

static char gk_rd8(uint64_t va)
{
	uint64_t pa = gk.ttbr ? xnu_va_to_pa_in(va, gk.tcr, gk.ttbr) : 0;

	return pa ? *(volatile char *)pa : 0;
}

static void gk_name(uint64_t va, int max)
{
	for (int i = 0; i < max; i++) {
		char c = gk_rd8(va + (uint64_t)i);
		if (c == 0) {
			break;
		}
		gk_putc(c >= 0x20 && c < 0x7f ? c : '?');
	}
}

/* the thread processor 0 runs: "tid 245 lsd" */
static void gk_running(void)
{
	uint64_t t = gk.cd ? gk_rd64(gk.cd + XC_ACTIVE) : 0, tro, task = 0, pss, name;

	gk_puts("running tid ");
	gk_dec(t ? gk_rd64(t + XT_TID) : 0);
	if (!t) {
		return;
	}
	gk_putc(' ');
	tro = gk_rd64(t + XT_TRO);
	if (tro && gk_rd64(tro) == t) {
		task = gk_rd64(tro + 0x20);
	}
	pss = xsyms.proc_struct_size ? gk_rd64(xsyms.proc_struct_size) : 0;
	if (task && pss && pss < 0x4000) {
		gk_name(task - pss + XP_NAME, 17);
	} else {
		gk_putc('?');
	}
	name = gk_rd64(t + XT_PTH_NAME);
	if (name) {
		gk_putc('/');
		gk_name(name, 40);
	}
}

static void gk_read(uint32_t v[GK_N])
{
	uint64_t r = gk.frame, sgi = r + 0x10000;

	v[0] = rd32(PLAT_GICD_BASE) & ~(1u << 31);     /* (not RWP) */
	v[1] = rd32(r) & ~(1u << 3);
	v[2] = rd32(r + 0x14);
	v[3] = rd32(r + 0x24);                          /* GIC-600's power register; RES0 elsewhere */
	v[4] = rd32(sgi + 0x80);
	v[5] = rd32(sgi + 0xd00);
	v[6] = rd32(sgi + 0x100);
	v[7] = rd32(sgi + 0x418) >> 24;
}

/* the distributor's registers that hold settings (not pending or active, which change all the time) */
static int gk_gicd_word(uint32_t i)
{
	uint32_t off = i * 4;

	return off >= 4 && off < 0xf00 && !(off >= 0x200 && off < 0x400);
}

static void gk_gicd_take(void)
{
	for (uint32_t i = 0; i < 1024; i++) {
		gk.gicd[i] = gk_gicd_word(i) ? rd32(PLAT_GICD_BASE + i * 4) : 0;
	}
	gk.have_gicd = 1;
}

static void gk_gicd_diff(void)
{
	uint32_t n = 0;

	if (!gk.have_gicd) {
		return;
	}
	gk_puts("  distributor settings changed since Group 1 was last seen on (offset: then -> now):");
	for (uint32_t i = 0; i < 1024; i++) {
		uint32_t v;

		if (!gk_gicd_word(i) || (v = rd32(PLAT_GICD_BASE + i * 4)) == gk.gicd[i]) {
			continue;
		}
		if (n++ < 40) {
			gk_puts(n % 4 == 1 ? "\n   " : "  ");
			gk_hex(i * 4);
			gk_puts(": ");
			gk_hex(gk.gicd[i]);
			gk_puts(" -> ");
			gk_hex(v);
		}
	}
	gk_puts(n ? "\n   " : " none\n");
	if (n) {
		gk_dec(n);
		gk_puts(" words in all\n");
	}
}

/* the last `n` characters XNU has put into its message buffer */
static void gk_msgbuf_tail(int32_t n)
{
	uint64_t mb = xsyms.msgbufp ? gk_rd64(xsyms.msgbufp) : 0;

	if (!mb) {
		return;
	}
	uint64_t w0 = gk_rd64(mb), w1 = gk_rd64(mb + 8), bufc = gk_rd64(mb + 16);
	int32_t size = (int32_t)(w0 >> 32), bufx = (int32_t)w1;

	if (size <= 0 || size > 1024 * 1024 || bufx < 0 || bufx > size || !bufc) {
		return;
	}
	gk_puts("  XNU's message buffer ends:\n");
	for (int32_t k = size - n; k < size; k++) {
		char c = gk_rd8(bufc + (uint64_t)((bufx + k) % size));
		if (c) {
			gk_putc(c);
		}
	}
	gk_puts("\n");
}

static void gk_report(uint64_t now, uint64_t due)
{
	uint64_t r = gk.frame, sgi = r + 0x10000, ms = dbg_freq() / 1000;
	uint32_t lines = rd32(PLAT_GICD_BASE + 0x4) & 0x1f;

	gk_time(now);
	gk_puts("XNU's timer was due at ");
	gk_dec(due / ms);
	gk_puts(" ms and has not been taken; ");
	gk_dec(gk.cnt);
	gk_puts(" timer interrupts, the last new one seen at ");
	gk_dec(gk.cnt_moved_at / ms);
	gk_puts(" ms; ");
	gk_running();
	gk_puts("\n  GICD_CTLR ");
	gk_hex(rd32(PLAT_GICD_BASE));
	gk_puts(" IIDR ");
	gk_hex(rd32(PLAT_GICD_BASE + 0x8));
	gk_puts("; boot core's GICR: CTLR ");
	gk_hex(rd32(r));
	gk_puts(" WAKER ");
	gk_hex(rd32(r + 0x14));
	gk_puts(" PWRR ");
	gk_hex(rd32(r + 0x24));
	gk_puts(" IGROUPR0 ");
	gk_hex(rd32(sgi + 0x80));
	gk_puts(" IGRPMODR0 ");
	gk_hex(rd32(sgi + 0xd00));
	gk_puts(" ISENABLER0 ");
	gk_hex(rd32(sgi + 0x100));
	gk_puts(" ISPENDR0 ");
	gk_hex(rd32(sgi + 0x200));
	gk_puts(" ISACTIVER0 ");
	gk_hex(rd32(sgi + 0x300));
	gk_puts(" IPRIORITYR27 ");
	gk_hex(rd32(sgi + 0x418) >> 24);
	gk_puts("\n  SPIs pending or active (word: enabled pending active group):");
	for (uint32_t w = 1; w <= lines && w < 32; w++) {
		uint32_t en = rd32(PLAT_GICD_BASE + 0x100 + w * 4), pe = rd32(PLAT_GICD_BASE + 0x200 + w * 4);
		uint32_t ac = rd32(PLAT_GICD_BASE + 0x300 + w * 4), gr = rd32(PLAT_GICD_BASE + 0x80 + w * 4);
		if (!pe && !ac) {
			continue;
		}
		gk_puts("\n   ");
		gk_dec(w);
		gk_puts(": ");
		gk_hex(en);
		gk_puts(" ");
		gk_hex(pe);
		gk_puts(" ");
		gk_hex(ac);
		gk_puts(" ");
		gk_hex(gr);
	}
	gk_puts("\n");
	if (xsyms.gic_unhandled_count) {
		gk_puts("  kernel: gic_unhandled_count ");
		gk_dec(*(volatile uint64_t *)kva_to_pa(xsyms.gic_unhandled_count));
		gk_puts(" last_unhandled ");
		gk_dec(rd32(kva_to_pa(xsyms.gic_last_unhandled)));
		gk_puts(" active_iar[0] ");
		gk_hex(*(volatile uint64_t *)kva_to_pa(xsyms.gic_active_iar));
		gk_puts("\n");
	}
	gk_gicd_diff();
	gk_msgbuf_tail(600);
}

/* Group 1 back on in the distributor, as gic_v3_irq_cpu_init does it */
static void gk_fix(uint64_t now)
{
	uint32_t was = rd32(PLAT_GICD_BASE);

	wr32(PLAT_GICD_BASE, (was & ~(1u << 31)) | (1u << 1) | (1u << 4));
	for (int i = 0; i < 10000 && (rd32(PLAT_GICD_BASE) & (1u << 31)); i++) {
	}
	gk.fixes++;
	gk.seen[0] = rd32(PLAT_GICD_BASE) & ~(1u << 31);
	if (gk.fixes <= 20) {
		gk_time(now);
		gk_puts("GICD_CTLR ");
		gk_hex(was);
		gk_puts(" -> ");
		gk_hex(gk.seen[0]);
		gk_puts(": Group 1 turned back on (time ");
		gk_dec(gk.fixes);
		gk_puts(")\n");
	}
	gk.check_at = now + dbg_freq();
	gk.cnt_at_fix = gk.cnt;
}

/*
 * Nothing is written to ramoops before XNU has its page tables (cpu_ttep): until then the loader on the other cores
 * may still be writing to it, with its MMU off, and two writers would garble it. The values at the watcher's start
 * are kept and the first line says what they were; changes are compared from then on.
 */
static void gk_attach(uint64_t now)
{
	uint64_t ttbr, cd;

	if (gk.pa_cnt || now < gk.next_attach || !xsyms.cpu_ttep || !xsyms.cpu_data_entries) {
		return;
	}
	gk.next_attach = now + dbg_freq() / 100;
	if (!gk.ttbr) {
		ttbr = *(volatile uint64_t *)kva_to_pa(xsyms.cpu_ttep);
		if (!ttbr || !in_xnu_mem(ttbr & 0x0000fffffffff000ULL)) {
			return;
		}
		gk.ttbr = ttbr;
		gk.tcr = 0x22a51a251aULL;
		gk_begin();
		gk_time(now);
		gk_puts("XNU has its page tables; watching the GIC (cpu");
		gk_dec(XNU_BOOT_CORE);
		gk_puts("'s redistributor at ");
		gk_hex(gk.frame);
		gk_puts("), at the watcher's start:");
		for (int i = 0; i < GK_N; i++) {
			gk_puts(" ");
			gk_puts(gk_names[i]);
			gk_puts(" ");
			gk_hex(gk.seen[i]);
		}
		gk_puts("\n");
		gk_end();
	}
	cd = *(volatile uint64_t *)kva_to_pa(xsyms.cpu_data_entries + 8);
	if (!cd) {
		return;
	}
	gk.cd = cd;
	gk.pa_pop = xnu_va_to_pa_in(cd + XC_RTCPOP, gk.tcr, gk.ttbr);
	gk.pa_base = xnu_va_to_pa_in(cd + XC_BASE_TB, gk.tcr, gk.ttbr);
	gk.pa_cnt = xnu_va_to_pa_in(cd + XC_TIMER_CNT, gk.tcr, gk.ttbr);
	if (!gk.pa_pop || !gk.pa_base || !gk.pa_cnt || ((gk.pa_pop | gk.pa_base | gk.pa_cnt) & 7)) {
		gk.pa_cnt = 0;
		return;
	}
	gk_begin();
	gk_time(now);
	gk_puts("XNU's processor 0 found (cpu_data ");
	gk_hex(cd);
	gk_puts(")\n");
	gk_end();
}

static void gk_init(void)
{
	memset(&gk, 0, sizeof(gk));
	gk.frame = PLAT_GICR_BASE + (uint64_t)XNU_BOOT_CORE * 0x20000;
	for (int f = 0; f < 8; f++) {
		uint64_t r = PLAT_GICR_BASE + (uint64_t)f * 0x20000;
		if (rd32(r + 0xc) == (uint32_t)XNU_BOOT_CORE << 8) {
			gk.frame = r;
			break;
		}
		if (rd32(r + 0x8) & 0x10) {
			break;
		}
	}
	gk_read(gk.seen);
	gk.g1_seen = (gk.seen[0] >> 1) & 1;
	gk_gicd_take();
	gk.next_beat = dbg_ticks() + 20 * dbg_freq();
}

static void gk_step(void)
{
	uint64_t now = dbg_ticks(), ms = dbg_freq() / 1000;
	uint32_t v[GK_N];
	int wrote = 0;

	gk_attach(now);
	if (!gk.ttbr) {
		return;
	}
	gk_read(v);
	for (int i = 0; i < GK_N; i++) {
		if (v[i] == gk.seen[i]) {
			continue;
		}
		if (!wrote) {
			gk_begin();
			wrote = 1;
		}
		if (gk.changes++ < GK_MAX_CHANGES) {
			gk_time(now);
			gk_puts(gk_names[i]);
			gk_puts(" ");
			gk_hex(gk.seen[i]);
			gk_puts(" -> ");
			gk_hex(v[i]);
			gk_puts("; ");
			gk_running();
			gk_puts("\n");
		}
		if (i == 0 && (v[0] & 2) && !(gk.seen[0] & 2)) {
			gk.g1_seen = 1;
			gk_gicd_take();         /* (Group 1 on: what the distributor is like now is the good state) */
		}
		if (i == 0 && !(v[0] & 2) && (gk.seen[0] & 2) && gk.changes <= GK_MAX_CHANGES) {
			gk_gicd_diff();
		}
		gk.seen[i] = v[i];
	}

	if (gk.pa_cnt) {
		uint64_t c = *(volatile uint64_t *)gk.pa_cnt;

		if (c != gk.cnt) {
			if (gk.stalled) {
				if (!wrote) {
					gk_begin();
					wrote = 1;
				}
				if (gk.reports <= GK_FULL_REPORTS + 5) {
					gk_time(now);
					gk_puts("XNU's timer interrupts again (");
					gk_dec(c);
					gk_puts(")\n");
				}
				gk.stalled = 0;
			}
			gk.cnt = c;
			gk.cnt_moved_at = now;
		} else if (!gk.stalled && c != 0) {
			uint64_t pop = *(volatile uint64_t *)gk.pa_pop, base = *(volatile uint64_t *)gk.pa_base;
			uint64_t due = pop - base;

			if (pop != ~0ULL && pop != 0x7fffffffffffffffULL && pop > base && now > due + 100 * ms &&
			    now > gk.cnt_moved_at + 100 * ms) {
				gk.stalled = 1;
				if (!wrote) {
					gk_begin();
					wrote = 1;
				}
				if (gk.reports++ < GK_FULL_REPORTS) {
					gk_report(now, due);
				} else {
					gk_time(now);
					gk_puts("XNU's timer overdue again; GICD_CTLR ");
					gk_hex(rd32(PLAT_GICD_BASE));
					gk_puts("\n");
				}
				if (!(v[0] & 2) && gk.g1_seen && gk.fixes < GK_MAX_FIXES) {
					gk_fix(now);
				} else if (gk.reports <= GK_FULL_REPORTS) {
					gk_puts("  Group 1 is on (or never was): nothing tried\n");
				}
			}
		}
	}
	/* after the first time it was proven, Group 1 goes back on as soon as it is seen off */
	if (gk.fixes && !(gk.seen[0] & 2) && gk.fixes < GK_MAX_FIXES) {
		if (!wrote) {
			gk_begin();
			wrote = 1;
		}
		gk_fix(now);
	}
	if (gk.check_at && now >= gk.check_at) {
		if (!wrote) {
			gk_begin();
			wrote = 1;
		}
		if (gk.fixes <= 20) {
			gk_time(now);
			gk_puts("in the second after: ");
			gk_dec(gk.cnt - gk.cnt_at_fix);
			gk_puts(" timer interrupts; ");
			gk_running();
			gk_puts("\n");
		}
		gk.check_at = 0;
	}
	if (now >= gk.next_beat) {
		if (!wrote) {
			gk_begin();
			wrote = 1;
		}
		gk_time(now);
		gk_puts("timer interrupts ");
		gk_dec(gk.cnt);
		gk_puts(", GICD_CTLR ");
		gk_hex(gk.seen[0]);
		gk_puts(", Group 1 turned back on ");
		gk_dec(gk.fixes);
		gk_puts(" times; ");
		gk_running();
		gk_puts("\n");
		gk.next_beat = now + 20 * dbg_freq();
	}
	if (wrote) {
		gk_end();
	}
}

void watcher_main(void)
{
	for (int i = 0; i < 512; i++) {
		uint64_t pa = (uint64_t)i << 30;
		if (i == 0) {
			watcher_l1[i] = pa | (1u << 10) | (0u << 2) | 1;            /* MMIO: device */
		} else if (pa < PLAT_DRAM_TOP) {
			/* DRAM: normal WB, inner shareable. The MT6768 loader mapped 1-8 GB; here XNU's window and the marker are above
			 * 8 GB (platform.h), so the map goes to the top of DRAM. */
			watcher_l1[i] = pa | (1u << 10) | (3u << 8) | (1u << 2) | 1;
		} else {
			watcher_l1[i] = 0;
		}
	}
	mmu_on((uint64_t)watcher_l1);
	watcher_started = 1;
	dcache_clean_range((uint64_t)&watcher_started, 8);

	uint64_t end = dbg_ticks() + dbg_freq() * XNU_WATCH_SECONDS;
	gk_init();
	while (dbg_ticks() < end) {
		dcache_clean_range((uint64_t)&xnu_back, 8);
		if (xnu_back) {
			for (;;) {
				__asm__ volatile("wfe");
			}
		}
		gk_step();
		dbg_delay_us(250);
	}
	dcache_clean_range((uint64_t)&xnu_back, 8);
	if (xnu_back) {
		for (;;) {
			__asm__ volatile("wfe");
		}
	}

	fbc.base = 0; /* the screen is XNU's: read it, do not draw on it */
	marker_set(MARK_DONE);
	dbg_puts("\n=== watcher: XNU still running after " );
	dbg_dec(XNU_WATCH_SECONDS);
	dbg_puts(" s, snapshot ===\n");
	if (xsyms.cpu_ttep) {
		xnu_fp[3] = *(volatile uint64_t *)kva_to_pa(xsyms.cpu_ttep);
		xnu_fp[2] = 0x22a51a251aULL;
	}
	dbg_puts("cpu_ttep ");
	dbg_hex(xnu_fp[3], 10);
	if (xsyms.live) {
		dbg_puts(" heartbeat ");
		dbg_dec(*(volatile uint64_t *)kva_to_pa(xsyms.live + 8));
	}
	if (xsyms.log_len) {
		dbg_puts(" loader_log_len ");
		dbg_dec(rd32(kva_to_pa(xsyms.log_len)));
	}
	xnu_text_range();
	watch_xnu_cpus();
	watch_gic();
	out("keeper: Group 1 turned back on ");
	out_dec(gk.fixes);
	out(" times, ");
	out_dec(gk.changes);
	out(" changes seen, ");
	out_dec(gk.reports);
	out(" times the timer was overdue\n");
	/*
	 * PC sampling: the window is the MT6768's core 0 (0x0e010000). On the MT6897 there is a ROM table,
	 * a PMU and a CTI in it, but whose they are is not known yet: the ROM table's entries are printed.
	 */
	{
		const uint64_t win = 0x0e010000ULL; /* cpu_dbgapb, core 0 */
		uint64_t fb_saved = fbc.base, pmu = 0;
		out("\ncpu_dbgapb components:");
		for (uint64_t off = 0; off < 0x100000; off += 0x10000) {
			uint64_t b = win + off;
			uint32_t cidr1 = rd32(b + 0xff4), devarch = rd32(b + 0xfbc), devtype = rd32(b + 0xfcc);
			out("\n  +");
			out_hex(off);
			out(" CIDR1 ");
			out_hex(cidr1);
			out(" DEVARCH ");
			out_hex(devarch);
			out(" DEVTYPE ");
			out_hex(devtype);
			if ((devarch & 0xffff) == 0x2a16) { /* ARM PMU v3 */
				pmu = b;
			}
			if ((devarch & 0xffff) == 0x0af7 && cidr1 != 0xdeaddeadu) { /* CoreSight ROM table */
				uint32_t devid = rd32(b + 0xfc8);
				out(" DEVID ");
				out_hex(devid);
				out(" entries");
				for (int e = 0; e < 16; e++) {
					out(" ");
					out_hex((devid & 0xf) == 1 ? rd32(b + (uint64_t)e * 8) | (uint64_t)rd32(b + (uint64_t)e * 8 + 4) << 32
					    : rd32(b + (uint64_t)e * 4));
				}
			}
		}
		out("\n");
		if (pmu) {
			wr32(pmu + 0xfb0, 0xc5acce55u);
			out("cpu0 PC samples (PMPCSR):\n");
			for (int i = 0; i < 24; i++) {
				uint32_t lo = rd32(pmu + 0x200);
				uint32_t hi = rd32(pmu + 0x204);
				uint64_t pc = ((uint64_t)hi << 32 | lo) & 0x00ffffffffffffffULL;
				if (pc >> 47) {
					pc |= 0xff00000000000000ULL;
				}
				out("  ");
				out_sym(pc);
				out("\n");
				dbg_delay_ms(15);
			}
		}
		fbc.base = fb_saved;
	}
	if (xsyms.gic_unhandled_count) {
		dbg_puts("kernel: gic_unhandled_count ");
		dbg_dec(*(volatile uint64_t *)kva_to_pa(xsyms.gic_unhandled_count));
		dbg_puts(" last_unhandled ");
		dbg_dec(rd32(kva_to_pa(xsyms.gic_last_unhandled)));
		dbg_puts(" active_iar[0] ");
		dbg_hex(*(volatile uint64_t *)kva_to_pa(xsyms.gic_active_iar), 16);
		dbg_puts("\n");
	}

	dbg_puts("\n=== what the screen shows ===\n");
	read_screen();
	show_panic_string(&xsyms);
	/* (the ramoops console is a ring of 256 KB: the threads may fill it to 120 KB, the rest is for the logs) */
	watch_xnu_threads(120 * 1024);
	if (xsyms.msgbufp && xnu_fp[3]) {
		uint64_t mb = xnu_rd64(xsyms.msgbufp);
		uint64_t w0 = xnu_rd64(mb), w1 = xnu_rd64(mb + 8), bufc = xnu_rd64(mb + 16);
		int32_t size = (int32_t)(w0 >> 32), bufx = (int32_t)w1;
		dbg_puts("--- kernel message buffer (its last 32 KB) ---\n");
		if (size > 0 && size <= 1024 * 1024 && bufx >= 0 && bufx <= size) {
			int32_t first = size > 32 * 1024 ? size - 32 * 1024 : 0;
			for (int32_t k = first; k < size; k++) {
				char c = xnu_rd8(bufc + (uint64_t)((bufx + k) % size));
				if (c) {
					dbg_putc(c);
				}
			}
		}
		dbg_puts("\n--- end of message buffer ---\n");
	}
	if (xsyms.log && xsyms.log_len && xsyms.log_ring_at && xsyms.log_wrapped) {
		/* osfmk/arm64/loader_return.c: a 256 KB head, then a 1 MB ring. Its first 4 KB, and as much of its end as fits. */
		const uint64_t head_sz = 256 * 1024, ring_sz = 1024 * 1024;
		const volatile char *log = (const volatile char *)kva_to_pa(xsyms.log);
		uint32_t len = rd32(kva_to_pa(xsyms.log_len));
		uint32_t ring_at = rd32(kva_to_pa(xsyms.log_ring_at));
		uint8_t wrapped = *(const volatile uint8_t *)kva_to_pa(xsyms.log_wrapped);
		struct span all[3], tail[3];
		int n = 0, k = 0;
		uint64_t total = 0, room, want;

		if (ring_at > ring_sz) {
			ring_at = 0;
		}
		if (!wrapped) {
			all[n++] = (struct span){ log, len > head_sz + ring_sz ? head_sz + ring_sz : len };
		} else {
			all[n++] = (struct span){ log, head_sz };
			all[n++] = (struct span){ log + head_sz + ring_at, ring_sz - ring_at };
			all[n++] = (struct span){ log + head_sz, ring_at };
		}
		for (int i = 0; i < n; i++) {
			total += all[i].n;
		}
		room = PLOG_CAP - PLOG_HDR->size;
		room = room > 8192 ? room - 8192 : 0;
		dbg_puts("--- loader_log: ");
		dbg_dec(total);
		dbg_puts(" bytes ---\n");
		if (total <= room) {
			save_log(all, n);
		} else {
			struct span first = { all[0].p, 4096 };
			save_log(&first, 1);
			want = room > 4096 ? room - 4096 : 0;
			dbg_puts("\n[... ");
			dbg_dec(total - 4096 - want);
			dbg_puts(" bytes of the XNU log left out here ...]\n");
			/* the last `want` bytes, across the pieces */
			uint64_t skip = total - want;
			for (int i = 0; i < n; i++) {
				if (skip >= all[i].n) {
					skip -= all[i].n;
					continue;
				}
				tail[k++] = (struct span){ all[i].p + skip, all[i].n - skip };
				skip = 0;
			}
			save_log(tail, k);
		}
	}
	save_host_keep(&xsyms);
	dbg_puts("=== end of watcher snapshot ===\n");
	plog_flush();
	psci_system_reset();
	wdt_reset();
}

static void watcher_start(void)
{
	if (!XNU_WATCH_SECONDS) {
		return;
	}
	if (WATCH_CORE == 0 && boot_core != 0) {
		out("watcher: cpu0, this one, once cpu");
		out_dec((uint64_t)boot_core);
		out(" has gone into XNU\n");
		return;
	}
	watcher_started = 0;
	int64_t r = psci_cpu_on((uint64_t)WATCH_CORE << 8, (uint64_t)watcher_entry, 0);
	out("watcher on cpu");
	out_dec(WATCH_CORE);
	out(": PSCI CPU_ON ");
	out_dec((uint64_t)r);
	for (int i = 0; i < 100 && !watcher_started; i++) {
		dbg_delay_ms(10);
	}
	out(watcher_started ? ", running\n" : ", not seen\n");
}

/* ---- starting XNU ---- */

/* ---- how fast the cores are running ---- */

/*
 * What the boot loader left the cores at, in megahertz. The system counter runs at a fixed rate
 * whatever the cores do, so counting core cycles between two of its ticks gives the core's clock
 * directly. Nothing here changes a frequency -- it only asks, because a kernel that finds itself
 * on a core the boot loader parked at a third of its speed is worth knowing about before anyone
 * sets about raising it.
 */
static uint32_t cpu_mhz(void)
{
	uint64_t pmcr, cycles_before, cycles_after, ticks_before, ticks_after, hz = dbg_freq();

	__asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
	__asm__ volatile("msr pmcr_el0, %0" : : "r"((pmcr | 5u) & ~2u));   /* enable, reset the count */
	__asm__ volatile("msr pmcntenset_el0, %0" : : "r"(1ull << 31));
	__asm__ volatile("isb");

	ticks_before = dbg_ticks();
	__asm__ volatile("mrs %0, pmccntr_el0" : "=r"(cycles_before));
	dbg_delay_ms(20);
	__asm__ volatile("mrs %0, pmccntr_el0" : "=r"(cycles_after));
	ticks_after = dbg_ticks();

	if (cycles_after <= cycles_before || ticks_after <= ticks_before) {
		return 0;                       /* the counter is not ours to read */
	}
	return (uint32_t)(((cycles_after - cycles_before) * hz) /
	    ((ticks_after - ticks_before) * 1000000ull));
}

/* ---- the cores' clocks as the firmware sees them (read only) ---- */

/*
 * CPU frequency on this SoC is the MCUPM's business: Android's mediatek-cpufreq-hw.ko only writes an OPP index into
 * one of three "performance domains" in the MCUPM's shared SRAM (DT performance-controller@11bc00: domain 0 the four
 * A510s, 1 the three A715s, 2 core 7) and the firmware sets the PLL and the voltage. Per domain, from that module
 * (cpufreq_mtk_offsets and mtk_cpufreq_hw_cpu_init/target_index): +0x00 the table, 32 words, MHz in bits 11:0,
 * highest first; +0x84 "enable", which the module sets to 1; +0x88 the index asked for (the module's only write
 * afterwards); +0x8c status, which the module waits on until bits 0 (cpufreq hardware) and 1 (SVS) are both set;
 * +0x114 the transition latency. Run 11 read LK's choice back exactly: index 9 of domain 0 (1300 MHz) where core 0
 * counts 1299, index 22 of domain 2 (1200 MHz) where core 7 counts 1196.
 *
 * CPUFREQ_SET=1 asks every domain for its first entry, as the module would (enable 1, then the index), and only where
 * the status says the firmware is listening: 2200 / 3200 / 3350 MHz. The voltage that goes with it is the
 * firmware's, from its own table. Nothing in XNU brings it down again when the phone warms up: short runs only.
 */
#ifndef CPUFREQ_SHOW
#define CPUFREQ_SHOW        1
#endif
#ifndef CPUFREQ_SET
#define CPUFREQ_SET         0
#endif
#define CPUFREQ_DOMAIN(n)   (0x0011bc10ULL + 0x120ULL * (n))
#define CPUFREQ_READY       3u          /* status: cpufreq hardware and SVS */

static void cpufreq_show(void)
{
	static const char *const names[3] = { "A510 x4", "A715 x3", "A715 core 7" };

	for (int d = 0; d < 3; d++) {
		uint64_t b = CPUFREQ_DOMAIN(d);
		uint32_t en = rd32(b + 0x84), want = rd32(b + 0x88), now = rd32(b + 0x8c), last = 0;
		int n = 0;

		out("cpufreq domain ");
		out_dec((uint64_t)d);
		out(" (");
		out(names[d]);
		out("): enable ");
		out_hex(en);
		out(" status ");
		out_hex(now);
		out(" asked ");
		out_dec(want);
		if (want < 32) {
			out(" = ");
			out_dec(rd32(b + 4 * (uint64_t)want) & 0xfff);
			out(" MHz");
		}
		out(" latency ");
		out_dec(rd32(b + 0x114));
		out("\n  table MHz:");
		for (int i = 0; i < 32; i++) {
			uint32_t mhz = rd32(b + 4 * (uint64_t)i) & 0xfff;

			if (i > 0 && mhz == last) {
				break;
			}
			out(" ");
			out_dec(mhz);
			last = mhz;
			n++;
		}
		out(" (");
		out_dec((uint64_t)n);
		out(")\n");
	}
	out("cpufreq fdvfs signature (0x118000) ");
	out_hex(rd32(0x00118000ULL));
	out("\n");
}

static void cpufreq_set_max(void)
{
	for (int d = 0; d < 3; d++) {
		uint64_t b = CPUFREQ_DOMAIN(d);
		uint32_t status = rd32(b + 0x8c);

		out("cpufreq domain ");
		out_dec((uint64_t)d);
		if ((status & CPUFREQ_READY) != CPUFREQ_READY) {
			out(": status ");
			out_hex(status);
			out(", the firmware is not listening: left as it is\n");
			continue;
		}
		wr32(b + 0x84, 1);
		wr32(b + 0x88, 0);
		out(": asked for index 0, ");
		out_dec(rd32(b) & 0xfff);
		out(" MHz\n");
	}
	dbg_delay_ms(20);
}

/* ---- the core XNU starts on (XNU_BOOT_CORE) ---- */

static volatile uint64_t boot_core_state __attribute__((aligned(64)));   /* 1: waiting, 2: gone into XNU */
static volatile uint64_t boot_core_entry_pa, boot_core_args_pa;          /* set by this core at the jump */
static volatile uint64_t boot_core_el, boot_core_mhz;

static void order_cores(int boot)
{
	int n = 0;

	core_order[n++] = (uint8_t)boot;
	for (int i = 0; i < 8; i++) {
		if (plat_core_order[i] != boot && plat_core_order[i] != 0) {
			core_order[n++] = plat_core_order[i];
		}
	}
	if (boot != 0) {
		core_order[n++] = 0;
	}
}

/*
 * The boot core, from PSCI CPU_ON (start.S boot_core_entry): MMU and caches off like this one, so what either writes
 * the other reads from memory. It says how fast it runs and at which exception level it came in, and waits for the
 * kernel's entry; then it goes where this core would have gone. It prints nothing: the console is this core's until
 * the jump, and XNU's after it.
 */
void boot_core_main(void)
{
	uint64_t el;

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	boot_core_el = (el >> 2) & 3;
	boot_core_mhz = cpu_mhz();
	__asm__ volatile("dsb sy");
	boot_core_state = 1;
	__asm__ volatile("dsb sy; sev");
	while (!boot_core_entry_pa) {
		__asm__ volatile("wfe");
	}
	uint64_t entry = boot_core_entry_pa, args = boot_core_args_pa;
	boot_core_state = 2;
	__asm__ volatile("dsb sy; sev");
	jump_to_kernel(entry, args);
}

/*
 * Core 0 after the hand-over, with cpus=8 (XNU_CORE0_PARK): the mailbox filled in, then wait for the kernel's entry and
 * go there as PSCI CPU_ON would have brought it (start.S park_jump). Nothing is printed: the console is XNU's.
 */
static void __attribute__((noreturn)) core0_park(void)
{
	uint64_t entry;

	core_park[1] = PLAT_MPIDR(0);
	core_park[2] = 0;
	core_park[4] = 1;
	__asm__ volatile("dsb sy");
	core_park[0] = CORE_PARK_MAGIC;
	__asm__ volatile("dsb sy; sev");
	while ((entry = core_park[2]) == 0) {
		__asm__ volatile("wfe");
	}
	core_park[4] = 2;
	__asm__ volatile("dsb sy; sev");
	park_jump(entry);
}

/* Before the device tree is written, since cpu0 of the tree is whichever core comes in here. */
static void boot_core_start(void)
{
	boot_core = 0;
	core_park[0] = 0;
	if (XNU_BOOT_CORE != 0) {
		boot_core_state = 0;
		boot_core_entry_pa = 0;
		int64_t r = psci_cpu_on(PLAT_MPIDR(XNU_BOOT_CORE), (uint64_t)boot_core_entry, 0);

		for (int i = 0; i < 100 && r == 0 && boot_core_state != 1; i++) {
			dbg_delay_ms(10);
		}
		out("boot core cpu");
		out_dec(XNU_BOOT_CORE);
		out(": PSCI CPU_ON ");
		out_dec((uint64_t)r);
		if (r == 0 && boot_core_state == 1) {
			boot_core = XNU_BOOT_CORE;
			out(", waiting at EL");
			out_dec(boot_core_el);
			out(", running at ");
			out_dec(boot_core_mhz);
			out(" MHz\n");
		} else {
			out(", not seen: XNU starts on cpu0\n");
		}
	}
	order_cores(boot_core);
	out("cores for XNU:");
	for (int i = 0; i < XNU_CORES; i++) {
		out(" ");
		out_dec(core_order[i]);
	}
	out("\n");
}

/*
 * The jump, made by the boot core. This one says nothing after the line below (the screen is XNU's), waits until the
 * boot core has taken the entry, and then either watches (WATCH_CORE 0) or switches itself off, which is what the
 * kernel's PSCI CPU_ON needs of it when it is one of the kernel's cores (cpus=8).
 */
static void __attribute__((noreturn)) boot_core_jump(uint64_t entry_pa, uint64_t args_pa)
{
	out("Jumping to XNU on cpu");
	out_dec((uint64_t)boot_core);
	out("...\n");
	boot_core_args_pa = args_pa;
	__asm__ volatile("dsb sy");
	boot_core_entry_pa = entry_pa;
	__asm__ volatile("dsb sy; sev");
	while (boot_core_state != 2) {
		__asm__ volatile("wfe");
	}
	if (XNU_WATCH_SECONDS && WATCH_CORE == 0) {
		watcher_main();
	}
	if (XNU_CORE0_PARK) {
		core0_park();
	}
	core0_off = 1;
	core0_off = 0x100u | ((uint64_t)psci_cpu_off() & 0xffu);   /* only if it comes back: a refusal */
	for (;;) {
		__asm__ volatile("wfe");
	}
}

/* key and a number in decimal at the end of the kernel's command line; where the line ends now */
static size_t boot_arg_add(char *line, size_t at, const char *key, uint64_t v)
{
	char num[21];
	int n = 0;

	do {
		num[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	for (const char *c = key; *c && at < BOOT_LINE_LENGTH - 1; c++) {
		line[at++] = *c;
	}
	while (n && at < BOOT_LINE_LENGTH - 1) {
		line[at++] = num[--n];
	}
	line[at] = 0;
	return at;
}

/*
 * The framebuffer's height as XNU is told it. Run 20: XNU's io_map(), before its VM is up (initialize_screen maps the
 * framebuffer that early), maps round_page(size + offset in the page) but moves its next free address on by only
 * round_page(size). LK's framebuffer is at 0xfcc2f000, 0x3000 into a 16 KB page: 1220 x 2712 at 4992 bytes is
 * 0xce9400, 0xcec000 reserved for 0xcf0000 mapped, and the next mapping -- the GIC's distributor -- took the
 * framebuffer's last page. The last 0x390 bytes of the panel's bottom row went into GICD 0x000-0x38f: GICD_CTLR's
 * Group 1 off at the end of every copy of a frame (black pixels, 0xff000000; ISACTIVER set for SPIs 24-31 of each
 * word), and with the screen blanked, bzero's DC ZVA on device memory: "Unaligned kernel data abort" at 49.8 s.
 * Until every kernel has the fix (xnu/duchamp-overlay.py), the height is made a few rows taller, so that the two
 * roundings agree; those rows are below the panel, still in LK's framebuffer, and never shown.
 */
static uint32_t xnu_fb_height(void)
{
	const uint64_t page = 0x4000, off = fb_base & (page - 1);
	uint32_t h = PANEL_H;

	if (!fb_base || !fb_pitch) {
		return h;
	}
	for (uint32_t extra = 0; extra < 64; extra++) {
		uint64_t size = (uint64_t)(PANEL_H + extra) * fb_pitch;
		uint64_t reserved = (size + page - 1) & ~(page - 1), mapped = (size + off + page - 1) & ~(page - 1);
		if (reserved == mapped && (!lk_fb_size || fb_base + size <= lk_fb_base + lk_fb_size)) {
			h = PANEL_H + extra;
			break;
		}
	}
	out("framebuffer for XNU: ");
	out_dec(PANEL_W);
	out("x");
	out_dec(h);
	out(h == PANEL_H ? "\n" : " (rows below the panel, so that XNU's early io_map does not hand its last page to the next mapping)\n");
	return h;
}

#if TOUCH_PROBE || XNU_TOUCH || GPU_PROBE || XNU_GPU
#include "mtktouch.h"         /* the touch screen from the loader: both of duchamp's chips */
#endif
#if GPU_PROBE || XNU_GPU
#include "mtkgpu.h"           /* the Mali-G615 and its GPUEB: what is powered, read only */
#endif

static void __attribute__((noreturn)) run_xnu(void)
{
	struct payload_info pi;
	struct xnu_syms s;

	if (!parse_payload(&pi)) {
		out("ERROR: no XNU payload in this image\n");
		dbg_delay_ms(5000);
		chain_android();
	}
	if (CPUFREQ_SHOW || CPUFREQ_SET) {
		cpufreq_show();
	}
	if (CPUFREQ_SET) {
		cpufreq_set_max();
		cpufreq_show();
	}
	boot_core_start();
	out("XNU kernel ");
	out_dec(pi.kernel_size);
	out(" bytes, ramdisk ");
	out_dec(pi.ramdisk_size);
	out(" bytes\n");

	const struct mach_header_64 *mh = (const struct mach_header_64 *)pi.kernel;
	uint64_t vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
			uint64_t dst_pa = kva_to_pa(sg->vmaddr);
			memset((void *)dst_pa, 0, sg->vmsize);
			memcpy((void *)dst_pa, pi.kernel + sg->fileoff, sg->filesize);
		} else if (lc->cmd == LC_UNIXTHREAD) {
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	out("kernel at ");
	out_hex(kva_to_pa(0xfffffff007004000ULL));
	out(", entry ");
	out_hex(kva_to_pa(entry_va));
	out("\n");

	find_syms(pi.kernel, &s);
	xsyms = s;
	xkernel = pi.kernel;

	/* XNU's console colors (32 bpp column) opaque. */
	if (s.vc_colors) {
		volatile uint32_t *c = (volatile uint32_t *)kva_to_pa(s.vc_colors);
		for (int i = 0; i < 8; i++) {
			c[i * 4 + 2] |= 0xff000000u;
		}
		out("vc_colors: alpha set\n");
	}

	/*
	 * This kernel's serial console is a PL011 driver (vmapple_uart): its transmit_ready waits
	 * for a flag the MediaTek UART does not have, so "serial=3" hangs there forever. Make
	 * transmit_ready return 1 and transmit_data do nothing; kprintf then reaches loader_log
	 * (mt6577_uart_putc, which the kernel calls as well, is silenced below: on this SoC it points at another UART).
	 */
	if (s.uart_tx_ready && s.uart_tx_data) {
		volatile uint32_t *r = (volatile uint32_t *)kva_to_pa(s.uart_tx_ready);
		volatile uint32_t *d = (volatile uint32_t *)kva_to_pa(s.uart_tx_data);
		r[0] = 0x52800020u; /* mov w0, #1 */
		r[1] = 0xd65f03c0u; /* ret        */
		d[0] = 0xd65f03c0u; /* ret        */
		/*
		 * ... and the byte itself goes to loader_log_putc, which keeps it in the kernel's log
		 * buffer: that is how anything the first programs print reaches us at all. (This
		 * build of loader_log_putc calls nothing, so there is no way back round.)
		 */
		if (s.log_putc) {
			int64_t rel = (int64_t)(s.log_putc - s.uart_tx_data) >> 2;

			if (rel >= -(1 << 25) && rel < (1 << 25)) {
				d[0] = 0x14000000u | ((uint32_t)rel & 0x03ffffffu);
			}
		}
		/* and no input: the kernel's serial keyboard would otherwise read this UART's
		 * receive register, which here is another register entirely, for ever. */
		if (s.uart_rx_ready) {
			volatile uint32_t *rx = (volatile uint32_t *)kva_to_pa(s.uart_rx_ready);
			rx[0] = 0x52800000u; /* mov w0, #0 */
			rx[1] = 0xd65f03c0u; /* ret        */
		}
		out("vmapple_uart: transmit_ready patched, transmit_data -> loader_log_putc, receive off\n");
	}

	/*
	 * The MediaTek kernel's own early console (iokit/Kernel/GenericARMPE.cpp, mt6577_uart_putc, which loader_log_putc
	 * calls for every character as well) writes the MT6768's console UART, a constant 0x11002000. On the MT6897 that
	 * address is uart1 (serial@11002000), behind its own clock gate that nothing here has opened, and a read of a gated
	 * MediaTek block can stall the bus; at best it would cost 100000 reads a character. This phone has no serial port to
	 * read anyway, and everything the kernel says reaches pstore through loader_log_putc: the function returns at once.
	 * So does mt6577_uart_puts (GenericARMPE's two lines), into which the compiler inlined the same loop.
	 */
	if (s.mt6577_putc) {
		*(volatile uint32_t *)kva_to_pa(s.mt6577_putc) = 0xd65f03c0u; /* ret */
	}
	if (s.mt6577_puts) {
		*(volatile uint32_t *)kva_to_pa(s.mt6577_puts) = 0xd65f03c0u; /* ret */
	}
	if (s.mt6577_putc || s.mt6577_puts) {
		out("mt6577_uart_putc/puts -> ret (0x11002000 is uart1 on the MT6897)\n");
	}

	/*
	 * Everything written to /dev/console -- the kernel's own text and whatever the first
	 * programs print -- goes through uart_putc, and on this phone that is a PL011 driver
	 * writing into nothing. Send it to loader_log_putc instead (same argument, a character in
	 * w0), which keeps it in the kernel's log buffer and puts it out of the MediaTek UART as
	 * well. Without this the only voice userland has is lost.
	 */
	if (s.log_putc) {
		/* uart_putc is kprintf's way out, uart_putc_options the console's (and so userland's) */
		const uint64_t from[2] = { 0, 0 };

		for (int i = 0; i < 2; i++) {
			if (!from[i]) {
				continue;
			}
			volatile uint32_t *at = (volatile uint32_t *)kva_to_pa(from[i]);
			int64_t rel = (int64_t)(s.log_putc - from[i]) >> 2;

			if (rel >= -(1 << 25) && rel < (1 << 25)) {
				at[0] = 0x14000000u | ((uint32_t)rel & 0x03ffffffu); /* b loader_log_putc */
			}
		}
		out("uart_putc and uart_putc_options -> loader_log_putc\n");
	}

	/*
	 * EOImode is per processor, and gic_fix_eoimode() below only reaches the one this code runs
	 * on. Every other core comes out of PSCI CPU_ON with the bit set again, takes one timer
	 * interrupt and goes deaf: with four cores iOS crawled on the one that worked, and the
	 * kernel's own heartbeat beat 3 times in 840 seconds.
	 *
	 * Each core calls gic_v3_irq_cpu_init() for itself, and in it writes zero to ICC_BPR1_EL1.
	 * That instruction becomes a write of zero to ICC_CTLR_EL1, which clears EOImode (and CBPR,
	 * which wants to be zero anyway). What is given up is BPR1 = 0, the preemption grouping,
	 * which keeps its reset value -- a small thing beside a core whose timer never fires again.
	 */
	if (s.gic_cpu_init) {
		volatile uint32_t *at = (volatile uint32_t *)kva_to_pa(s.gic_cpu_init);
		int done = 0;

		for (int i = 0; i < 256 && !done; i++) {
			if (at[i] == 0xd518cc7fu) {          /* msr ICC_BPR1_EL1, xzr */
				at[i] = 0xd518cc9fu;         /* msr ICC_CTLR_EL1, xzr */
				done = 1;
			}
		}
		out(done ? "gic_v3_irq_cpu_init: every core clears EOImode\n" :
		    "gic_v3_irq_cpu_init: the BPR1 write is not where it was -- EOImode stays set\n");
	}

	/* Have XNU come back here on a panic or an early exception. */
	if (s.stub_slot) {
		volatile uint64_t *slot = (volatile uint64_t *)kva_to_pa(s.stub_slot);
		if (slot[0] == 0x545352444c554e58ULL) { /* "XNULDRST" */
			slot[1] = (uint64_t)xnu_return_stub;
			slot[2] = XNU_CHECKPOINT;
			slot[3] = XNU_SLIDE;
			out("return stub at ");
			out_hex(slot[1]);
			out("\n");
		}
	}

	uint64_t kern_hi = (kva_to_pa(vm_hi) + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	if (rd_size) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
	}
	/* and the big one off the flash, if it is there (with XNU_ROOT_EMMC the kernel reads it itself) */
	uint64_t sys_size = 0;       /* no storage driver yet: the payload's RAM disk is the root (docs/porting-mt6897.md) */
	uint64_t sys_pa = sys_size ? SYSTEM_PA : 0;
	uint64_t sys_span = (sys_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

	uint64_t dt_pa = sys_size ? sys_pa + sys_span : rd_pa + rd_size;
	uint32_t dt_len = build_device_tree(dt_pa, rd_pa, rd_size, sys_pa, sys_span);
	uint64_t ba_pa = (dt_pa + dt_len + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->physBase = XNU_PHYS_BASE;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->memSize = XNU_MEM_SIZE;
	ba->memSizeActual = XNU_MEM_SIZE;
	ba->topOfKernelData = top;
	ba->deviceTreeP = dt_pa - XNU_SLIDE;
	ba->deviceTreeLength = dt_len;
	ba->video.v_baseAddr = fb_base;
	ba->video.v_display = XNU_VERBOSE ? 0 : 1; /* 0: the kernel's text console, 1: for iOS to draw */
	ba->video.v_rowBytes = fb_pitch;
	ba->video.v_width = PANEL_W;
	ba->video.v_height = xnu_fb_height();
	ba->video.v_depth = 32;
	memcpy(ba->commandLine, XNU_BOOT_ARGS, sizeof(XNU_BOOT_ARGS));
	{
		size_t at = sizeof(XNU_BOOT_ARGS) - 1;

		if (ufs_xnu_ok) {
			/* where the partition is, for bsd/dev/redwood_mtkufs.c */
			at = boot_arg_add(ba->commandLine, at, " ufs_lun=", 2);
			at = boot_arg_add(ba->commandLine, at, " ufs_lba=", ufs_xnu_lba);
			at = boot_arg_add(ba->commandLine, at, " ufs_blocks=", ufs_xnu_blocks);
		}
#if XNU_TOUCH
		if (touch_chip != TOUCH_NONE) {
			/* what mtktouch.h found, for bsd/dev/redwood_mtktouch.c */
			at = boot_arg_add(ba->commandLine, at, " mtktouch=", touch_chip);
			at = boot_arg_add(ba->commandLine, at, " mtktouch_at=", gdx_touch_addr);
			at = boot_arg_add(ba->commandLine, at, " mtktouch_x=", gdx_max_x);
			at = boot_arg_add(ba->commandLine, at, " mtktouch_y=", gdx_max_y);
		}
#endif
#if XNU_GPU
		{
			uint32_t gpu_fw = 0;
			uint64_t gpu_eb = 0;

			if (gpu_for_xnu(&gpu_fw, &gpu_eb)) {
				at = boot_arg_add(ba->commandLine, at, " mtkgpu=", 1);
				at = boot_arg_add(ba->commandLine, at, " mtkgpu_fw=", gpu_fw);
				at = boot_arg_add(ba->commandLine, at, " mtkgpu_eb=", gpu_eb);
			}
		}
#endif
		if (XNU_CORE0_PARK && boot_core != 0) {
			/* core 0's mailbox, for the kernel's psci_smp.c (its physical address: this program runs with its MMU off) */
			at = boot_arg_add(ba->commandLine, at, " cpu_park=", (uint64_t)(uintptr_t)core_park);
		}
	}

	out("md0 ");
	out_hex(sys_size ? sys_pa : rd_pa);
	out(sys_size ? " (the system image), md1 " : XNU_ROOT_EMMC ? " is none: md1 (root.packfs), the system image on the eMMC" :
	    " (root.packfs only)");
	if (sys_size) {
		out_hex(rd_pa);
	}
	out("\nboot-args: ");
	out(ba->commandLine);
	out("\nframebuffer ");
	out_hex(fb_base);
	out(" pitch ");
	out_dec(fb_pitch);
	out(", XNU memory ");
	out_hex(XNU_PHYS_BASE);
	out("+");
	out_hex(XNU_MEM_SIZE);
	out("\n");
	display_alpha_off();

	if (XNU_WDT_SECONDS) {
		uint32_t t = XNU_WDT_SECONDS > 31 ? 31 : XNU_WDT_SECONDS;
		marker_set(MARK_RUNNING);
		wr32(TOPRGU_LENGTH, ((t * 32768u / 512u) << 5) | 0x8u);
		wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
		wdt_mode_set(1);         /* LK's other mode bits kept (mtkdbg.h) */
		out("watchdog: back to Android in ");
		out_dec(t);
		out(" s\n");
	} else {
		marker_set(0);
	}
	/* The timers XNU will use: a virtual counter offset by EL2 (MediaTek's GenieZone) would
	 * leave its deadlines forever in the future, and the kernel idle with nothing to wake it. */
	{
		uint64_t pct, vct, frq, kctl, pfr0;
		__asm__ volatile("isb; mrs %0, cntpct_el0; mrs %1, cntvct_el0; mrs %2, cntfrq_el0;"
		    "mrs %3, cntkctl_el1; mrs %4, id_aa64pfr0_el1"
		    : "=r"(pct), "=r"(vct), "=r"(frq), "=r"(kctl), "=r"(pfr0));
		out("timers: CNTPCT ");
		out_hex(pct);
		out(" CNTVCT ");
		out_hex(vct);
		out(" delta ");
		out_hex(pct - vct);
		out(" CNTFRQ ");
		out_dec(frq);
		out(" CNTKCTL ");
		out_hex(kctl);
		out(" EL2 ");
		out_dec((pfr0 >> 8) & 0xf);
		out("\n");
	}

	{
		uint32_t mhz = cpu_mhz();

		out("this core is running at ");
		if (mhz) {
			out_dec(mhz);
			out(" MHz\n");
		} else {
			out("a speed its cycle counter will not say\n");
		}
	}

	/* must happen on the core the kernel runs on, and before it sets its GIC up */
	gic_fix_eoimode();
#if TIMER_PROBE
	timer_probe();
#endif
	watcher_start();
	/*
	 * The touch screen, while there is still a console to complain to. The controller has to be
	 * handed its firmware before anything can read a finger out of it, and the kernel is a bad
	 * place to do it from: it would have to carry those hundred kilobytes itself, and it comes
	 * up long after the chip has forgotten them.
	 */
	/* (the MT6768's touch firmware upload and big-core clock step are that phone's: none here yet, docs/porting-mt6897.md) */
	/*
	 * Nothing of ours is left on the panel: iOS paints only what it draws, and where it does not
	 * reach, what LK wrote there stays -- its "Orange State: your device has been unlocked"
	 * showed through the home screen.
	 */
	fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
	for (uint64_t at = fb_base; fb_base && at < fb_base + (uint64_t)PANEL_H * fb_pitch; at += 64) {
		__asm__ volatile("dc cvac, %0" : : "r"(at));
	}
	__asm__ volatile("dsb sy");
	if (boot_core != 0) {
		boot_core_jump(kva_to_pa(entry_va), ba_pa);
	}
	out("Jumping to XNU...\n");
	jump_to_kernel(kva_to_pa(entry_va), ba_pa);
	__builtin_unreachable();
}




/* ---- choosing what to boot ---- */

/*
 * Android unless asked for XNU, once. The keypad's state registers change by themselves (a scan
 * pattern), so keys are not used. The request is a line written to /dev/pmsg0 under Android
 * (tools/boot-xnu.sh): ramoops keeps it in its pmsg zone across the reboot, where this finds it
 * and wipes it, so the boot after XNU is Android again.
 */
static const char boot_xnu_flag[] = "DARWIN-REDWOOD:BOOT-XNU";

/*
 * Whether Android left `flag` in the pmsg zone before rebooting, and if so wipe it, so it is taken
 * once: a run that hangs costs one reboot, after which the loader finds nothing and starts Android.
 */
static int pmsg_take(const char *flag, uint32_t n)
{
	volatile struct ramoops_hdr *h = (volatile struct ramoops_hdr *)RAMOOPS_PMSG;
	volatile char *d = (volatile char *)(RAMOOPS_PMSG + sizeof(struct ramoops_hdr));
	uint32_t cap = RAMOOPS_PMSG_SIZE - sizeof(struct ramoops_hdr);
	int found = 0;

	if (!RAMOOPS_PMSG || RAMOOPS_PMSG_SIZE <= sizeof(struct ramoops_hdr) || h->sig != RAMOOPS_SIG || h->size > cap) {
		return 0;
	}
	for (uint32_t at = 0; at + n <= cap; at++) {
		uint32_t k = 0;
		while (k < n && d[at + k] == flag[k]) {
			k++;
		}
		if (k == n) {
			for (k = 0; k < n; k++) {
				d[at + k] = '-';
			}
			found = 1;
		}
	}
	return found;
}

static int xnu_requested(void)
{
	return pmsg_take(boot_xnu_flag, sizeof(boot_xnu_flag) - 1);
}

static int boot_choice(void)
{
	int xnu = MENU_DEFAULT_XNU || xnu_requested();
	uint64_t screen = fbc.base;

	fbc.base = 0;
	dbg_puts(xnu ? "boot: Darwin / XNU (requested)\n" : "boot: Android\n");
	fbc.base = screen;
	fb_rect(0, 600, PANEL_W, 160, 0xff000000u);
	fbc.scale = 3;
	fbc.x = 48;
	fbc.y = 640;
	fbc.fg = xnu ? 0xff40ff40u : 0xffffffffu;
	for (const char *c = xnu ? "Booting Darwin / XNU" : "Booting Android"; *c; c++) {
		fbcon_putc(*c);
	}
	fbc.scale = 1;
	fbc.fg = 0xffffffffu;
	dbg_delay_ms(1000);
	return xnu;
}



/*
 * UFS_PROBE=1: the storage controller as LK leaves it, read only -- the facts the UFS step needs before any of it is written
 * (docs/porting-mt6897.md). ufshci@112b0000 ("mediatek,mt8183-ufshci", 0x3000 of registers) has nine interrupts in Android's
 * tree, one more than a controller with one doorbell list needs: the host may be UFSHCI 4.0 with multi-circular queues (MCQ),
 * and which of the two modes LK runs it in decides the whole driver. Read here: the standard registers (JESD223, 0x00-0x9c),
 * the MCQ configuration (0x300, 0x380 and the first queue's registers where MCQCAP says they are), MediaTek's own
 * reference clock and IP version registers (Linux's ufs-mediatek.h), and the first two transfer request descriptors in DRAM
 * where UTRLBA points. Nothing is written. Asked for the same way as XNU (once); then Android, without XNU.
 */
#ifndef UFS_PROBE
#define UFS_PROBE           0
#endif
#if UFS_PROBE || XNU_UFS
static void ufs_reg(const char *name, uint32_t off)
{
	out("  ");
	out(name);
	out(" [");
	out_hex(off);
	out("] ");
	out_hex(rd32(PLAT_UFSHCI_BASE + off));
	out("\n");
}

static void ufs_dump(const char *what, uint64_t pa, uint32_t bytes)
{
	out("  ");
	out(what);
	out(" at ");
	out_hex(pa);
	out(":");
	for (uint32_t i = 0; i < bytes; i += 4) {
		if (i % 32 == 0) {
			out("\n   ");
		}
		out(" ");
		out_hex(rd32(pa + i));
	}
	out("\n");
}

static void __attribute__((unused)) ufs_probe(void)
{
	static const struct { const char *name; uint16_t off; } regs[] = {
		{ "CAP", 0x00 }, { "MCQCAP", 0x04 }, { "VER", 0x08 }, { "EXT_CAP", 0x0c }, { "HCPID", 0x10 }, { "HCMID", 0x14 },
		{ "AHIT", 0x18 }, { "IS", 0x20 }, { "IE", 0x24 }, { "HCS", 0x30 }, { "HCE", 0x34 }, { "UECPA", 0x38 },
		{ "UECDL", 0x3c }, { "UECN", 0x40 }, { "UECT", 0x44 }, { "UECDME", 0x48 }, { "UTRIACR", 0x4c },
		{ "UTRLBA", 0x50 }, { "UTRLBAU", 0x54 }, { "UTRLDBR", 0x58 }, { "UTRLCLR", 0x5c }, { "UTRLRSR", 0x60 },
		{ "UTRLCNR", 0x64 }, { "UTMRLBA", 0x70 }, { "UTMRLBAU", 0x74 }, { "UTMRLDBR", 0x78 }, { "UTMRLRSR", 0x80 },
		{ "UICCMD", 0x90 }, { "UCMDARG1", 0x94 }, { "UCMDARG2", 0x98 }, { "UCMDARG3", 0x9c },
		{ "CONFIG", 0x300 }, { "MCQCONFIG", 0x380 },
		{ "MTK_XOUFS_CTRL", 0x140 }, { "MTK_REFCLK_CTRL", 0x144 }, { "MTK_MMIO_OPT_CTRL_0", 0x160 },
		{ "MTK_IP_VER", 0x2240 },
	};
	uint32_t cap, mcqcap, utrlba, utrlbau;

	out("UFS probe: ufshci at ");
	out_hex(PLAT_UFSHCI_BASE);
	out(", read only\n");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		ufs_reg(regs[i].name, regs[i].off);
	}
	cap = rd32(PLAT_UFSHCI_BASE + 0x00);
	mcqcap = rd32(PLAT_UFSHCI_BASE + 0x04);
	out("  slots ");
	out_dec((cap & 0x1f) + 1);
	out(", 64-bit ");
	out_dec((cap >> 24) & 1);
	out(", MCQ supported ");
	out_dec((cap >> 30) & 1);
	out(", MCQ in use (CONFIG.QT) ");
	out_dec(rd32(PLAT_UFSHCI_BASE + 0x300) & 1);
	out(", HCS device present ");
	out_dec(rd32(PLAT_UFSHCI_BASE + 0x30) & 1);
	out(" list ready ");
	out_dec((rd32(PLAT_UFSHCI_BASE + 0x30) >> 1) & 1);
	out("\n");
	if ((cap >> 30) & 1) {
		/* MCQCAP: MAXQ 7:0, QCFGPTR 31:24 (the queue registers at QCFGPTR * 0x200) */
		uint32_t qcfg = ((mcqcap >> 24) & 0xff) * 0x200;

		out("  MCQ: ");
		out_dec((mcqcap & 0xff) + 1);
		out(" queues, their registers at ");
		out_hex(qcfg);
		out("\n");
		if (qcfg && qcfg + 0x40 <= 0x3000) {
			ufs_dump("queue 0's registers", PLAT_UFSHCI_BASE + qcfg, 0x40);
		}
	}
	utrlba = rd32(PLAT_UFSHCI_BASE + 0x50);
	utrlbau = rd32(PLAT_UFSHCI_BASE + 0x54);
	uint64_t list = (uint64_t)utrlbau << 32 | (utrlba & ~0x3ffu);
	if (list >= PLAT_ANDROID_PA && list + 64 <= PLAT_DRAM_TOP) {
		ufs_dump("UTRL slots 0-1", list, 64);
		/* slot 0's command descriptor (UCDBA/UCDBAU at +8/+12): its command UPIU, the last thing LK sent */
		uint64_t ucd = (uint64_t)rd32(list + 12) << 32 | (rd32(list + 8) & ~0x7fu);
		if (ucd >= PLAT_ANDROID_PA && ucd + 32 <= PLAT_DRAM_TOP) {
			ufs_dump("slot 0's command UPIU", ucd, 32);
		}
	} else {
		out("  UTRL not in DRAM: ");
		out_hex(list);
		out("\n");
	}
}
/*
 * UFS_PROBE=2: the first stage of bringing the storage up, after run 4 found it off. Nothing of it reaches the flash: the
 * host controller is enabled (HCE), the UniPro link to the device is started (UIC DME_LINKSTARTUP, which the spec allows
 * three tries) and a few of its attributes are read back (DME_GET); then the host is disabled again (HCE 0), as LK left
 * it, and Android brings it up from nothing as always. Every step says what it is about to do first.
 * Registers: JESD223 (UFSHCI 4.0) -- HCE 0x34, HCS 0x30 (DP bit 0, UCRDY bit 3), IS 0x20 (UCCS bit 10, write 1 to clear),
 * UICCMD 0x90, UCMDARG1-3 0x94-0x9c; UniPro attribute ids from the MIPI UniPro spec, as Linux's ufs/unipro.h names them.
 */
#define UFS_HCE             0x34
#define UFS_HCS             0x30
#define UFS_IS              0x20
#define UFS_UICCMD          0x90
#define UFS_ARG1            0x94
#define UFS_ARG2            0x98
#define UFS_ARG3            0x9c
#define UFS_IS_UCCS         (1u << 10)

static int ufs_wait(uint32_t off, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t end = dbg_ticks() + dbg_freq() / 1000 * ms;

	while ((rd32(PLAT_UFSHCI_BASE + off) & mask) != want) {
		if (dbg_ticks() > end) {
			return 0;
		}
	}
	return 1;
}

/* one UIC command; returns its result code (0 = success), 0x100 on a time-out; *value gets ARG3 */
static uint32_t ufs_uic(uint32_t cmd, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t *value)
{
	if (!ufs_wait(UFS_HCS, 1u << 3, 1u << 3, 100)) {
		out("  UIC not ready (HCS ");
		out_hex(rd32(PLAT_UFSHCI_BASE + UFS_HCS));
		out(")\n");
		return 0x100;
	}
	wr32(PLAT_UFSHCI_BASE + UFS_IS, UFS_IS_UCCS);
	wr32(PLAT_UFSHCI_BASE + UFS_ARG1, arg1);
	wr32(PLAT_UFSHCI_BASE + UFS_ARG2, arg2);
	wr32(PLAT_UFSHCI_BASE + UFS_ARG3, arg3);
	wr32(PLAT_UFSHCI_BASE + UFS_UICCMD, cmd);
	if (!ufs_wait(UFS_IS, UFS_IS_UCCS, UFS_IS_UCCS, 500)) {
		out("  UIC command ");
		out_hex(cmd);
		out(" did not complete (IS ");
		out_hex(rd32(PLAT_UFSHCI_BASE + UFS_IS));
		out(")\n");
		return 0x100;
	}
	wr32(PLAT_UFSHCI_BASE + UFS_IS, UFS_IS_UCCS);
	if (value) {
		*value = rd32(PLAT_UFSHCI_BASE + UFS_ARG3);
	}
	return rd32(PLAT_UFSHCI_BASE + UFS_ARG2) & 0xff;
}

static void __attribute__((unused)) ufs_link_test(void)
{
	static const struct { const char *name; uint16_t id; } attrs[] = {
		{ "PA_ConnectedTxDataLanes", 0x1561 }, { "PA_ConnectedRxDataLanes", 0x1581 }, { "PA_PWRMode", 0x1571 },
		{ "PA_TxGear", 0x1568 }, { "PA_RxGear", 0x1583 }, { "PA_MaxRxHSGear", 0x1587 }, { "PA_RemoteVerInfo", 0x15a0 },
		{ "PA_LocalVerInfo", 0x15a9 }, { "N_DeviceID", 0x3000 },
	};
	uint32_t r = 0x100, v;

	out("UFS stage A: HCE <- 1\n");
	wr32(PLAT_UFSHCI_BASE + UFS_HCE, 1);
	if (!ufs_wait(UFS_HCE, 1, 1, 100)) {
		out("  HCE did not come to 1 (the host may be held in reset): stopping here\n");
		return;
	}
	out("  host enabled; HCS ");
	out_hex(rd32(PLAT_UFSHCI_BASE + UFS_HCS));
	out(", IS ");
	out_hex(rd32(PLAT_UFSHCI_BASE + UFS_IS));
	out("\n");
	for (int tries = 1; tries <= 3; tries++) {
		out("UFS stage A: DME_LINKSTARTUP, try ");
		out_dec(tries);
		out("\n");
		r = ufs_uic(0x16, 0, 0, 0, NULL);
		out("  result ");
		out_hex(r);
		out(", HCS ");
		out_hex(rd32(PLAT_UFSHCI_BASE + UFS_HCS));
		out(", IS ");
		out_hex(rd32(PLAT_UFSHCI_BASE + UFS_IS));
		out("\n");
		if (r == 0 && (rd32(PLAT_UFSHCI_BASE + UFS_HCS) & 1)) {
			break;
		}
	}
	if (r == 0 && (rd32(PLAT_UFSHCI_BASE + UFS_HCS) & 1)) {
		out("  link up, device present\n");
		for (unsigned i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
			v = 0;
			r = ufs_uic(0x01, (uint32_t)attrs[i].id << 16, 0, 0, &v);     /* DME_GET, selector 0 */
			out("  ");
			out(attrs[i].name);
			out(" = ");
			out_hex(v);
			if (r) {
				out(" (result ");
				out_hex(r);
				out(")");
			}
			out("\n");
		}
	} else {
		out("  no link: the M-PHY or UniPro needs MediaTek's setup first (next stage)\n");
	}
	out("UFS stage A: HCE <- 0 (as LK left it)\n");
	wr32(PLAT_UFSHCI_BASE + UFS_HCE, 0);
	ufs_wait(UFS_HCE, 1, 0, 100);
	out("  HCE ");
	out_hex(rd32(PLAT_UFSHCI_BASE + UFS_HCE));
	out("\n");
}
#if UFS_PROBE >= 3 || XNU_UFS
/*
 * UFS_PROBE=3. Run 5 enabled the host and asked for the link three times: FAILURE each time, with a UIC error (IS.UE).
 * Android's driver for this controller never starts the link from the state LK leaves. The phone's own ufs-mediatek
 * (vendor_dlkm), which follows Linux's, first does what this does, in the same order:
 *   1. the UFS clocks ungated: ufscfg_ao 0x112b8000 (UniPro TX symbol, RX symbol 0 and 1, UniPro system clock: bits 0-3)
 *      and ufscfg_pdn 0x112bb000 (UFSHCI, its AES engine: bits 0-1); set/clear/status at +4/+0xc/+8, a set status bit
 *      meaning gated (clk-mt6897-peri's tables)
 *   2. the device reset through MediaTek's SiP call (SMC64 fast 0xc2000276 = MTK_SIP_UFS_CONTROL, x1 = 2 = device
 *      reset, x2 = 0 holds RST_n low, 1 lets it go), with the host off in between and 10-15 ms after
 *   3. the host's UniPro, crypto and HCI blocks reset (ufscfg_pdn: set at 0x48, clear at 0x4c, bits 0-2 -- the tree's
 *      ti,reset-bits), then XOUFS_CTRL bit 7 (the clock gate kept on, against a false error check) and HCE
 *   4. before the link: UniPro's own power-down off (VS_UniproPowerDownControl 0xd0a8 <- 0), the host's TX line
 *      control commands off (PA_Local_TX_LCC_Enable 0x155e <- 0), deep stall off (VS_SavePowerControl 0xd0a6 bit 6)
 * Its M-PHY driver does nothing on this phone (the tree's mphy-ver 1), so neither does this. After each try: the UIC
 * error registers and the link state (MediaTek debug select 0x22c0/0x22d0-0x22dc, state in bits 31:28 of 0x22c8).
 * Nothing of it writes to the flash. The watchdog is armed meanwhile (a hang resets the board and this log is kept).
 */
#define MTK_SIP_UFS_CONTROL     0xc2000276u
#define UFS_SIP_DEVICE_RESET    2u
#define UFSCFG_AO_BASE          0x112b8000ULL
#define UFSCFG_PDN_BASE         0x112bb000ULL
#define UFSCFG_CG_SET           0x04
#define UFSCFG_CG_STA           0x08
#define UFSCFG_CG_CLR           0x0c
#define UFSCFG_RST_SET          0x48
#define UFSCFG_RST_CLR          0x4c

#define UFS_UECPA               0x38
#define UFS_UTRLBA              0x50
#define UFS_UTRLBAU             0x54
#define UFS_UTRLDBR             0x58
#define UFS_UTRLCLR             0x5c
#define UFS_UTRLRSR             0x60
#define UFS_UTRLCNR             0x64
#define UFS_UTMRLBA             0x70
#define UFS_UTMRLBAU            0x74
#define UFS_UTMRLRSR            0x80
#define UFS_XOUFS_CTRL          0x140
#define UFS_REFCLK_CTRL         0x144
#define UFS_SIP_REF_CLK         8u

static uint64_t ufs_sip(uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = MTK_SIP_UFS_CONTROL;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	register uint64_t x4 __asm__("x4") = 0;
	register uint64_t x5 __asm__("x5") = 0;
	register uint64_t x6 __asm__("x6") = 0;
	register uint64_t x7 __asm__("x7") = 0;

	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
			 :
			 : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void ufs_wdt_kick(void)
{
	wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
}

static void ufs_kv(const char *name, uint32_t v)
{
	out(name);
	out(" ");
	out_hex(v);
}

static uint32_t ufs_rd(uint32_t off)
{
	return rd32(PLAT_UFSHCI_BASE + off);
}

static void ufs_link_state(void)
{
	static const char *const names[] = { "disabled", "down", "up", "hibern8", "lost", "config" };
	uint32_t v;

	/* the select Android's driver writes for this IP (MTK_IP_VER 0x01440000: the newer one above 3.5) */
	if ((ufs_rd(0x2240) & 0xfe0000) > 0x350000) {
		wr32(PLAT_UFSHCI_BASE + 0x22c0, 0x00820820u);
		wr32(PLAT_UFSHCI_BASE + 0x22d0, 0);
		wr32(PLAT_UFSHCI_BASE + 0x22d4, 0x55555555u);
		wr32(PLAT_UFSHCI_BASE + 0x22d8, 0xaaaaaaaau);
		wr32(PLAT_UFSHCI_BASE + 0x22dc, 0xffffffffu);
	} else {
		wr32(PLAT_UFSHCI_BASE + 0x22c0, 0x20);
	}
	v = ufs_rd(0x22c8);
	out("  UniPro link state ");
	out_dec(v >> 28);
	if ((v >> 28) < 6) {
		out(" (");
		out(names[v >> 28]);
		out(")");
	}
	out(", probe register ");
	out_hex(v);
	out("\n");
}

static void ufs_errors(void)
{
	static const char *const layer[] = { "PA", "DL", "N", "T", "DME" };

	out("  UIC errors:");
	for (int i = 0; i < 5; i++) {
		uint32_t v = ufs_rd(UFS_UECPA + 4 * i);   /* clear on read */
		out(" ");
		ufs_kv(layer[i], v);
	}
	out("\n");
}

static uint32_t ufs_dme_get(uint16_t id)
{
	uint32_t v = 0, r = ufs_uic(0x01, (uint32_t)id << 16, 0, 0, &v);

	return r ? 0xdead0000u | r : v;
}

static uint32_t ufs_dme_set(uint16_t id, uint32_t v)
{
	return ufs_uic(0x02, (uint32_t)id << 16, 0, v, NULL);
}

static void ufs_clocks(void)
{
	out("  clock gates (1 = gated): ufscfg_ao ");
	out_hex(rd32(UFSCFG_AO_BASE + UFSCFG_CG_STA));
	out(", ufscfg_pdn ");
	out_hex(rd32(UFSCFG_PDN_BASE + UFSCFG_CG_STA));
	wr32(UFSCFG_AO_BASE + UFSCFG_CG_CLR, 0xf);
	wr32(UFSCFG_PDN_BASE + UFSCFG_CG_CLR, 0x3);
	dbg_delay_us(10);
	out("; ungated: ");
	out_hex(rd32(UFSCFG_AO_BASE + UFSCFG_CG_STA));
	out(", ");
	out_hex(rd32(UFSCFG_PDN_BASE + UFSCFG_CG_STA));
	out("\n");
}

static void ufs_device_reset(void)
{
	uint64_t r0, r1;

	r0 = ufs_sip(UFS_SIP_DEVICE_RESET, 0, 0);
	wr32(PLAT_UFSHCI_BASE + UFS_HCE, 0);
	ufs_wait(UFS_HCE, 1, 0, 10);
	dbg_delay_us(15);
	r1 = ufs_sip(UFS_SIP_DEVICE_RESET, 1, 0);
	dbg_delay_ms(15);
	out("  device reset (SiP): hold ");
	out_hex(r0);
	out(", release ");
	out_hex(r1);
	out(r0 == 0 && r1 == 0 ? " (both accepted)\n" : " (0 = accepted; 0xffff... = ATF does not know the call)\n");
}

/* host reset, XOUFS bit 7, HCE; then UniPro's settings before the link. 1 when the host is enabled. */
static int ufs_host_enable(void)
{
	wr32(UFSCFG_PDN_BASE + UFSCFG_RST_SET, 1u << 2);   /* HCI */
	wr32(UFSCFG_PDN_BASE + UFSCFG_RST_SET, 1u << 1);   /* crypto */
	wr32(UFSCFG_PDN_BASE + UFSCFG_RST_SET, 1u << 0);   /* UniPro */
	dbg_delay_us(110);
	wr32(UFSCFG_PDN_BASE + UFSCFG_RST_CLR, 1u << 0);
	wr32(UFSCFG_PDN_BASE + UFSCFG_RST_CLR, 1u << 1);
	wr32(UFSCFG_PDN_BASE + UFSCFG_RST_CLR, 1u << 2);
	dbg_delay_us(10);
	wr32(PLAT_UFSHCI_BASE + UFS_XOUFS_CTRL, ufs_rd(UFS_XOUFS_CTRL) | 0x80);
	wr32(PLAT_UFSHCI_BASE + UFS_HCE, 1);
	dbg_delay_us(600);
	if (!ufs_wait(UFS_HCE, 1, 1, 100)) {
		out("  HCE did not come to 1 after the host reset\n");
		return 0;
	}
	out("  host reset and enabled: HCS ");
	out_hex(ufs_rd(UFS_HCS));
	out(", XOUFS_CTRL ");
	out_hex(ufs_rd(UFS_XOUFS_CTRL));
	out(", REFCLK_CTRL ");
	out_hex(ufs_rd(UFS_REFCLK_CTRL));
	if ((ufs_rd(UFS_REFCLK_CTRL) & 3) != 3) {
		/* the device's reference clock was let go with the host: asked for again, as Android's driver does it
		 * (SiP 8 = reference clock notice, before and after; request bit 0, acknowledge bit 1, 3 ms) */
		ufs_sip(UFS_SIP_REF_CLK, 1, 0);
		wr32(PLAT_UFSHCI_BASE + UFS_REFCLK_CTRL, 1);
		int ok = ufs_wait(UFS_REFCLK_CTRL, 3, 3, 3);
		dbg_delay_us(32);
		ufs_sip(UFS_SIP_REF_CLK, 1, 1);
		out(ok ? " -> asked again, acknowledged" : " -> asked again, NOT acknowledged");
	}
	out("\n  before the link:");
	ufs_kv(" VS_UniproPowerDownControl", ufs_dme_get(0xd0a8));
	ufs_kv(", PA_Local_TX_LCC_Enable", ufs_dme_get(0x155e));
	ufs_kv(", VS_SavePowerControl", ufs_dme_get(0xd0a6));
	out("\n");
	uint32_t r = ufs_dme_set(0xd0a8, 0);
	r |= ufs_dme_set(0x155e, 0) << 8;
	uint32_t spc = ufs_dme_get(0xd0a6);
	if (!(spc & 0xdead0000u)) {
		r |= ufs_dme_set(0xd0a6, spc & ~0x40u) << 16;
	}
	out("  set: power-down 0, TX LCC 0, deep stall off (results ");
	out_hex(r);
	out(", 0 = all accepted); now");
	ufs_kv(" PowerDown", ufs_dme_get(0xd0a8));
	ufs_kv(", LCC", ufs_dme_get(0x155e));
	ufs_kv(", SavePower", ufs_dme_get(0xd0a6));
	out("\n");
	return 1;
}

static int ufs_bringup(void)
{
	static const struct { const char *name; uint16_t id; } attrs[] = {
		{ "PA_ConnectedTxDataLanes", 0x1561 }, { "PA_ConnectedRxDataLanes", 0x1581 }, { "PA_PWRMode", 0x1571 },
		{ "PA_TxGear", 0x1568 }, { "PA_RxGear", 0x1583 }, { "PA_MaxRxHSGear", 0x1587 }, { "PA_RemoteVerInfo", 0x15a0 },
		{ "PA_LocalVerInfo", 0x15a9 }, { "N_DeviceID", 0x3000 },
	};
	int up = 0;

	out("UFS stage A2: the bring-up as Android's ufs-mediatek does it\n");
	ufs_link_state();
	ufs_clocks();
	ufs_device_reset();
	for (int tries = 1; tries <= 3 && !up; tries++) {
		ufs_wdt_kick();
		out("UFS stage A2: try ");
		out_dec(tries);
		out("\n");
		if (!ufs_host_enable()) {
			break;
		}
		uint32_t r = ufs_uic(0x16, 0, 0, 0, NULL);
		out("  DME_LINKSTARTUP: result ");
		out_hex(r);
		out(", HCS ");
		out_hex(ufs_rd(UFS_HCS));
		out(", IS ");
		out_hex(ufs_rd(UFS_IS));
		out("\n");
		ufs_errors();
		ufs_link_state();
		up = r == 0 && (ufs_rd(UFS_HCS) & 1);
	}
	if (!up) {
		out("  no link. The M-PHY as it is (read only):");
		ufs_kv(" 0x8c", rd32(PLAT_UFSPHY_BASE + 0x8c));
		ufs_kv(", 0xa09c", rd32(PLAT_UFSPHY_BASE + 0xa09c));
		ufs_kv(", 0xa0ac", rd32(PLAT_UFSPHY_BASE + 0xa0ac));
		ufs_kv(", 0xb044", rd32(PLAT_UFSPHY_BASE + 0xb044));
		out("\n");
		return 0;
	}
	out("  LINK UP, device present\n");
	for (unsigned i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
		out("  ");
		ufs_kv(attrs[i].name, ufs_dme_get(attrs[i].id));
		out("\n");
	}
	return 1;
}

/*
 * Stage B: talking to the device, read only. The doorbell list (legacy mode: CONFIG.QT stays 0) with one slot used,
 * a command descriptor and a buffer in the loader's own memory (MMU off: nothing to clean from a cache). In order:
 * NOP OUT until the device answers, fDeviceInit set and waited for (the device's own initialisation, which Android
 * asks for on every boot; no data is written), then on LUNs 0-7: READ CAPACITY(10) and READ(10) of block 1, where a
 * GPT header ("EFI PART") would be; for the first LUN that has one, its partition entries, printed.
 * Layouts: JESD223E (transfer request descriptor, PRDT) and JESD220 (UPIUs), as Linux's ufs.h/ufshci.h name them.
 */
#define UFS_UCD_RSP             512
#define UFS_UCD_PRDT            1024
#define UFS_BUF_SIZE            (6 * 4096)

static volatile uint32_t ufs_utrl[8 * 32] __attribute__((aligned(1024)));
static volatile uint32_t ufs_utmrl[20 * 8] __attribute__((aligned(1024)));
static volatile uint8_t ufs_ucd[2048] __attribute__((aligned(128)));
static volatile uint8_t ufs_buf[UFS_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t ufs_tag;

static uint32_t ufs_be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t ufs_le32(const volatile uint8_t *p)
{
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static uint64_t ufs_le64(const volatile uint8_t *p)
{
	return (uint64_t)ufs_le32(p + 4) << 32 | ufs_le32(p);
}

/* a fresh command descriptor: transaction type, task tag */
static volatile uint8_t *ufs_upiu(uint8_t type)
{
	for (unsigned i = 0; i < sizeof(ufs_ucd) / 4; i++) {
		((volatile uint32_t *)ufs_ucd)[i] = 0;
	}
	ufs_ucd[0] = type;
	ufs_ucd[3] = ufs_tag++ & 0x1f;
	return ufs_ucd;
}

/* slot 0: dd 0 none, 2 from the device; len bytes into ufs_buf. 0 when the controller says done (OCS 0). */
static int ufs_xfer(uint32_t dd, uint32_t len, uint32_t ms)
{
	uint64_t ucd = (uint64_t)(uintptr_t)ufs_ucd, buf = (uint64_t)(uintptr_t)ufs_buf;
	volatile uint32_t *d = ufs_utrl;
	uint32_t ocs;

	d[0] = (1u << 28) | (dd << 25) | (1u << 24);    /* UFS storage command type, direction, interrupt */
	d[1] = 0;
	d[2] = 0x0f;                                    /* OCS "invalid" until the controller writes it */
	d[3] = 0;
	d[4] = (uint32_t)ucd;
	d[5] = (uint32_t)(ucd >> 32);
	d[6] = (UFS_UCD_RSP / 4) << 16 | (512 / 4);     /* response UPIU: offset, length (dwords) */
	d[7] = (UFS_UCD_PRDT / 4) << 16 | (len ? 1 : 0);
	if (len) {
		volatile uint32_t *p = (volatile uint32_t *)(ufs_ucd + UFS_UCD_PRDT);

		p[0] = (uint32_t)buf;
		p[1] = (uint32_t)(buf >> 32);
		p[2] = 0;
		p[3] = len - 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(PLAT_UFSHCI_BASE + UFS_IS, ufs_rd(UFS_IS));
	wr32(PLAT_UFSHCI_BASE + UFS_UTRLDBR, 1);
	if (!ufs_wait(UFS_UTRLDBR, 1, 0, ms)) {
		out("  no completion in ");
		out_dec(ms);
		out(" ms (IS ");
		out_hex(ufs_rd(UFS_IS));
		out("): slot cleared\n");
		wr32(PLAT_UFSHCI_BASE + UFS_UTRLCLR, ~1u);
		ufs_wait(UFS_UTRLDBR, 1, 0, 100);
		return -1;
	}
	wr32(PLAT_UFSHCI_BASE + UFS_UTRLCNR, ufs_rd(UFS_UTRLCNR));
	ocs = d[2] & 0xff;
	if (ocs) {
		out("  OCS ");
		out_hex(ocs);
		out(" (IS ");
		out_hex(ufs_rd(UFS_IS));
		out(")\n");
		return -1;
	}
	return 0;
}

static int ufs_nop(void)
{
	volatile uint8_t *r = ufs_ucd + UFS_UCD_RSP;

	ufs_upiu(0x00);
	if (ufs_xfer(0, 0, 100)) {
		return -1;
	}
	if (r[0] != 0x20 || r[6]) {
		out("  answer: type ");
		out_hex(r[0]);
		out(", response ");
		out_hex(r[6]);
		out("\n");
		return -1;
	}
	return 0;
}

/* opcode 5 read flag (*value), 6 set flag. 0 on success. */
static int ufs_flag(uint8_t opcode, uint8_t idn, uint32_t *value)
{
	volatile uint8_t *u = ufs_upiu(0x16), *r = ufs_ucd + UFS_UCD_RSP;

	u[5] = opcode == 5 ? 0x01 : 0x81;               /* standard read / write request */
	u[12] = opcode;
	u[13] = idn;
	if (ufs_xfer(0, 0, 1500)) {
		return -1;
	}
	if (r[0] != 0x36 || r[6]) {
		out("  query answer: type ");
		out_hex(r[0]);
		out(", response ");
		out_hex(r[6]);
		out("\n");
		return -1;
	}
	if (value) {
		*value = r[23] & 1;
	}
	return 0;
}

/* a SCSI command reading len bytes (or none); the SCSI status (0 good, 2 check condition, sense printed), -1 if no answer */
static int ufs_scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, uint32_t len, int quiet)
{
	volatile uint8_t *u = ufs_upiu(0x01), *r = ufs_ucd + UFS_UCD_RSP;

	u[1] = len ? 0x40 : 0;                          /* read, simple task */
	u[2] = lun;
	u[12] = (uint8_t)(len >> 24);
	u[13] = (uint8_t)(len >> 16);
	u[14] = (uint8_t)(len >> 8);
	u[15] = (uint8_t)len;
	for (unsigned i = 0; i < cdb_len; i++) {
		u[16 + i] = cdb[i];
	}
	if (ufs_xfer(len ? 2 : 0, len, 2000)) {
		return -1;
	}
	if (r[0] != 0x21 || (r[6] && !r[7])) {
		out("  answer: type ");
		out_hex(r[0]);
		out(", response ");
		out_hex(r[6]);
		out("\n");
		return -1;
	}
	if (r[7] && !quiet) {
		/* fixed-format sense after its 2-byte length at 32: key in byte 2, ASC/ASCQ in 12/13 */
		out("  check condition: sense key ");
		out_hex(r[36] & 0xf);
		out(", ASC ");
		out_hex(r[46]);
		out("/");
		out_hex(r[47]);
		out("\n");
	}
	return r[7];
}

static int ufs_read10(uint8_t lun, uint32_t lba, uint16_t blocks, uint32_t bsize)
{
	uint8_t cdb[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba, 0,
			    (uint8_t)(blocks >> 8), (uint8_t)blocks, 0 };

	return ufs_scsi(lun, cdb, sizeof(cdb), blocks * bsize, 0);
}

static void ufs_gpt_print(uint8_t lun, uint32_t bsize)
{
	volatile uint8_t *h = ufs_buf;
	uint64_t entries_lba = ufs_le64(h + 72);
	uint32_t n = ufs_le32(h + 80), esz = ufs_le32(h + 84), shown = 0;
	uint32_t blocks = (n * esz + bsize - 1) / bsize;

	out("  LUN ");
	out_dec(lun);
	out(": GPT with ");
	out_dec(n);
	out(" entries of ");
	out_dec(esz);
	out(" bytes at block ");
	out_dec(entries_lba);
	out("\n");
	if (esz < 128 || esz > 512) {
		return;
	}
	if (blocks * bsize > UFS_BUF_SIZE) {
		blocks = UFS_BUF_SIZE / bsize;
	}
	if (ufs_read10(lun, (uint32_t)entries_lba, (uint16_t)blocks, bsize)) {
		out("  the entries could not be read\n");
		return;
	}
	for (uint32_t i = 0; i < n && (i + 1) * esz <= blocks * bsize; i++) {
		volatile uint8_t *e = ufs_buf + i * esz;
		uint64_t first = ufs_le64(e + 32), last = ufs_le64(e + 40);

		if (!ufs_le64(e) && !ufs_le64(e + 8)) {
			continue;       /* unused entry */
		}
		out(shown % 3 ? "   " : "\n   ");
		for (int k = 0; k < 36 && e[56 + 2 * k]; k++) {
			char c = (char)e[56 + 2 * k];
			out_putc(c >= 0x20 && c < 0x7f ? c : '?');
		}
		out(" ");
		out_dec(first);
		out("+");
		out_dec(last - first + 1);
		shown++;
	}
	out("\n  (name first-block+blocks; ");
	out_dec(shown);
	out(" partitions)\n");
}

/* the lists in and running, NOP OUT answered, fDeviceInit done: 1 when the device takes commands */
static int ufs_utp_start(void)
{
	uint64_t l = (uint64_t)(uintptr_t)ufs_utrl, t = (uint64_t)(uintptr_t)ufs_utmrl;
	uint32_t v = 1;
	int ok = -1;

	wr32(PLAT_UFSHCI_BASE + UFS_UTRLBA, (uint32_t)l);
	wr32(PLAT_UFSHCI_BASE + UFS_UTRLBAU, (uint32_t)(l >> 32));
	wr32(PLAT_UFSHCI_BASE + UFS_UTMRLBA, (uint32_t)t);
	wr32(PLAT_UFSHCI_BASE + UFS_UTMRLBAU, (uint32_t)(t >> 32));
	if (!ufs_wait(UFS_HCS, 0x6, 0x6, 100)) {
		out("  request lists not ready: HCS ");
		out_hex(ufs_rd(UFS_HCS));
		out("\n");
		return 0;
	}
	wr32(PLAT_UFSHCI_BASE + UFS_UTMRLRSR, 1);
	wr32(PLAT_UFSHCI_BASE + UFS_UTRLRSR, 1);
	out("  lists at ");
	out_hex(l);
	out(", running; NOP OUT");
	for (int i = 0; i < 10 && ok; i++) {
		ok = ufs_nop();
		out(ok ? " ." : " answered (NOP IN)\n");
	}
	if (ok) {
		out("\n  the device does not answer\n");
		return 0;
	}
	ufs_wdt_kick();
	out("  fDeviceInit <- 1: ");
	if (ufs_flag(6, 0x01, NULL)) {
		return 0;
	}
	for (int ms = 0; ms < 5000 && v; ms += 10) {
		if (ufs_flag(5, 0x01, &v)) {
			return 0;
		}
		if (v) {
			dbg_delay_ms(10);
		} else {
			out("device initialised after ");
			out_dec(ms);
			out(" ms\n");
		}
	}
	if (v) {
		out("still initialising after 5 s\n");
		return 0;
	}
	return 1;
}

/*
 * The gear change Android makes once the device is initialised (ufshcd_config_pwr_mode, with ufs-mediatek's pre-change
 * hook read off ufs-mediatek-mod.ko): every HS gear's sync length at least 0x48 on both ends (ufs_mtk_adjust_sync_length),
 * then, as the tree asks with mediatek,ufs-pmc-via-fastauto, HS-G1 rate B in FASTAUTO mode first, then the gear itself
 * in FAST mode, all connected lanes, rate B, TX adapt INITIAL from G4 on (UniPro 1.8 and later: host version 3) and the
 * data link's timeouts at Linux's defaults. Android asks for G5 and drops to G4 when it scales its clocks down; G4 here,
 * with ufs_sel as LK left it. 0 = stay in PWM-G1. Returns 1 when the link reports the change done (PWR_LOCAL).
 */
#ifndef UFS_HS_GEAR
#define UFS_HS_GEAR             4
#endif
#define UFS_IS_UPMS             (1u << 4)       /* UIC power mode status */

static uint32_t ufs_dme_peer_get(uint16_t id)
{
	uint32_t v = 0, r = ufs_uic(0x03, (uint32_t)id << 16, 0, 0, &v);

	return r ? 0xdead0000u | r : v;
}

static uint32_t ufs_dme_peer_set(uint16_t id, uint32_t v)
{
	return ufs_uic(0x04, (uint32_t)id << 16, 0, v, NULL);
}

/* PA_PWRMode <- mode, then the power mode change's own status: HCS.UPMCRS, 1 (PWR_LOCAL) = done */
static int ufs_pmc(uint32_t mode)
{
	uint32_t r, st;

	wr32(PLAT_UFSHCI_BASE + UFS_IS, UFS_IS_UPMS);
	r = ufs_dme_set(0x1571, mode);
	out("  PA_PWRMode <- ");
	out_hex(mode);
	if (r) {
		out(": refused, result ");
		out_hex(r);
		out("\n");
		return 0;
	}
	if (!ufs_wait(UFS_IS, UFS_IS_UPMS, UFS_IS_UPMS, 500)) {
		out(": no power mode status in 500 ms (IS ");
		out_hex(ufs_rd(UFS_IS));
		out(")\n");
		return 0;
	}
	wr32(PLAT_UFSHCI_BASE + UFS_IS, UFS_IS_UPMS);
	st = (ufs_rd(UFS_HCS) >> 8) & 7;
	out(": status ");
	out_dec(st);
	out(st == 1 ? " (done)\n" : " (1 would be done; 2 remote, 3 busy, 4 capability, 5 fatal)\n");
	return st == 1;
}

/* the settings both of Android's steps make before PA_PWRMode, for this gear */
static uint32_t ufs_pmc_attrs(uint32_t gear, uint32_t lanes_rx, uint32_t lanes_tx)
{
	static const struct { uint16_t id; uint16_t v; } timeouts[] = {
		{ 0x15b0, 8191 }, { 0x15b1, 65535 }, { 0x15b2, 32767 },        /* PA_PWRModeUserData0-5 */
		{ 0x15b3, 8191 }, { 0x15b4, 65535 }, { 0x15b5, 32767 },
		{ 0xd041, 8191 }, { 0xd042, 65535 }, { 0xd043, 32767 },        /* DME_LocalFC0/TC0/AFC0 timeouts */
	};
	uint32_t r = 0;

	r |= ufs_dme_set(0x1583, gear);         /* PA_RxGear */
	r |= ufs_dme_set(0x1580, lanes_rx);     /* PA_ActiveRxDataLanes */
	r |= ufs_dme_set(0x1584, 1);            /* PA_RxTermination */
	r |= ufs_dme_set(0x1568, gear);         /* PA_TxGear */
	r |= ufs_dme_set(0x1560, lanes_tx);     /* PA_ActiveTxDataLanes */
	r |= ufs_dme_set(0x1569, 1);            /* PA_TxTermination */
	r |= ufs_dme_set(0x156a, 2);            /* PA_HSSeries: rate B */
	for (unsigned i = 0; i < sizeof(timeouts) / sizeof(timeouts[0]); i++) {
		r |= ufs_dme_set(timeouts[i].id, timeouts[i].v);
	}
	return r;
}

static int __attribute__((unused)) ufs_hs_gear(void)
{
	static const uint16_t sync[] = { 0x1552, 0x1554, 0x1556, 0x15d0, 0x15d6 };   /* PA_TxHsG1..G5SyncLength */
	uint32_t lanes_tx = ufs_dme_get(0x1561), lanes_rx = ufs_dme_get(0x1581);
	uint32_t max_rx = ufs_dme_get(0x1587), max_tx = ufs_dme_peer_get(0x1587);       /* PA_MaxRxHSGear, ours and its */
	uint32_t gear = UFS_HS_GEAR, r;
	int ok;

	if ((lanes_tx | lanes_rx | max_rx | max_tx) & 0xdead0000u) {
		out("UFS gear: the link's attributes could not be read\n");
		return 0;
	}
	gear = gear < max_rx ? gear : max_rx;
	gear = gear < max_tx ? gear : max_tx;
	out("UFS gear: to HS-G");
	out_dec(gear);
	out(" rate B, ");
	out_dec(lanes_rx);
	out("+");
	out_dec(lanes_tx);
	out(" lanes (the host takes up to G");
	out_dec(max_rx);
	out(", the device up to G");
	out_dec(max_tx);
	out(")\n");
	if (gear == 0) {
		return 0;
	}
	ufs_wdt_kick();
	out("  sync lengths (ours/its, at least 0x48):");
	for (unsigned i = 0; i < sizeof(sync) / sizeof(sync[0]); i++) {
		uint32_t a = ufs_dme_get(sync[i]), b = ufs_dme_peer_get(sync[i]);

		if (a < 0x48) {
			ufs_dme_set(sync[i], 0x48);
		}
		if (b < 0x48) {
			ufs_dme_peer_set(sync[i], 0x48);
		}
		out(" ");
		out_hex(a);
		out("/");
		out_hex(b);
	}
	out("\n  HS-G1 rate B, FASTAUTO (MediaTek's pmc via fastauto):\n");
	r = ufs_pmc_attrs(1, lanes_rx, lanes_tx) | ufs_dme_set(0x15d4, 3);      /* PA_TxHsAdaptType: none */
	if (r) {
		out("  some settings refused: ");
		out_hex(r);
		out("\n");
	}
	if (!ufs_pmc(0x44)) {
		out("  (Android goes on after this too)\n");
	}
	ufs_wdt_kick();
	out("  HS-G");
	out_dec(gear);
	out(" rate B, FAST:\n");
	r = ufs_dme_set(0x15d4, gear >= 4 ? 1 : 3) | ufs_pmc_attrs(gear, lanes_rx, lanes_tx);
	if (r) {
		out("  some settings refused: ");
		out_hex(r);
		out("\n");
	}
	ok = ufs_pmc(0x11);
	out("  now");
	ufs_kv(" PA_PWRMode", ufs_dme_get(0x1571));
	ufs_kv(", PA_TxGear", ufs_dme_get(0x1568));
	ufs_kv(", PA_RxGear", ufs_dme_get(0x1583));
	ufs_kv(", PA_HSSeries", ufs_dme_get(0x156a));
	out("\n");
	ufs_errors();
	ufs_link_state();
	return ok && (ufs_rd(UFS_HCS) & 1);
}

static void __attribute__((unused)) ufs_stage_b(void)
{
	int gpt_done = 0;

	out("UFS stage B: talking to the device (read only)\n");
	if (!ufs_utp_start()) {
		return;
	}
	for (uint8_t lun = 0; lun < 8; lun++) {
		static const uint8_t rcap[10] = { 0x25 };
		int s = -1;

		ufs_wdt_kick();
		for (int i = 0; i < 3 && s != 0; i++) {
			s = ufs_scsi(lun, rcap, sizeof(rcap), 8, i < 2);    /* the first answers may be "unit attention" */
		}
		if (s) {
			out("  LUN ");
			out_dec(lun);
			out(": no capacity (status ");
			out_hex((uint32_t)s);
			out(")\n");
			continue;
		}
		uint32_t last = ufs_be32(ufs_buf), bsize = ufs_be32(ufs_buf + 4);
		out("  LUN ");
		out_dec(lun);
		out(": ");
		out_dec(last + 1ULL);
		out(" blocks of ");
		out_dec(bsize);
		out(" bytes (");
		out_dec(((uint64_t)last + 1) * bsize >> 20);
		out(" MB)");
		if (bsize < 512 || bsize > 4096) {
			out("\n");
			continue;
		}
		if (ufs_read10(lun, 1, 1, bsize)) {
			out(", block 1 not read\n");
			continue;
		}
		if (ufs_le32(ufs_buf) == 0x20494645u && ufs_le32(ufs_buf + 4) == 0x54524150u) {     /* "EFI PART" */
			out(", GPT\n");
			if (!gpt_done) {
				ufs_gpt_print(lun, bsize);
				gpt_done = 1;
			}
		} else {
			out(", no GPT at block 1 (");
			ufs_kv("starts", ufs_le32(ufs_buf));
			out(")\n");
		}
	}
}

static void __attribute__((unused)) ufs_stage3(void)
{
	/* the watchdog, and the marker that keeps this log over the reset it would make (mtkloader_main: MARK_DONE) */
	marker_set(MARK_DONE);
	wr32(TOPRGU_LENGTH, ((31u * 32768u / 512u) << 5) | 0x8u);
	wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
	wdt_mode_set(1);
	out("watchdog armed (31 s) while the UFS stages run\n");
	if (ufs_bringup()) {
		ufs_stage_b();
		wr32(PLAT_UFSHCI_BASE + UFS_UTRLRSR, 0);
		wr32(PLAT_UFSHCI_BASE + UFS_UTMRLRSR, 0);
	}
	out("UFS: HCE <- 0 (Android resets and brings it up again itself)\n");
	wr32(PLAT_UFSHCI_BASE + UFS_HCE, 0);
	ufs_wait(UFS_HCE, 1, 0, 100);
	wdt_disable();
	marker_set(0);
}
#endif /* UFS_PROBE >= 3 || XNU_UFS */

#if XNU_UFS
/* the partition named so in LUN 2's GPT (blocks of bsize): ufs_xnu_lba, ufs_xnu_blocks */
static int ufs_find_part(const char *name, uint32_t bsize)
{
	volatile uint8_t *h = ufs_buf;
	uint32_t n, esz, blocks;
	uint64_t entries_lba;

	if (ufs_read10(2, 1, 1, bsize) || ufs_le32(h) != 0x20494645u || ufs_le32(h + 4) != 0x54524150u) {
		out("  no GPT on LUN 2\n");
		return 0;
	}
	entries_lba = ufs_le64(h + 72);
	n = ufs_le32(h + 80);
	esz = ufs_le32(h + 84);
	if (esz != 128) {
		out("  GPT entries of an unexpected size\n");
		return 0;
	}
	blocks = (n * esz + bsize - 1) / bsize;
	if (blocks * bsize > UFS_BUF_SIZE) {
		blocks = UFS_BUF_SIZE / bsize;
	}
	if (ufs_read10(2, (uint32_t)entries_lba, (uint16_t)blocks, bsize)) {
		out("  the GPT's entries could not be read\n");
		return 0;
	}
	for (uint32_t i = 0; i < n && (i + 1) * esz <= blocks * bsize; i++) {
		volatile uint8_t *e = ufs_buf + i * esz;
		int k;

		for (k = 0; name[k] && e[56 + 2 * k] == (uint8_t)name[k] && e[57 + 2 * k] == 0; k++) {
		}
		if (name[k] || e[56 + 2 * k] || e[57 + 2 * k]) {
			continue;
		}
		ufs_xnu_lba = ufs_le64(e + 32);
		ufs_xnu_blocks = ufs_le64(e + 40) - ufs_xnu_lba + 1;
		out("  partition ");
		out(name);
		out(": block ");
		out_dec(ufs_xnu_lba);
		out(", ");
		out_dec(ufs_xnu_blocks);
		out(" blocks (");
		out_dec(ufs_xnu_blocks * bsize >> 20);
		out(" MB)\n");
		return ufs_xnu_lba != 0 && ufs_xnu_blocks != 0;
	}
	out("  no partition named ");
	out(name);
	out(" in LUN 2's GPT\n");
	return 0;
}

/* the sum the kernel's ufs_test prints too: over the partition's first 4 MB, 64-bit words in order */
static int ufs_part_sum(uint32_t bsize)
{
	uint64_t sum = 0xcbf29ce484222325ULL, len = ufs_xnu_blocks * bsize, t0 = dbg_ticks();
	uint32_t per = UFS_BUF_SIZE / bsize, done = 0, total;

	if (len > 4 * 1024 * 1024) {
		len = 4 * 1024 * 1024;
	}
	total = (uint32_t)(len / bsize);
	while (done < total) {
		uint32_t n = total - done < per ? total - done : per;

		if (ufs_read10(2, (uint32_t)(ufs_xnu_lba + done), (uint16_t)n, bsize)) {
			out("  sum: read failed at block ");
			out_dec(done);
			out("\n");
			return 0;
		}
		for (uint32_t i = 0; i < n * bsize; i += 8) {
			uint64_t w = (uint64_t)ufs_le32(ufs_buf + i + 4) << 32 | ufs_le32(ufs_buf + i);

			sum = (sum ^ w) * 0x100000001b3ULL;
		}
		if (done == 0) {
			out("  starts");
			for (int i = 0; i < 8; i++) {
				out(" ");
				out_hex(ufs_buf[i]);
			}
			out("\n");
		}
		done += n;
		ufs_wdt_kick();
	}
	out("  sum of its first ");
	out_dec(len >> 10);
	out(" KB: ");
	out_hex(sum);
	out(" in ");
	out_dec((dbg_ticks() - t0) * 1000 / dbg_freq());
	out(" ms (the kernel's ufs_test must print the same)\n");
	return 1;
}

/* before XNU: the UFS up, the partition found; the lists stopped, the host left enabled with the link up */
static int ufs_for_xnu(void)
{
	static const uint8_t rcap[10] = { 0x25 };
	int ok = 0;

	marker_set(MARK_DONE);
	wr32(TOPRGU_LENGTH, ((31u * 32768u / 512u) << 5) | 0x8u);
	wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
	wdt_mode_set(1);
	out("UFS for XNU: bring-up, then the partition " XNU_UFS_PART " of LUN 2 (read only; watchdog 31 s)\n");
	/* first in the HS gear; if that or a read in it fails, the way back: all of it again in PWM-G1 (run 7) */
	for (int slow = UFS_HS_GEAR == 0; slow < 2 && !ok; slow++) {
		int st = -1;

		if (slow && UFS_HS_GEAR != 0) {
			out("UFS gear: back to PWM-G1, the bring-up again\n");
			wr32(PLAT_UFSHCI_BASE + UFS_UTRLRSR, 0);
			wr32(PLAT_UFSHCI_BASE + UFS_UTMRLRSR, 0);
			wr32(PLAT_UFSHCI_BASE + UFS_HCE, 0);
			ufs_wait(UFS_HCE, 1, 0, 100);
		}
		if (!ufs_bringup() || !ufs_utp_start()) {
			break;
		}
		if (!slow && !ufs_hs_gear()) {
			continue;
		}
		for (int i = 0; i < 3 && st != 0; i++) {
			st = ufs_scsi(2, rcap, sizeof(rcap), 8, i < 2);
		}
		uint32_t bsize = ufs_be32(ufs_buf + 4);
		if (st != 0 || bsize != 4096) {
			out("  LUN 2: no capacity, or blocks not of 4 KB\n");
		} else if (ufs_find_part(XNU_UFS_PART, bsize)) {
			ok = !XNU_UFS_TEST || ufs_part_sum(bsize);
		}
	}
	wr32(PLAT_UFSHCI_BASE + UFS_UTRLRSR, 0);
	wr32(PLAT_UFSHCI_BASE + UFS_UTMRLRSR, 0);
	if (ok) {
		out("UFS: left up for the kernel (HCE ");
		out_hex(ufs_rd(UFS_HCE));
		out(", HCS ");
		out_hex(ufs_rd(UFS_HCS));
		out(", lists stopped)\n");
	} else {
		out("UFS: not for the kernel this time; HCE <- 0\n");
		wr32(PLAT_UFSHCI_BASE + UFS_HCE, 0);
		ufs_wait(UFS_HCE, 1, 0, 100);
	}
	wdt_disable();
	marker_set(0);
	return ok;
}
#endif /* XNU_UFS */
#endif /* UFS_PROBE || XNU_UFS */

void mtkloader_main(uint64_t dtb)
{
	uint32_t mark = marker_get();

	struct lk_facts lk;

	wdt_disable();
	lk_dtb = dtb;
	/* before anything is printed: where LK put its framebuffer and ramoops (mtkfdt.h) */
	lk_facts_read(dtb, &lk);
	if (lk.have_fb) {
		lk_fb_base = lk.fb_base;
		lk_fb_size = lk.fb_size;
	}
	if (lk.have_ramoops) {
		lk_facts_ramoops_zones(&lk, &ramoops_console, &ramoops_console_size, &ramoops_pmsg, &ramoops_pmsg_size);
	}
	display_find();
	fbcon_init(fb_base, fb_pitch, 0, PLAT_FB_SCALE);

	if (mark == MARK_DONE) {
		/* XNU's log was saved when it came back; leave ramoops as it is. */
		chain_android();
	}
	if (mark == MARK_RUNNING) {
		rescue_log();
		dbg_delay_ms(5000);
		chain_android();
	}

	plog_reset();
	if (!boot_choice()) {
		chain_android();
	}
	fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
	fbc.x = 0;
	fbc.y = fbc.top = 0;
	fbc.fg = 0xff40ff40u;
	out("== mtkloader: Darwin/XNU on " PLAT_NAME " ==\n");
	fbc.fg = 0xffffffffu;
	out("LK dtb ");
	out_hex(dtb);
	out("\nLK framebuffer ");
	if (lk.have_fb) {
		out_hex(lk.fb_base);
		out(" + ");
		out_hex(lk.fb_size);
		out(", ");
		out_dec(lk.fb_fps / 100);       /* atag,videolfb keeps hundredths: 6000 is 60 Hz */
		out(" Hz, panel ");
		out(lk.fb_lcm_found ? lk.lcm_name : "(LK found none)");
	} else {
		out("not in LK's tree (no chosen/atag,videolfb)");
	}
	out("\nramoops ");
	if (lk.have_ramoops) {
		out_hex(lk.ramoops_base);
		out(" + ");
		out_hex(lk.ramoops_size);
		out(": console ");
		out_hex(ramoops_console);
		out(" + ");
		out_hex(ramoops_console_size);
		out(", pmsg ");
		out_hex(ramoops_pmsg);
		out(" + ");
		out_hex(ramoops_pmsg_size);
	} else {
		out("not in LK's tree: this run's log is not kept");
	}
	out("\n");
#if CONNSYS_REPORT
	connsys_dt_report(dtb);         /* Wi-Fi step 1: what Android's tree says of CONNSYS (mtkconnsys.h) */
#endif
#if UFS_PROBE
	ufs_probe();
#if UFS_PROBE >= 3
	ufs_stage3();
#else
	if (UFS_PROBE >= 2) {
		ufs_link_test();
	}
#endif
	out("UFS probe done; starting Android (no XNU this time)\n");
	dbg_delay_ms(3000);
	chain_android();
#endif
#if TOUCH_PROBE
	touch_probe();
	out("touch probe done; starting Android (no XNU this time)\n");
	dbg_delay_ms(2000);
	chain_android();
#endif
#if GPU_PROBE
	gpu_probe();
	out("GPU probe done; starting Android (no XNU this time)\n");
	dbg_delay_ms(3000);
	chain_android();
#endif
#if XNU_UFS
	ufs_xnu_ok = ufs_for_xnu();
#endif
#if XNU_TOUCH
	{
		uint32_t lk_mode[4];

		touch_bringup(lk_mode);         /* the SPI pins stay GPIOs: the kernel drives them by hand */
	}
#endif
	run_xnu();
}
