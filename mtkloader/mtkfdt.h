/*
 * mtkfdt.h: what LK's own device tree says that the shipped one does not -- read at run time, before anything is printed.
 *
 * On the MT6768 loader these were constants found by hand on the one phone (ramoops at 0x4d010000, LK's framebuffer at
 * 0x787b0000). On the MT6897 LK places both at boot, so the loader asks the tree LK hands over in x0 -- the very tree Android
 * boots with -- instead of guessing:
 *
 *   /chosen/atag,videolfb       LK's framebuffer, as MediaTek's display driver reads it (mtk_drm_drv.c, _parse_tag_videolfb):
 *                               u64 fb_base, u32 islcmfound, u32 fps, u32 vram (bytes), char lcmname[] -- little endian, as LK
 *                               copies its own struct in
 *   compatible = "ramoops"      reg (the parent's cells), record-size, console-size, ftrace-size, pmsg-size: the zones are laid
 *                               out as Linux's ramoops_probe() does it -- dump records from the start, then console, ftrace,
 *                               pmsg, each at the end of what is left
 *   /chosen/bootargs            ... or, as duchamp's LK does it (seen on the phone, 11 November): no ramoops node -- only
 *                               "mediatek,pstore" in reserved-memory -- and ramoops.mem_address=, mem_size=, console_size=,
 *                               pmsg_size= on the command line; record_size and ftrace_size then keep Linux's default, 4096
 *
 * Read only; uses the walker helpers of mtkconnsys.h (dt_be32, dt_streq, dt_contains, dt_len) and memcmp_n.
 */
#ifndef MTKFDT_H
#define MTKFDT_H

struct lk_facts {
	uint64_t fb_base, fb_size;
	uint32_t fb_fps, fb_lcm_found;
	const char *lcm_name;
	uint64_t ramoops_base, ramoops_size;
	uint32_t record_size, console_size, ftrace_size, pmsg_size;
	int have_fb, have_ramoops;
};

static uint64_t fdt_cells(const uint8_t *p, uint32_t cells)
{
	return cells == 2 ? ((uint64_t)dt_be32(p) << 32 | dt_be32(p + 4)) : dt_be32(p);
}

static uint64_t fdt_le64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--) {
		v = v << 8 | p[i];
	}
	return v;
}

static uint32_t fdt_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* the number after `key` in a command line ("ramoops.mem_address=0x48090000"), 0x or decimal; 0 if absent */
static uint64_t bootarg_num(const char *line, uint32_t len, const char *key)
{
	uint32_t kl = dt_len(key);

	for (uint32_t i = 0; i + kl < len; i++) {
		if ((i == 0 || line[i - 1] == ' ') && !memcmp_n(line + i, key, kl)) {
			const char *p = line + i + kl;
			uint64_t v = 0;
			int base = 10;

			if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
				base = 16;
				p += 2;
			}
			for (; p < line + len; p++) {
				int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
				    *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : 99;
				if (d >= base) {
					break;
				}
				v = v * (uint64_t)base + (uint64_t)d;
			}
			return v;
		}
	}
	return 0;
}

static void lk_facts_read(uint64_t dtb, struct lk_facts *f)
{
	const uint8_t *fdt = (const uint8_t *)(uintptr_t)dtb;
	const uint8_t *p, *end;
	const char *strings;
	struct {
		const char *name;
		uint32_t address_cells, size_cells;     /* for this node's children */
		const uint8_t *reg;
		uint32_t reg_len, record, console, ftrace, pmsg;
		int ramoops;
	} lv[24];
	int depth = -1;
	const char *bootargs = NULL;
	uint32_t bootargs_len = 0;

	memset(f, 0, sizeof(*f));
	if (dtb == 0 || dt_be32(fdt) != FDT_MAGIC) {
		return;
	}
	p = fdt + dt_be32(fdt + 8);
	strings = (const char *)(fdt + dt_be32(fdt + 12));
	end = p + dt_be32(fdt + 36);
	while (p < end) {
		uint32_t token = dt_be32(p);

		p += 4;
		if (token == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;

			p += (dt_len(name) + 1 + 3) & ~3u;
			if (++depth >= 24) {
				return;
			}
			memset(&lv[depth], 0, sizeof(lv[depth]));
			lv[depth].name = name;
			lv[depth].address_cells = 2;
			lv[depth].size_cells = 1;
		} else if (token == FDT_PROP) {
			uint32_t len = dt_be32(p);
			const char *prop = strings + dt_be32(p + 4);
			const uint8_t *v = p + 8;

			p += 8 + ((len + 3) & ~3u);
			if (depth < 0) {
				continue;
			}
			if (dt_streq(prop, "#address-cells") && len == 4) {
				lv[depth].address_cells = dt_be32(v);
			} else if (dt_streq(prop, "#size-cells") && len == 4) {
				lv[depth].size_cells = dt_be32(v);
			} else if (dt_streq(prop, "compatible") && dt_contains((const char *)v, len, "ramoops")) {
				lv[depth].ramoops = 1;
			} else if (dt_streq(prop, "reg")) {
				lv[depth].reg = v;
				lv[depth].reg_len = len;
			} else if (dt_streq(prop, "record-size") && len == 4) {
				lv[depth].record = dt_be32(v);
			} else if (dt_streq(prop, "console-size") && len == 4) {
				lv[depth].console = dt_be32(v);
			} else if (dt_streq(prop, "ftrace-size") && len == 4) {
				lv[depth].ftrace = dt_be32(v);
			} else if (dt_streq(prop, "pmsg-size") && len == 4) {
				lv[depth].pmsg = dt_be32(v);
			} else if (depth == 1 && dt_streq(lv[1].name, "chosen") && dt_streq(prop, "bootargs")) {
				bootargs = (const char *)v;
				bootargs_len = len;
			} else if (depth == 1 && dt_streq(lv[1].name, "chosen") && dt_streq(prop, "atag,videolfb") && len >= 20) {
				f->fb_base = fdt_le64(v);
				f->fb_lcm_found = fdt_le32(v + 8);
				f->fb_fps = fdt_le32(v + 12);
				f->fb_size = fdt_le32(v + 16);
				f->lcm_name = len > 20 ? (const char *)(v + 20) : "";
				f->have_fb = f->fb_base != 0 && f->fb_size != 0;
			}
		} else if (token == FDT_END_NODE) {
			if (depth < 0) {
				break;
			}
			if (lv[depth].ramoops && lv[depth].reg && depth > 0 && !f->have_ramoops) {
				uint32_t ac = lv[depth - 1].address_cells, sc = lv[depth - 1].size_cells;

				if ((ac == 1 || ac == 2) && (sc == 1 || sc == 2) && lv[depth].reg_len >= 4 * (ac + sc)) {
					f->ramoops_base = fdt_cells(lv[depth].reg, ac);
					f->ramoops_size = fdt_cells(lv[depth].reg + 4 * ac, sc);
					f->record_size = lv[depth].record;
					f->console_size = lv[depth].console;
					f->ftrace_size = lv[depth].ftrace;
					f->pmsg_size = lv[depth].pmsg;
					f->have_ramoops = f->ramoops_size != 0 &&
					    (uint64_t)f->console_size + f->ftrace_size + f->pmsg_size <= f->ramoops_size;
				}
			}
			depth--;
		} else if (token == FDT_NOP) {
			continue;
		} else {
			break;
		}
	}
	if (!f->have_ramoops && bootargs) {
		f->ramoops_base = bootarg_num(bootargs, bootargs_len, "ramoops.mem_address=");
		f->ramoops_size = bootarg_num(bootargs, bootargs_len, "ramoops.mem_size=");
		f->console_size = (uint32_t)bootarg_num(bootargs, bootargs_len, "ramoops.console_size=");
		f->pmsg_size = (uint32_t)bootarg_num(bootargs, bootargs_len, "ramoops.pmsg_size=");
		f->record_size = (uint32_t)bootarg_num(bootargs, bootargs_len, "ramoops.record_size=");
		f->ftrace_size = (uint32_t)bootarg_num(bootargs, bootargs_len, "ramoops.ftrace_size=");
		if (!f->record_size) {
			f->record_size = 4096;          /* Linux's default (fs/pstore/ram.c), as the phone reports it */
		}
		if (!f->ftrace_size) {
			f->ftrace_size = 4096;
		}
		f->have_ramoops = f->ramoops_base != 0 && f->ramoops_size != 0 &&
		    (uint64_t)f->console_size + f->ftrace_size + f->pmsg_size <= f->ramoops_size;
	}
}

/*
 * The console and pmsg zones, walked forward as Linux's ramoops_probe() lays them out: the dump records take
 * dump_mem_sz = size - console - ftrace - pmsg, cut into dump_mem_sz / record_size zones of dump_mem_sz / count each (the
 * remainder is skipped, which is why this does not simply count back from the end); then console, then ftrace (one zone
 * unless per-CPU, the same length either way when it divides), then pmsg.
 */
static void lk_facts_ramoops_zones(const struct lk_facts *f, uint64_t *console, uint32_t *console_size,
    uint64_t *pmsg, uint32_t *pmsg_size)
{
	uint64_t at = f->ramoops_base;
	uint64_t dump = f->ramoops_size - f->console_size - f->ftrace_size - f->pmsg_size;

	if (f->record_size && dump >= f->record_size) {
		uint64_t count = dump / f->record_size;

		at += count * (dump / count);
	}
	*console_size = f->console_size;
	*console = f->console_size ? at : 0;
	at += f->console_size;
	at += f->ftrace_size;
	*pmsg_size = f->pmsg_size;
	*pmsg = f->pmsg_size ? at : 0;
}

#endif /* MTKFDT_H */
