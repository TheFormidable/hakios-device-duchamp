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
_Static_assert(XNU_CORES >= 1 && XNU_CORES <= 7, "one core at least, and one left for the watcher");

/*
 * The processors in the order the kernel is offered them: the one this code runs on first (LK
 * leaves us on core 0), then the two Cortex-A75s, which are about twice the Cortex-A55s, then
 * the rest of the A55s. So "cpus=3" already gives iOS both fast cores.
 */
static const uint8_t core_order[8] = PLAT_CORE_ORDER;
#define WATCH_CORE          core_order[XNU_CORES]
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
/* 1: wake the GPU and ask it what it is, if Android asked for it (tools/gpu-probe.sh); then Android. */
#ifndef GPU_PROBE
#define GPU_PROBE           0
#endif
/* 1: read the camera's pins, rails and I2C controller and report, if Android asked for it (tools/cam-probe.sh); then Android. */
#ifndef CAM_PROBE
#define CAM_PROBE           0
#endif
/* 1: read the eMMC and report, then go back to Android (no XNU). */
#ifndef MSDC_TEST
#define MSDC_TEST           0
#endif
/* 1: check at EL1 that the timer interrupts arrive at all (diagnostics, leaves GIC state). */
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
#if XNU_ROOT_UFS
#define XNU_ROOT_ARG        "rd=md0 ufs_root=1"
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
	    hk_keep, hk_keep_len, hk_keep_dropped;
};

static struct xnu_syms xsyms; /* of the kernel that was started; kept for when it comes back */
static const uint8_t *xkernel;  /* its Mach-O file, for symbols */
/* XNU's x29, x30, TCR_EL1 and TTBR1_EL1 when it came back (start.S) */
uint64_t xnu_fp[4];
static volatile uint64_t xnu_back __attribute__((aligned(64))); /* set by xnu_returned() */

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
		if (n[0] != '_' || (n[1] != 'l' && n[1] != 'v' && n[1] != 'd' && n[1] != 'i' && n[1] != 'm' && n[1] != 'c' && n[1] != 'g' && n[1] != 'n' && n[1] != 't' && n[1] != 'r' && n[1] != 'h')) {
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
				char c = sp[i].p[k];
				plog_putc(c ? c : ' ');
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
 * granule), as they were when it came back. 0 if not mapped.
 */
static uint64_t xnu_va_to_pa(uint64_t va)
{
	uint64_t tcr = xnu_fp[2], ttbr1 = xnu_fp[3];
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
		uint64_t d = *(volatile uint64_t *)(table + idx * 8);
		if (!(d & 1)) {
			return 0;
		}
		uint64_t oa = d & 0x0000ffffffffffffULL & ~((1ULL << g) - 1);
		if (l == levels - 1 || !(d & 2)) { /* page, or block */
			uint64_t span = 1ULL << shift;
			return (oa & ~(span - 1)) | (va & (span - 1));
		}
		table = oa;
	}
	return 0;
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
static void out_sym(uint64_t addr)
{
	const struct mach_header_64 *mh = (const struct mach_header_64 *)xkernel;
	const struct load_command *lc;
	const struct symtab_command *st = NULL;
	uint64_t best = 0;
	const char *best_name = NULL;

	out_hex(addr);
	if (!xkernel) {
		return;
	}
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SYMTAB) {
			st = (const void *)lc;
		}
	}
	if (!st) {
		return;
	}
	const struct nlist_64 *sym = (const struct nlist_64 *)(xkernel + st->symoff);
	const char *strs = (const char *)(xkernel + st->stroff);
	for (uint32_t i = 0; i < st->nsyms; i++) {
		if ((sym[i].n_type & 0x0e) == 0x0e && sym[i].n_value <= addr && sym[i].n_value > best) { /* N_SECT */
			best = sym[i].n_value;
			best_name = strs + sym[i].n_strx;
		}
	}
	if (best_name && addr - best < 0x100000) {
		out(" ");
		out(best_name);
		out("+");
		out_hex(addr - best);
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
void watcher_main(void)
{
	for (int i = 0; i < 512; i++) {
		uint64_t pa = (uint64_t)i << 30;
		if (i == 0) {
			watcher_l1[i] = pa | (1u << 10) | (0u << 2) | 1;            /* MMIO: device */
		} else if (i < 8) {
			watcher_l1[i] = pa | (1u << 10) | (3u << 8) | (1u << 2) | 1; /* DRAM: normal WB, inner shareable */
		} else {
			watcher_l1[i] = 0;
		}
	}
	mmu_on((uint64_t)watcher_l1);
	watcher_started = 1;
	dcache_clean_range((uint64_t)&watcher_started, 8);

	uint64_t end = dbg_ticks() + dbg_freq() * XNU_WATCH_SECONDS;
	while (dbg_ticks() < end) {
		dcache_clean_range((uint64_t)&xnu_back, 8);
		if (xnu_back) {
			for (;;) {
				__asm__ volatile("wfe");
			}
		}
		dbg_delay_ms(100);
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
	/* Where the first core is: find its PMU in the cpu_dbgapb window, sample PMPCSR. */
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
	/*
	 * Is the virtual timer's PPI 27 even reaching the first core? GICR frame 0 is its
	 * redistributor (SGI/PPI page at +0x10000); pending but not active means the GIC latched
	 * the timer and the CPU interface refused it, nothing pending means no timer at all.
	 */
	{
		const uint64_t gicd = PLAT_GICD_BASE, gicr = PLAT_GICR_BASE, sgi = gicr + 0x10000;
		dbg_puts("GICD_CTLR ");
		dbg_hex(rd32(gicd + 0x000), 8);
		dbg_puts(" TYPER ");
		dbg_hex(rd32(gicd + 0x004), 8);
		dbg_puts("\nGICR_CTLR ");
		dbg_hex(rd32(gicr + 0x000), 8);
		dbg_puts(" WAKER ");
		dbg_hex(rd32(gicr + 0x014), 8);
		dbg_puts(" TYPER ");
		dbg_hex(rd32(gicr + 0x008), 8);
		dbg_putc(' ');
		dbg_hex(rd32(gicr + 0x00c), 8);
		dbg_puts("\nGICR_SGI: IGROUPR0 ");
		dbg_hex(rd32(sgi + 0x080), 8);
		dbg_puts(" IGRPMODR0 ");
		dbg_hex(rd32(sgi + 0xd00), 8);
		dbg_puts(" ISENABLER0 ");
		dbg_hex(rd32(sgi + 0x100), 8);
		dbg_puts("\n          ISPENDR0 ");
		dbg_hex(rd32(sgi + 0x200), 8);
		dbg_puts(" ISACTIVER0 ");
		dbg_hex(rd32(sgi + 0x300), 8);
		dbg_puts(" IPRIORITY27 ");
		dbg_hex(*(volatile uint8_t *)(sgi + 0x400 + 27), 2);
		dbg_puts(" ICFGR1 ");
		dbg_hex(rd32(sgi + 0xc04), 8);
		dbg_puts("\n");
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
	if (xsyms.msgbufp && xnu_fp[3]) {
		uint64_t mb = xnu_rd64(xsyms.msgbufp);
		uint64_t w0 = xnu_rd64(mb), w1 = xnu_rd64(mb + 8), bufc = xnu_rd64(mb + 16);
		int32_t size = (int32_t)(w0 >> 32), bufx = (int32_t)w1;
		dbg_puts("--- kernel message buffer ---\n");
		if (size > 0 && size <= 1024 * 1024 && bufx >= 0 && bufx <= size) {
			for (int pass = 0; pass < 2; pass++) {
				int from = pass == 0 ? bufx : 0, to = pass == 0 ? size : bufx;
				for (int i = from; i < to; i++) {
					char c = xnu_rd8(bufc + i);
					if (c) {
						dbg_putc(c);
					}
				}
			}
		}
		dbg_puts("\n--- end of message buffer ---\n");
	}
	if (xsyms.log && xsyms.log_len) {
		uint32_t len = rd32(kva_to_pa(xsyms.log_len));
		struct span sp = { (const volatile char *)kva_to_pa(xsyms.log), len > 256 * 1024 ? 256 * 1024 : len };
		dbg_puts("--- loader_log (head) ---\n");
		save_log(&sp, 1);
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

static void __attribute__((noreturn)) run_xnu(void)
{
	struct payload_info pi;
	struct xnu_syms s;

	if (!parse_payload(&pi)) {
		out("ERROR: no XNU payload in this image\n");
		dbg_delay_ms(5000);
		chain_android();
	}
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
	 * (and mt6577_uart_putc, which the kernel calls as well and which drives this UART right).
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
	ba->video.v_height = PANEL_H;
	ba->video.v_depth = 32;
	memcpy(ba->commandLine, XNU_BOOT_ARGS, sizeof(XNU_BOOT_ARGS));

	out("md0 ");
	out_hex(sys_size ? sys_pa : rd_pa);
	out(sys_size ? " (the system image), md1 " : XNU_ROOT_EMMC ? " is none: md1 (root.packfs), the system image on the eMMC" :
	    " (root.packfs only)");
	if (sys_size) {
		out_hex(rd_pa);
	}
	out("\nboot-args: " XNU_BOOT_ARGS "\nframebuffer ");
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
		wr32(TOPRGU_MODE, TOPRGU_MODE_KEY | 0x1u);
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
		out_dec(lk.fb_fps);
		out(" fps, panel ");
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
	connsys_dt_report(dtb);         /* Wi-Fi step 1: what Android's tree says of CONNSYS (mtkconnsys.h) */
	run_xnu();
}
