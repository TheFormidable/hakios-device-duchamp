/*
 * TOUCH_PROBE=1: the touch screen, from the loader, before any of it goes into the kernel.
 *
 * duchamp ships with one of two touch controllers, and Android's device tree carries both on the same node
 * (dtbo, xiaomi_touch@0 under spi2@11012000): "xiaomi,touch-spi" for the Goodix GT9916 (Berlin-D, vendor driver
 * goodix_9916) and "focaltech,n11a-3683g-spi" for the FocalTech FT3683G (vendor driver focaltech_3683g). The
 * vendor drivers are public (github.com/mt6897-devs/android_kernel_device_modules_6.1, drivers/input/touchscreen/)
 * and everything below is read from them, from the MT6897 pin table (drivers/pinctrl/mediatek/pinctrl-mt6897.c,
 * pinctrl-mtk-mt6897.h), the MT6368 regulator table (drivers/regulator/mt6368-regulator.c) and the PMIF driver
 * (drivers/spmi/spmi-mtk-pmif-core.c). Both chips keep their firmware in their own flash: nothing is downloaded.
 *
 *   I/O supply   MT6368 (SPMI slave 5) LDO VTP at 1.8 V: enable bit 0 of 0x1c87, voltage index in 0x1da5[3:0]
 *                (4 = 1.8 V). Written only if the PMIC at slave 5 says it is an MT6368 and VTP is off.
 *   analog       GPIO73 high ("goodix,avdd-gpio", which the FocalTech driver also uses)
 *   reset        GPIO60, active low; interrupt GPIO9, low = a report is waiting
 *   SPI          GPIO11 CLK, 12 CS, 13 MISO, 14 MOSI (function 1 = SPI2); mode 0 for both chips. Driven by hand here:
 *                no clock, no controller, nothing to get wrong but the wire protocol.
 *   Goodix       0xF1 + 32-bit address + 3 dummy bytes to read, 0xF0 + address to write; firmware version at 0x10014
 *                (28 bytes), chip description at 0x10070, the touch report where the description says (Android's
 *                log on Isaac's phone: 0x10308), coordinates in 1/16 pixel (19520 x 43392)
 *   FocalTech    address, 0xA0 (read with CRC), 16-bit length, 3 dummy bytes, data, CRC-16/Kermit; chip ID at
 *                0xA3/0x9F (0x56 0x72), firmware version 0xA6, report at 0x01 (84 bytes, protocol v2), 1/16 pixel
 *
 * Which chip it is: whichever answers. The vendor drivers look at two panel ID lines, but they name them by Linux's
 * global GPIO numbers (385, 391), whose base is only known at run time; asking both chips is just as certain. The
 * FocalTech question goes first: to a Goodix chip it is a frame that does not start with 0xF0 or 0xF1, which it
 * ignores, whereas a Goodix read (F1 00 01 00 14 ...) would reach a FocalTech chip as a write to its register 0xF1.
 *
 * For TOUCH_SECS seconds the probe reads the chip every 8 ms and draws a dot where each finger is, in the finger's
 * colour, so the phone shows straight away whether touch works and whether the axes are the right way round. Then
 * the SPI pins get back the functions LK left them and Android starts. The log says all of it.
 *
 * XNU_TOUCH=1 does the first half of it, touch_bringup(), on the way to XNU, and tells the kernel what it found on
 * its command line (mtktouch=1 Goodix / 2 FocalTech, mtktouch_at= the Goodix report's address, mtktouch_x= and
 * mtktouch_y= the coordinates' range): the kernel's driver (xnu/files/bsd/dev/redwood_mtktouch.c) only reads the
 * chip, on the same pins driven by hand, as hakios-mtk's TD4160 driver reads the chip mtkloader/tcm.h woke.
 */
#ifndef MTKTOUCH_H
#define MTKTOUCH_H

#ifndef TOUCH_SECS
#define TOUCH_SECS 40
#endif

/* ---- pins: pinctrl@10005000, MT6897 ("race free" registers: +4 sets bits, +8 clears them, +0xc writes a mode) ---- */

#define TP_GPIO         0x10005000ULL
#define TP_IOCFG_TM     0x11f20000ULL   /* the pinctrl node's 9th register block: input enables of pins 9, 11-14 */
#define TP_PIN_INT      9
#define TP_PIN_CLK      11
#define TP_PIN_CS       12
#define TP_PIN_MI       13
#define TP_PIN_MO       14
#define TP_PIN_RESET    60
#define TP_PIN_AVDD     73

static inline uint64_t tp_reg(uint32_t base, uint32_t pin)
{
	return TP_GPIO + base + 0x10u * (pin / 32);
}

static inline void tp_set(uint32_t pin, int high)
{
	wr32(tp_reg(0x100, pin) + (high ? 4 : 8), 1u << (pin % 32));
}

static inline int tp_get(uint32_t pin)
{
	return (int)((rd32(tp_reg(0x200, pin)) >> (pin % 32)) & 1u);
}

static inline int tp_out_level(uint32_t pin)
{
	return (int)((rd32(tp_reg(0x100, pin)) >> (pin % 32)) & 1u);
}

static inline int tp_is_output(uint32_t pin)
{
	return (int)((rd32(tp_reg(0x000, pin)) >> (pin % 32)) & 1u);
}

static inline void tp_dir(uint32_t pin, int output)
{
	wr32(tp_reg(0x000, pin) + (output ? 4 : 8), 1u << (pin % 32));
}

static inline uint32_t tp_mode(uint32_t pin)
{
	return (rd32(TP_GPIO + 0x300 + 0x10u * (pin / 8)) >> (4 * (pin % 8))) & 7u;
}

static inline void tp_mode_set(uint32_t pin, uint32_t mode)
{
	/* bit 3 of each four is "change this one": the other pins of the register keep theirs */
	wr32(TP_GPIO + 0x300 + 0x10u * (pin / 8) + 0xc, (0x8u | (mode & 7u)) << (4 * (pin % 8)));
	__asm__ volatile("dsb sy");
}

/* an output, at a level set before it starts driving */
static void tp_output(uint32_t pin, int high)
{
	tp_set(pin, high);
	tp_mode_set(pin, 0);
	tp_dir(pin, 1);
}

static void tp_input(uint32_t pin)
{
	tp_mode_set(pin, 0);
	tp_dir(pin, 0);
}

static inline void tp_settle(void)
{
	(void)rd32(TP_GPIO + 0x200);
}

/* ---- the PMIC, through PMIF's software channel 2 (the one Linux uses) ---- */

#define PMIF_SW_ACC     (PLAT_SPMI_BASE + 0x880)
#define PMIF_SW_WDATA   (PLAT_SPMI_BASE + 0x884)
#define PMIF_SW_RDATA   (PLAT_SPMI_BASE + 0x894)
#define PMIF_SW_VLDCLR  (PLAT_SPMI_BASE + 0x8a4)
#define PMIF_SW_STA     (PLAT_SPMI_BASE + 0x8a8)
#define PMIF_IDLE       0
#define PMIF_WFVLDCLR   6
#define PMIF_EXT_LONG   3u              /* extended register, 16-bit address */
#define PMIC_SWCID      0x0b
#define MT6363_SID      4
#define MT6368_SID      5
#define MT6368_VTP_EN   0x1c87          /* bit 0 */
#define MT6368_VTP_SEL  0x1da5          /* [3:0], ldo_volt_table1: 1.2 1.3 1.5 1.7 1.8 2.0 ... V */
#define VTP_SEL_1V8     4

static int pmif_wait(uint32_t state)
{
	for (uint32_t i = 0; i < 10000; i++) {             /* 100 ms, as Linux */
		if (((rd32(PMIF_SW_STA) >> 1) & 7u) == state) {
			return 1;
		}
		dbg_delay_us(10);
	}
	return 0;
}

static int spmi_rd(uint32_t sid, uint32_t addr, uint8_t *value)
{
	if (!pmif_wait(PMIF_IDLE)) {
		return 0;
	}
	wr32(PMIF_SW_ACC, (PMIF_EXT_LONG << 30) | (sid << 24) | (0u << 16) | (addr & 0xffffu));
	if (!pmif_wait(PMIF_WFVLDCLR)) {
		return 0;
	}
	*value = (uint8_t)rd32(PMIF_SW_RDATA);
	wr32(PMIF_SW_VLDCLR, 1);
	return 1;
}

static int spmi_wr(uint32_t sid, uint32_t addr, uint8_t value)
{
	if (!pmif_wait(PMIF_IDLE)) {
		return 0;
	}
	wr32(PMIF_SW_WDATA, value);
	wr32(PMIF_SW_ACC, (PMIF_EXT_LONG << 30) | (1u << 29) | (sid << 24) | (0u << 16) | (addr & 0xffffu));
	return pmif_wait(PMIF_IDLE);
}

/* ---- output ---- */

static void tp_byte(uint8_t b)
{
	static const char hex[] = "0123456789abcdef";

	out_putc(hex[b >> 4]);
	out_putc(hex[b & 15]);
}

static void tp_bytes(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (i) {
			out_putc(' ');
		}
		tp_byte(p[i]);
	}
}

static void tp_text(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		out_putc(p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '.');
	}
}

static void tp_pin(const char *name, uint32_t pin)
{
	out(name);
	out(" GPIO");
	out_dec(pin);
	out(": function ");
	out_dec(tp_mode(pin));
	out(tp_is_output(pin) ? ", output " : ", input");
	if (tp_is_output(pin)) {
		out_dec((uint64_t)tp_out_level(pin));
	}
	out(", reads ");
	out_dec((uint64_t)tp_get(pin));
	out("\n");
}

/* ---- SPI by hand, mode 0: the clock idles low, both sides sample on its rising edge ---- */

static void tp_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	tp_set(TP_PIN_CLK, 0);
	tp_set(TP_PIN_CS, 0);
	dbg_delay_us(2);
	for (uint32_t i = 0; i < len; i++) {
		uint8_t o = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			tp_set(TP_PIN_MO, (o >> bit) & 1);
			tp_settle();
			tp_set(TP_PIN_CLK, 1);
			tp_settle();
			in = (uint8_t)((in << 1) | tp_get(TP_PIN_MI));
			tp_set(TP_PIN_CLK, 0);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	tp_settle();
	dbg_delay_us(2);
	tp_set(TP_PIN_CS, 1);
	dbg_delay_us(2);
}

static uint8_t tp_tx[1100], tp_rx[1100];

/* ---- Goodix GT9916 ---- */

#define GDX_FW_VERSION  0x10014u
#define GDX_IC_INFO     0x10070u
#define GDX_MAX_READ    1024u
#define GDX_MAX_TOUCH   10u

static uint32_t gdx_touch_addr, gdx_max_x = 19520, gdx_max_y = 43392;

static int gdx_read(uint32_t addr, uint8_t *data, uint32_t len)
{
	if (len > GDX_MAX_READ) {
		return 0;
	}
	tp_tx[0] = 0xf1;
	tp_tx[1] = (uint8_t)(addr >> 24);
	tp_tx[2] = (uint8_t)(addr >> 16);
	tp_tx[3] = (uint8_t)(addr >> 8);
	tp_tx[4] = (uint8_t)addr;
	tp_tx[5] = tp_tx[6] = tp_tx[7] = 0xff;
	for (uint32_t i = 0; i < len; i++) {
		tp_tx[8 + i] = 0;
	}
	tp_xfer(tp_tx, tp_rx, 8 + len);
	for (uint32_t i = 0; i < len; i++) {
		data[i] = tp_rx[8 + i];
	}
	return 1;
}

static void __attribute__((unused)) gdx_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
	tp_tx[0] = 0xf0;
	tp_tx[1] = (uint8_t)(addr >> 24);
	tp_tx[2] = (uint8_t)(addr >> 16);
	tp_tx[3] = (uint8_t)(addr >> 8);
	tp_tx[4] = (uint8_t)addr;
	for (uint32_t i = 0; i < len; i++) {
		tp_tx[5 + i] = data[i];
	}
	tp_xfer(tp_tx, 0, 5 + len);
}

/* The chip's blocks end in the 16-bit sum of their bytes; all zeros or all ones is no answer at all. */
static int gdx_sum_ok(const uint8_t *d, uint32_t len)
{
	uint32_t sum = 0;
	int blank = 1;

	if (len < 3) {
		return 0;
	}
	for (uint32_t i = 0; i < len - 2; i++) {
		sum += d[i];
		blank &= d[i] == 0 || d[i] == 0xff;
	}
	return !blank && (sum & 0xffffu) == (uint32_t)(d[len - 2] | d[len - 1] << 8);
}

static uint32_t tp_le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t tp_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int gdx_probe(void)
{
	static uint8_t v[28], info[GDX_MAX_READ];
	uint32_t len, off;

	gdx_read(GDX_FW_VERSION, v, sizeof(v));
	out("touch: Goodix firmware version block: ");
	tp_bytes(v, sizeof(v));
	out("\n");
	if (!gdx_sum_ok(v, sizeof(v))) {
		out("touch: no Goodix answer (no valid checksum)\n");
		return 0;
	}
	/* struct goodix_fw_version: rom_pid[6] rom_vid[3] - patch_pid[8] patch_vid[4] - sensor_id - - checksum */
	out("touch: Goodix chip \"");
	tp_text(v, 6);
	out("\", firmware \"");
	tp_text(v + 10, 8);
	out("\" ");
	tp_bytes(v + 18, 4);
	out(", sensor ");
	out_dec(v[23]);
	out("\n");

	gdx_read(GDX_IC_INFO, info, 2);
	len = tp_le16(info);
	if (len < 40 || len >= GDX_MAX_READ) {
		out("touch: bad chip description length ");
		out_dec(len);
		out("\n");
		return 0;
	}
	gdx_read(GDX_IC_INFO, info, len);
	if (!gdx_sum_ok(info, len)) {
		out("touch: chip description fails its checksum\n");
		return 0;
	}
	/* length, version (16), features (10), four counts, then five lists of 16-bit numbers, then "misc" */
	off = 2 + 16 + 10 + 4;
	for (int i = 0; i < 5; i++) {
		if (off >= len) {
			return 0;
		}
		off += 1 + 2u * info[off];
	}
	if (off + 56 > len) {
		out("touch: chip description too short\n");
		return 0;
	}
	gdx_touch_addr = tp_le32(info + off + 44);
	out("touch: report at ");
	out_hex(gdx_touch_addr);
	out(", header ");
	out_dec(tp_le16(info + off + 48));
	out(", point ");
	out_dec(tp_le16(info + off + 50));
	out(" bytes, panel ");
	out_dec(tp_le16(info + off + 52));
	out(" x ");
	out_dec(tp_le16(info + off + 54));
	out(", commands at ");
	out_hex(tp_le32(info + off));
	if (off + 122 + 20 + 2 <= len) {
		uint32_t x = tp_le16(info + off + 122 + 16), y = tp_le16(info + off + 122 + 18);

		out(", screen max ");
		out_dec(x);
		out(" x ");
		out_dec(y);
		if (x && y) {
			gdx_max_x = x;
			gdx_max_y = y;
		}
	}
	out("\n");
	return gdx_touch_addr != 0;
}

/* ---- FocalTech FT3683G ---- */

#define FTS_REPORT_LEN  84u             /* FTS_TOUCH_DATA_LEN_V2: 10 points of 8 bytes and a 4-byte head */

static uint16_t fts_crc(const uint8_t *d, uint32_t len)
{
	uint32_t crc = 0xffff;

	for (uint32_t i = 0; i < len; i++) {
		crc ^= d[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 1) ? (crc >> 1) ^ 0x8408u : crc >> 1;
		}
	}
	return (uint16_t)crc;
}

/* 1: read and checked; 0: no transfer was clean (busy, CRC); *status gets the chip's status byte of the last try */
static int fts_read(uint8_t addr, uint8_t *data, uint32_t len, uint8_t *status)
{
	const uint32_t dp = 7, total = dp + len + 2;

	for (int attempt = 0; attempt < 3; attempt++) {
		for (uint32_t i = 0; i < total; i++) {
			tp_tx[i] = 0;
		}
		tp_tx[0] = addr;
		tp_tx[1] = 0xa0;                        /* READ_CMD: 0x80 | DATA_CRC_EN */
		tp_tx[2] = (uint8_t)(len >> 8);
		tp_tx[3] = (uint8_t)len;
		tp_xfer(tp_tx, tp_rx, total);
		dbg_delay_us(150);                      /* CS_HIGH_DELAY */
		if (status) {
			*status = tp_rx[3];
		}
		if ((tp_rx[3] & 0xa0) == 0 &&
		    fts_crc(tp_rx + dp, len) == (uint16_t)(tp_rx[dp + len] | tp_rx[dp + len + 1] << 8)) {
			for (uint32_t i = 0; i < len; i++) {
				data[i] = tp_rx[dp + i];
			}
			return 1;
		}
	}
	return 0;
}

static int fts_probe(void)
{
	uint8_t id_h = 0, id_l = 0, ver = 0, s1 = 0, s2 = 0;

	for (int attempt = 0; attempt < 5; attempt++) {
		int ok = fts_read(0xa3, &id_h, 1, &s1) && fts_read(0x9f, &id_l, 1, &s2);

		if (ok && id_h == 0x56 && id_l == 0x72) {
			fts_read(0xa6, &ver, 1, 0);
			out("touch: FocalTech FT3683G (chip ID 56 72), firmware version ");
			tp_byte(ver);
			out("\n");
			return 1;
		}
		if (attempt == 0 || attempt == 4) {
			out("touch: FocalTech chip ID ");
			out(ok ? "read: " : "not read cleanly: ");
			tp_byte(id_h);
			out_putc(' ');
			tp_byte(id_l);
			out(", status ");
			tp_byte(s1);
			out_putc(' ');
			tp_byte(s2);
			out(", raw ");
			tp_bytes(tp_rx, 12);
			out("\n");
		}
		dbg_delay_ms(200);                      /* INTERVAL_READ_REG */
	}
	return 0;
}

#if TOUCH_PROBE
/* ---- fingers, on the screen and in the log ---- */

static const uint32_t tp_colour[GDX_MAX_TOUCH] = {
	0xffff4040u, 0xff40ff40u, 0xff4080ffu, 0xffffff40u, 0xffff40ffu,
	0xff40ffffu, 0xffffa040u, 0xffa0a0ffu, 0xffffffffu, 0xff80ff80u,
};

static struct {
	uint32_t polls, reports, touches, bad, int_low, downs, lines;
	uint32_t down, seen;                     /* bit per finger id */
	uint32_t x[GDX_MAX_TOUCH], y[GDX_MAX_TOUCH];
} tp;

static void tp_finger(uint32_t id, uint32_t x, uint32_t y)
{
	uint32_t px = x / 16, py = y / 16;      /* both chips report 1/16 of a pixel */

	if (id >= GDX_MAX_TOUCH) {
		return;
	}
	tp.touches++;
	tp.seen |= 1u << id;
	if (px > PANEL_W - 1) {
		px = PANEL_W - 1;
	}
	if (py > PANEL_H - 1) {
		py = PANEL_H - 1;
	}
	fb_rect(px > 6 ? px - 6 : 0, py > 6 ? py - 6 : 0, 13, 13, tp_colour[id]);
	if (!(tp.down & (1u << id))) {
		tp.downs++;
		if (tp.lines < 60) {
			tp.lines++;
			out("touch: finger ");
			out_dec(id);
			out(" down at ");
			out_dec(x);
			out(", ");
			out_dec(y);
			out(" (pixel ");
			out_dec(px);
			out(", ");
			out_dec(py);
			out(")\n");
		}
	}
	tp.down |= 1u << id;
	tp.x[id] = x;
	tp.y[id] = y;
}

/* a finger that was down and is no longer in the report has lifted */
static void tp_lifted(void)
{
	uint32_t up = tp.down & ~tp.seen;

	for (uint32_t id = 0; id < GDX_MAX_TOUCH; id++) {
		if ((up & (1u << id)) && tp.lines < 60) {
			tp.lines++;
			out("touch: finger ");
			out_dec(id);
			out(" up at ");
			out_dec(tp.x[id]);
			out(", ");
			out_dec(tp.y[id]);
			out("\n");
		}
	}
	tp.down &= tp.seen;
	tp.seen = 0;
}

static void gdx_poll(void)
{
	static uint8_t r[8 + 8 * GDX_MAX_TOUCH + 2];
	const uint8_t zero = 0;
	uint32_t n;

	tp.polls++;
	gdx_read(gdx_touch_addr, r, 8 + 16 + 2);         /* the head and two fingers, all there is most of the time */
	if (r[0] == 0) {
		return;
	}
	tp.reports++;
	n = r[2] & 0xfu;
	if ((r[0] & 0x80) && n > 2 && n <= GDX_MAX_TOUCH) {
		gdx_read(gdx_touch_addr + 26, r + 26, (n - 2) * 8);
	}
	gdx_write(gdx_touch_addr, &zero, 1);             /* read: the chip may write the next one */
	if (!gdx_sum_ok(r, 8)) {
		if (tp.bad++ < 6) {
			out("touch: bad report head ");
			tp_bytes(r, 8);
			out("\n");
		}
		return;
	}
	if (r[0] & 0x40) {                                /* a request, 3 = reset me */
		if (tp.lines < 60) {
			tp.lines++;
			out("touch: the chip makes request ");
			tp_byte(r[2]);
			out("\n");
		}
		return;
	}
	if (!(r[0] & 0x80)) {
		return;
	}
	if (n > GDX_MAX_TOUCH || (n && !gdx_sum_ok(r + 8, 8 * n + 2))) {
		if (tp.bad++ < 6) {
			out("touch: bad report, ");
			out_dec(n);
			out(" fingers\n");
		}
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *t = r + 8 + 8 * i;
		uint32_t kind = t[0] & 0xfu;

		if (kind == 1 || kind == 3) {                 /* a stylus */
			continue;
		}
		tp_finger(t[0] >> 4, tp_le16(t + 2), tp_le16(t + 4));
	}
	tp_lifted();
}

static void fts_poll(void)
{
	static uint8_t r[FTS_REPORT_LEN];
	uint32_t type, n;

	tp.polls++;
	if (!fts_read(0x01, r, FTS_REPORT_LEN, 0)) {
		tp.bad++;
		return;
	}
	if (r[1] == 0xff && r[2] == 0xff && r[3] == 0xff && r[4] == 0xff) {
		return;                                   /* nothing yet: the firmware has just started */
	}
	tp.reports++;
	type = r[1] >> 4;
	n = r[1] & 0xfu;
	if (type == 2) {                                  /* TOUCH_PROTOCOL_v2: 8 bytes a finger after a 4-byte head */
		for (uint32_t i = 0; i < n && i < GDX_MAX_TOUCH; i++) {
			const uint8_t *t = r + 4 + 8 * i;
			uint32_t x = (uint32_t)(t[0] & 15) << 12 | (uint32_t)t[1] << 4 | t[4] >> 4;
			uint32_t y = (uint32_t)(t[2] & 15) << 12 | (uint32_t)t[3] << 4 | (t[4] & 15u);

			if ((t[0] >> 6) != 1) {               /* 0 down, 2 contact; 1 is a finger lifting */
				tp_finger(t[2] >> 4, x, y);
			}
		}
	} else if (type == 0) {                           /* the older layout: 6 bytes a finger after a 2-byte head */
		for (uint32_t i = 0; i < GDX_MAX_TOUCH; i++) {
			const uint8_t *t = r + 2 + 6 * i;
			uint32_t id = t[2] >> 4;

			if (id >= GDX_MAX_TOUCH) {
				break;
			}
			if ((t[0] >> 6) != 1) {
				tp_finger(id, (uint32_t)(t[0] & 15) << 12 | (uint32_t)t[1] << 4 | t[4] >> 4,
				    (uint32_t)(t[2] & 15) << 12 | (uint32_t)t[3] << 4 | (t[4] & 15u));
			}
		}
	} else if (tp.bad++ < 6) {
		out("touch: report of type ");
		out_dec(type);
		out(": ");
		tp_bytes(r, 12);
		out("\n");
	}
	tp_lifted();
}

/* ---- all of it ---- */

static void tp_summary(uint32_t secs)
{
	out("touch: ");
	out_dec(secs);
	out(" s: ");
	out_dec(tp.polls);
	out(" reads, ");
	out_dec(tp.reports);
	out(" reports, ");
	out_dec(tp.touches);
	out(" finger samples, ");
	out_dec(tp.downs);
	out(" touches, ");
	out_dec(tp.bad);
	out(" bad, interrupt line low ");
	out_dec(tp.int_low);
	out(" times\n");
}

#endif /* TOUCH_PROBE */

/* The I/O supply: 1 if VTP is on at 1.8 V when this returns. */
static int tp_power_io(void)
{
	uint8_t cid4 = 0, cid5 = 0, en = 0, sel = 0;
	int ok4 = spmi_rd(MT6363_SID, PMIC_SWCID, &cid4), ok5 = spmi_rd(MT6368_SID, PMIC_SWCID, &cid5);

	out("touch: PMIC IDs: slave 4 ");
	if (ok4) {
		tp_byte(cid4);
	} else {
		out("(no answer)");
	}
	out(", slave 5 ");
	if (ok5) {
		tp_byte(cid5);
	} else {
		out("(no answer)");
	}
	out("\n");
	if (!ok5 || cid5 != 0x68) {
		out("touch: slave 5 is not an MT6368: VTP left alone\n");
		return 0;
	}
	if (!spmi_rd(MT6368_SID, MT6368_VTP_EN, &en) || !spmi_rd(MT6368_SID, MT6368_VTP_SEL, &sel)) {
		out("touch: VTP registers not readable\n");
		return 0;
	}
	out("touch: VTP enable register ");
	tp_byte(en);
	out(", voltage register ");
	tp_byte(sel);
	out((en & 1) ? " (on)\n" : " (off)\n");
	if (en & 1) {
		if ((sel & 15) != VTP_SEL_1V8) {
			out("touch: VTP is on at another voltage than 1.8 V: left alone\n");
			return 0;
		}
		return 1;
	}
	if ((sel & 15) != VTP_SEL_1V8 && !spmi_wr(MT6368_SID, MT6368_VTP_SEL, (uint8_t)((sel & 0xf0) | VTP_SEL_1V8))) {
		out("touch: VTP voltage write failed\n");
		return 0;
	}
	if (!spmi_wr(MT6368_SID, MT6368_VTP_EN, (uint8_t)(en | 1))) {
		out("touch: VTP enable write failed\n");
		return 0;
	}
	dbg_delay_ms(10);
	spmi_rd(MT6368_SID, MT6368_VTP_EN, &en);
	spmi_rd(MT6368_SID, MT6368_VTP_SEL, &sel);
	out("touch: VTP now ");
	tp_byte(en);
	out_putc(' ');
	tp_byte(sel);
	out("\n");
	return (en & 1) && (sel & 15) == VTP_SEL_1V8;
}

enum { TOUCH_NONE, TOUCH_GOODIX, TOUCH_FOCALTECH };
static uint32_t touch_chip;                     /* what touch_bringup() found */

/*
 * Power, pins and reset as the vendor drivers do them, then ask the two chips. The SPI pins stay GPIOs driven by
 * hand (lk_mode: the functions LK had given them, for whoever wants them back). Returns TOUCH_*.
 */
static uint32_t touch_bringup(uint32_t lk_mode[4])
{
	static const uint32_t spi_pins[4] = { TP_PIN_CLK, TP_PIN_CS, TP_PIN_MI, TP_PIN_MO };
	uint32_t ies;

	out("touch: as LK left them:\n");
	tp_pin("  interrupt", TP_PIN_INT);
	tp_pin("  SPI clock", TP_PIN_CLK);
	tp_pin("  SPI CS   ", TP_PIN_CS);
	tp_pin("  SPI MISO ", TP_PIN_MI);
	tp_pin("  SPI MOSI ", TP_PIN_MO);
	tp_pin("  reset    ", TP_PIN_RESET);
	tp_pin("  AVDD     ", TP_PIN_AVDD);
	ies = rd32(TP_IOCFG_TM + 0x80);
	out("  input enables: GPIO9 ");
	out_dec((ies >> 19) & 1);
	ies = rd32(TP_IOCFG_TM + 0x90);
	out(", GPIO13 ");
	out_dec((ies >> 14) & 1);
	out("\n");
	for (int i = 0; i < 4; i++) {
		lk_mode[i] = tp_mode(spi_pins[i]);
	}

	/* the vendor drivers' order: reset held, I/O supply, 10 ms, analog supply, 15 ms, reset let go */
	tp_output(TP_PIN_RESET, 0);
	tp_input(TP_PIN_INT);
	if (!((rd32(TP_IOCFG_TM + 0x80) >> 19) & 1)) {
		wr32(TP_IOCFG_TM + 0x80 + 4, 1u << 19);
	}
	if (!((rd32(TP_IOCFG_TM + 0x90) >> 14) & 1)) {
		wr32(TP_IOCFG_TM + 0x90 + 4, 1u << 14);
	}
	tp_output(TP_PIN_CS, 1);
	tp_output(TP_PIN_CLK, 0);
	tp_output(TP_PIN_MO, 0);
	tp_input(TP_PIN_MI);
	if (!tp_power_io()) {
		out("touch: trying the chip anyway\n");
	}
	dbg_delay_ms(10);
	tp_output(TP_PIN_AVDD, 1);
	dbg_delay_ms(15);
	out("touch: MISO with the chip in reset reads ");
	out_dec((uint64_t)tp_get(TP_PIN_MI));
	out(", interrupt ");
	out_dec((uint64_t)tp_get(TP_PIN_INT));
	out("\n");
	tp_set(TP_PIN_RESET, 1);
	dbg_delay_ms(300);
	out("touch: out of reset: interrupt line ");
	out_dec((uint64_t)tp_get(TP_PIN_INT));
	out("\n");

	touch_chip = TOUCH_NONE;
	if (fts_probe()) {
		touch_chip = TOUCH_FOCALTECH;
	} else if (gdx_probe()) {
		touch_chip = TOUCH_GOODIX;
	} else {
		out("touch: neither chip answered\n");
	}
	plog_flush();
	return touch_chip;
}

#if TOUCH_PROBE
static void touch_probe(void)
{
	static const uint32_t spi_pins[4] = { TP_PIN_CLK, TP_PIN_CS, TP_PIN_MI, TP_PIN_MO };
	uint32_t lk_mode[4], chip;
	uint64_t t0, next, end, freq = dbg_freq();
	uint32_t reported = 0;

	out("touch: probe (TOUCH_PROBE=1), ");
	out_dec(TOUCH_SECS);
	out(" s of touches after it\n");
	chip = touch_bringup(lk_mode);

	if (chip != TOUCH_NONE) {
		fb_rect(0, 0, PANEL_W, PANEL_H, 0xff000000u);
		fbc.x = 0;
		fbc.y = fbc.top = 0;
		fbc.scale = 3;
		fbc.fg = 0xff40ff40u;
		out(chip == TOUCH_GOODIX ? "TOUCH: Goodix GT9916\n" : "TOUCH: FocalTech FT3683G\n");
		out("Draw on the screen for ");
		out_dec(TOUCH_SECS);
		out(" s\n");
		fbc.scale = 1;
		fbc.fg = 0xffffffffu;
		t0 = dbg_ticks();
		next = t0;
		end = t0 + freq * TOUCH_SECS;
		while (dbg_ticks() < end) {
			uint32_t secs = (uint32_t)((dbg_ticks() - t0) / freq);

			if (!tp_get(TP_PIN_INT)) {
				tp.int_low++;
			}
			if (chip == TOUCH_GOODIX) {
				gdx_poll();
			} else {
				fts_poll();
			}
			if (secs >= reported + 5) {
				reported = secs;
				tp_summary(secs);
				plog_flush();
			}
			next += freq / 125;                   /* every 8 ms */
			while (dbg_ticks() < next) {
			}
		}
		tp_summary(TOUCH_SECS);
	}

	for (int i = 0; i < 4; i++) {
		tp_mode_set(spi_pins[i], lk_mode[i]);
	}
	out("touch: SPI pins back to LK's functions; probe done\n");
	plog_flush();
	dbg_delay_ms(3000);
}
#endif /* TOUCH_PROBE */

#endif /* MTKTOUCH_H */
