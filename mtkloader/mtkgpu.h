/*
 * GPU_PROBE=1: what state the Mali-G615 MC6 is in when the loader runs, read only. Nothing is written: no rail, no
 * clock, no power domain, no GPU register. Then Android starts.
 *
 * On the MT6897 the GPU is not woken by the SPM as the MT6768's Mali-G52 was (hakios-mtk mtkloader/mtkmali.h): Android
 * runs it in "EB mode" (the device tree's gpufreq-wrapper says gpueb-support = 1), where a microcontroller of the GPU
 * block's own, the GPUEB at 0x13c00000, switches the rails, the PLLs and the power domains when Linux asks it over a
 * mailbox. Its firmware is started before Linux: the kernel's GPUEB driver loads none. So the first questions are
 * whether the GPUEB is running when LK hands over, and what is powered.
 *
 * Where everything below comes from (GPL sources, read for addresses only): MediaTek's
 * drivers/gpu/mediatek/gpufreq/v2/gpufreq_mt6897.c and include/gpufreq_reg_mt6897.h (the power sequence and every
 * register), drivers/gpu/mediatek/gpueb/gpueb_debug.c (the GPUEB's status registers), drivers/regulator/
 * mt6368-regulator.c and mt6363-regulator.c with their headers (the rails), in
 * github.com/mt6897-devs/android_kernel_device_modules_6.1; the dtb of mt6897-devs/device_xiaomi_duchamp-kernel
 * (gpueb@13c00000, gpufreq@13fbf000, mali@13000000, vgpu-supply = mt6368 vbuck2, vsram-supply = mt6363 vsram_mdfe);
 * Linux drivers/gpu/drm/panthor/panthor_regs.h for the GPU's own registers.
 *
 * The order guards the reads: the SPM (always on) first, which says whether MFG0, the GPU block's always-on part
 * where the GPUEB and the MFG power controller live, has power. Only then the GPUEB and the MFG registers; and the
 * GPU's own registers only if MFG1 (the GPU's top) is on, out of isolation, out of reset and its PLL running. A read of
 * a block without power can hang the bus: plog_flush() before each step leaves in pstore how far it got.
 */
#ifndef MTKGPU_H
#define MTKGPU_H

/* the SPM ("sleep" in the gpufreq node) */
#define GPU_SPM                 0x1c001000ULL
#define GPU_SPM2GPUPM_CON       (GPU_SPM + 0x410)
#define GPU_SPM_SRC_REQ         (GPU_SPM + 0x818)
#define GPU_SPM_MFG0_PWR_CON    (GPU_SPM + 0xf00)
#define GPU_SPM_BUCK_ISO_CON    (GPU_SPM + 0xfa4)       /* bit 8 VGPU_EXT_BUCK_ISO, 9 SRAM ISO_DIN, 10 SRAM LATCH_ENB */
#define GPU_SPM_XPU_PWR_STA     (GPU_SPM + 0xfc0)       /* bit 1: MFG0 */
#define GPU_SPM_XPU_PWR_STA2    (GPU_SPM + 0xfc4)

/* the GPUEB */
#define GPUEB_GPR               0x13c2fd1cULL
#define GPUEB_CFG               0x13c60000ULL
#define GPUEB_MBOX              0x13c62000ULL

/* the MFG power controller, PLLs and top config */
#define MFG_RPC                 0x13f90000ULL
#define MFG_RPC_AO_CLK_CFG      (MFG_RPC + 0x1034)
#define MFG_RPC_SLP_PROT_STA    (MFG_RPC + 0x1048)
#define MFG_RPC_MFG1_PWR_CON    (MFG_RPC + 0x1070)
#define MFG_RPC_PWR_STA         (MFG_RPC + 0x1200)
#define MFG_RPC_PWR_STA2        (MFG_RPC + 0x1204)
#define MFG_PLL_CON0            0x13fa0008ULL
#define MFG_PLL_CON1            0x13fa000cULL
#define MFGSC_PLL_CON0          0x13fa0c08ULL
#define MFGSC_PLL_CON1          0x13fa0c0cULL
#define MFG_TOP_CG_CON          0x13fbf000ULL
#define MFG_TOP_ASYNC_CON       0x13fbf020ULL
#define MFG_TOP_GLOBAL_CON      0x13fbf0b0ULL

/* elsewhere */
#define GPU_TOPCK_CLK_CFG_3     0x10000040ULL
#define GPU_TOPCK_CLK_CFG_30    0x100001f0ULL
#define GPU_IFR_EMI_PROT_STA0   0x1002c100ULL       /* [20:19]: the GPU's bus to EMI held (1) or released (0) */
#define GPU_IFR_EMI_PROT_STA1   0x1002c120ULL
#define GPU_IFR_MFG_PROT_STA0   0x1002c1a0ULL

/* the GPU (panthor_regs.h) */
#define MALI_REGS               0x13000000ULL

/* the rails: VGPU = MT6368 VBUCK2, VSRAM = MT6363 VSRAM_MDFE (LDO) */
#define MT6368_VBUCK2_EN        0x240               /* bit 2 */
#define MT6368_VBUCK2_VOSEL     0x24e               /* 6.25 mV a step from 0 */
#define MT6363_VSRAM_MDFE_EN    0x243               /* bit 5 */
#define MT6363_VSRAM_MDFE_VOSEL 0x259               /* [6:0], 6.25 mV a step from 400 mV */

static void gpu_reg(const char *name, uint64_t a)
{
	out("  ");
	out(name);
	out(" (");
	out_hex(a);
	out(") ");
	out_hex(rd32(a));
	out("\n");
}

static void gpu_pmic(const char *name, uint32_t sid, uint32_t en_reg, uint32_t en_bit, uint32_t sel_reg,
    uint32_t sel_mask, uint32_t base_uv)
{
	uint8_t en = 0, sel = 0;
	int ok = spmi_rd(sid, en_reg, &en) && spmi_rd(sid, sel_reg, &sel);

	out("  ");
	out(name);
	if (!ok) {
		out(": the PMIC did not answer\n");
		return;
	}
	out(": enable register ");
	out_hex(en);
	out(en & (1u << en_bit) ? " (on)" : " (off)");
	out(", voltage index ");
	out_hex(sel);
	out(" = ");
	out_dec((base_uv + 6250u * (sel & sel_mask)) / 1000u);
	out(" mV\n");
}

/* what is powered; 0 MFG0 off, 1 MFG1 off, 2 PLL off, 3 the GPU answered */
static int gpu_state(void)
{
	uint32_t mfg0, rpc, rpc2, con1, pll;
	uint8_t id4 = 0, id5 = 0;


	out("gpu: rails\n");
	spmi_rd(MT6363_SID, PMIC_SWCID, &id4);
	spmi_rd(MT6368_SID, PMIC_SWCID, &id5);
	out("  PMIC IDs: slave 4 ");
	out_hex(id4);
	out(", slave 5 ");
	out_hex(id5);
	out("\n");
	gpu_pmic("VGPU (MT6368 VBUCK2)", MT6368_SID, MT6368_VBUCK2_EN, 2, MT6368_VBUCK2_VOSEL, 0xff, 0);
	gpu_pmic("VSRAM (MT6363 VSRAM_MDFE)", MT6363_SID, MT6363_VSRAM_MDFE_EN, 5, MT6363_VSRAM_MDFE_VOSEL, 0x7f, 400000);
	plog_flush();

	out("gpu: SPM\n");
	gpu_reg("XPU_PWR_STATUS", GPU_SPM_XPU_PWR_STA);
	gpu_reg("XPU_PWR_STATUS_2ND", GPU_SPM_XPU_PWR_STA2);
	gpu_reg("MFG0_PWR_CON", GPU_SPM_MFG0_PWR_CON);
	gpu_reg("SOC_BUCK_ISO_CON", GPU_SPM_BUCK_ISO_CON);
	gpu_reg("SPM2GPUPM_CON", GPU_SPM2GPUPM_CON);
	gpu_reg("SRC_REQ", GPU_SPM_SRC_REQ);
	out("gpu: clocks and bus\n");
	gpu_reg("TOPCK_CLK_CFG_3", GPU_TOPCK_CLK_CFG_3);
	gpu_reg("TOPCK_CLK_CFG_30", GPU_TOPCK_CLK_CFG_30);
	gpu_reg("IFR_EMISYS_PROTECT_EN_STA_0", GPU_IFR_EMI_PROT_STA0);
	gpu_reg("IFR_EMISYS_PROTECT_EN_STA_1", GPU_IFR_EMI_PROT_STA1);
	gpu_reg("IFR_MFGSYS_PROT_EN_STA_0", GPU_IFR_MFG_PROT_STA0);
	plog_flush();

	mfg0 = (rd32(GPU_SPM_XPU_PWR_STA) >> 1) & 1u;
	if (!mfg0 || !((rd32(GPU_SPM_XPU_PWR_STA2) >> 1) & 1u)) {
		out("gpu: MFG0 has no power: the GPUEB is not running and nothing of the GPU block is read\n");
		plog_flush();
		return 0;
	}

	out("gpu: MFG0 on; the GPUEB (its PC five times, 1 ms apart)\n");
	plog_flush();
	for (int i = 0; i < 5; i++) {
		out("  PC ");
		out_hex(rd32(GPUEB_CFG + 0x71c));
		out(" LR ");
		out_hex(rd32(GPUEB_CFG + 0x720));
		out(" SP ");
		out_hex(rd32(GPUEB_CFG + 0x724));
		out("\n");
		dbg_delay_ms(1);
	}
	gpu_reg("GPUEB_INTC_IRQ_EN", GPUEB_CFG + 0x300);
	gpu_reg("GPUEB_INTC_IRQ_STA", GPUEB_CFG + 0x338);
	gpu_reg("GPUEB_INTC_IRQ_RAW_STA", GPUEB_CFG + 0x33c);
	gpu_reg("GPUEB_CFGREG_AXI_STA", GPUEB_CFG + 0x610);
	gpu_reg("GPUEB_CFGREG_WDT_CON", GPUEB_CFG + 0x618);
	gpu_reg("GPUEB_CFGREG_MDSP_CFG", GPUEB_CFG + 0x634);
	gpu_reg("LOW_POWER_FOOTPRINT_GPR", GPUEB_GPR + 0x14);
	gpu_reg("GPUEB_DRAM_RES_STA_GPR", GPUEB_GPR + 0x40);
	gpu_reg("GPUFREQ_FOOTPRINT_GPR", GPUEB_GPR + 0x44);
	gpu_reg("GPUEB_DIAGNOSIS_GPR", GPUEB_GPR + 0x48);
	gpu_reg("GPUEB_MBOX_IPI_GPUEB", GPUEB_MBOX);
	gpu_reg("GPUEB_MBOX_SW_INT_STA", GPUEB_MBOX + 0x78);
	plog_flush();

	out("gpu: MFG power controller\n");
	gpu_reg("MFG_RPC_PWR_CON_STATUS", MFG_RPC_PWR_STA);
	gpu_reg("MFG_RPC_PWR_CON_2ND_STATUS", MFG_RPC_PWR_STA2);
	gpu_reg("MFG_RPC_MFG1_PWR_CON", MFG_RPC_MFG1_PWR_CON);
	gpu_reg("MFG_RPC_AO_CLK_CFG", MFG_RPC_AO_CLK_CFG);
	gpu_reg("MFG_RPC_SLP_PROT_EN_STA", MFG_RPC_SLP_PROT_STA);
	for (uint32_t off = 0x10a0; off <= 0x10d0; off += 4) {
		out("  MFG_RPC +");
		out_hex(off);
		out(" ");
		out_hex(rd32(MFG_RPC + off));
		out("\n");
	}
	plog_flush();

	rpc = rd32(MFG_RPC_PWR_STA);
	rpc2 = rd32(MFG_RPC_PWR_STA2);
	con1 = rd32(MFG_RPC_MFG1_PWR_CON);
	if (!((rpc >> 1) & 1u) || !((rpc2 >> 1) & 1u) || (con1 & 0x13u) != 0x01u) {
		out("gpu: MFG1 (the GPU's top) is off, isolated or in reset: the GPU itself is not read\n");
		plog_flush();
		return 1;
	}

	out("gpu: MFG1 on; PLLs\n");
	gpu_reg("MFG_PLL_CON0", MFG_PLL_CON0);
	gpu_reg("MFG_PLL_CON1", MFG_PLL_CON1);
	gpu_reg("MFGSC_PLL_CON0", MFGSC_PLL_CON0);
	gpu_reg("MFGSC_PLL_CON1", MFGSC_PLL_CON1);
	gpu_reg("MFG_CG_CON", MFG_TOP_CG_CON);
	gpu_reg("MFG_ASYNC_CON", MFG_TOP_ASYNC_CON);
	gpu_reg("MFG_GLOBAL_CON", MFG_TOP_GLOBAL_CON);
	plog_flush();

	pll = rd32(MFG_PLL_CON0);
	if (!(pll & 1u)) {
		out("gpu: the GPU's PLL is off: the GPU itself is not read\n");
		plog_flush();
		return 2;
	}

	out("gpu: the Mali's own registers\n");
	plog_flush();
	gpu_reg("GPU_ID", MALI_REGS + 0x000);
	gpu_reg("L2_FEATURES", MALI_REGS + 0x004);
	gpu_reg("CORE_FEATURES", MALI_REGS + 0x008);
	gpu_reg("TILER_FEATURES", MALI_REGS + 0x00c);
	gpu_reg("MEM_FEATURES", MALI_REGS + 0x010);
	gpu_reg("MMU_FEATURES", MALI_REGS + 0x014);
	gpu_reg("AS_PRESENT", MALI_REGS + 0x018);
	gpu_reg("CSF_ID", MALI_REGS + 0x01c);
	gpu_reg("GPU_STATUS", MALI_REGS + 0x034);
	gpu_reg("SHADER_PRESENT_LO", MALI_REGS + 0x100);
	gpu_reg("SHADER_PRESENT_HI", MALI_REGS + 0x104);
	gpu_reg("L2_PRESENT_LO", MALI_REGS + 0x120);
	gpu_reg("SHADER_READY_LO", MALI_REGS + 0x140);
	gpu_reg("L2_READY_LO", MALI_REGS + 0x160);
	gpu_reg("MCU_CONTROL", MALI_REGS + 0x700);
	gpu_reg("MCU_STATUS", MALI_REGS + 0x704);
	plog_flush();
	return 3;
}

/*
 * GPU_PROBE=2 (approved by the phone's owner on 12 November): ask the GPUEB to power the GPU, the way Android's
 * gpufreq does in EB mode (drivers/gpu/mediatek/gpufreq/v2/gpufreq_v2.c: gpufreq_shared_memory_init(),
 * gpufreq_power_control()), read the Mali's own registers, and ask it to power it off again. The GPUEB's firmware picks
 * the rails' voltages and the order; the loader writes no rail, clock or power register itself.
 *
 * The mailbox (mtk-mbox.c, mtk_rpmsg_mbox.c, gpueb_ipi.c; the dtb's gpueb node): one mailbox, "direct" shared memory of
 * 0xa0 four-byte slots at 0x13c2fd80, every send pin then every receive pin laid out in the order of send-table and
 * recv-table. Send sizes 4 6 3 6 9 4 6 6 1 4 3, receive sizes 4 6 1 1 1 4 1 6 4 1 3: IPI_ID_GPUFREQ (channel 1) sends
 * in slots 4..9 and is answered in slots 56..61. To send: bit 1 of the send status (0x13c62000) clear, the six words
 * written, bit 1 written to the set register (0x13c62004). The answer: bit 1 in the receive status (0x13c62078),
 * cleared by writing it to 0x13c62074. The message is struct gpufreq_ipi_data (gpufreq_ipi.h): cmd_id, target, then
 * a union at offset 8 (shared_mem: u64 base, u32 size; power_state; return_value).
 *
 * Before any command Linux tells the GPUEB where its status page is (CMD_INIT_SHARED_MEM): the start of the memory
 * LK reserved for it, which LK writes into the gpueb node as gpueb_mem_addr and gpueb_mem_size; the page is zeroed
 * first, as Linux does. Android repeats all of this when it boots after the probe.
 */
#if GPU_PROBE >= 2 || XNU_GPU

#define GPUEB_SMEM              0x13c2fd80ULL
#define GPUEB_MBOX_SEND_STA     0x13c62000ULL
#define GPUEB_MBOX_SET          0x13c62004ULL
#define GPUEB_MBOX_CLR          0x13c62074ULL
#define GPUEB_MBOX_RECV_STA     0x13c62078ULL
#define GPUFREQ_PIN             1u
#define GPUFREQ_SEND_SLOT       4u
#define GPUFREQ_RECV_SLOT       56u
#define GPUFREQ_WORDS           6u
#define GPUFREQ_MEM_SIZE        0x4000u     /* gpueb-mem-table: MEM_ID_GPUFREQ */
#define CMD_INIT_SHARED_MEM     0u
#define CMD_POWER_CONTROL       6u

/* gpueb_mem_addr and gpueb_mem_size of the node compatible with "mediatek,gpueb", from LK's tree */
static int gpueb_mem_from_dtb(uint64_t *addr, uint64_t *size)
{
	const uint8_t *fdt = (const uint8_t *)(uintptr_t)lk_dtb;
	const uint8_t *p, *end;
	const char *strings;
	int depth = -1, in_gpueb = -1;
	uint64_t a = 0, sz = 0;

	if (lk_dtb == 0 || dt_be32(fdt) != FDT_MAGIC) {
		return 0;
	}
	p = fdt + dt_be32(fdt + 8);
	strings = (const char *)(fdt + dt_be32(fdt + 12));
	end = p + dt_be32(fdt + 36);
	while (p < end) {
		uint32_t token = dt_be32(p);

		p += 4;
		if (token == FDT_BEGIN_NODE) {
			p += (dt_len((const char *)p) + 1 + 3) & ~3u;
			depth++;
			a = sz = 0;
		} else if (token == FDT_END_NODE) {
			if (in_gpueb == depth) {
				break;
			}
			depth--;
		} else if (token == FDT_PROP) {
			uint32_t len = dt_be32(p);
			const char *prop = strings + dt_be32(p + 4);
			const uint8_t *v = p + 8;

			p += 8 + ((len + 3) & ~3u);
			if (dt_streq(prop, "compatible") && dt_contains((const char *)v, len, "mediatek,gpueb")) {
				in_gpueb = depth;
			} else if (dt_streq(prop, "gpueb_mem_addr") && (len == 8 || len == 4)) {
				a = fdt_cells(v, len / 4);
			} else if (dt_streq(prop, "gpueb_mem_size") && (len == 8 || len == 4)) {
				sz = fdt_cells(v, len / 4);
			}
		} else if (token == FDT_END) {
			break;
		}
	}
	if (in_gpueb < 0) {
		return 0;
	}
	*addr = a;
	*size = sz;
	return a != 0 && sz != 0;
}

/* one gpufreq command; 1 and the answer's six words if the GPUEB answered within 2 s */
static int gpufreq_ipi(const uint32_t msg[GPUFREQ_WORDS], uint32_t ans[GPUFREQ_WORDS])
{
	uint64_t t0 = dbg_ticks(), freq = dbg_freq();

	while (rd32(GPUEB_MBOX_SEND_STA) & (1u << GPUFREQ_PIN)) {
		if (dbg_ticks() - t0 > freq / 2) {
			out("  the GPUEB has not taken the last message (send status ");
			out_hex(rd32(GPUEB_MBOX_SEND_STA));
			out(")\n");
			return 0;
		}
	}
	if (rd32(GPUEB_MBOX_RECV_STA) & (1u << GPUFREQ_PIN)) {
		wr32(GPUEB_MBOX_CLR, 1u << GPUFREQ_PIN);            /* an old answer nobody read */
	}
	for (uint32_t i = 0; i < GPUFREQ_WORDS; i++) {
		wr32(GPUEB_SMEM + 4u * (GPUFREQ_SEND_SLOT + i), msg[i]);
	}
	__asm__ volatile("dsb sy");
	wr32(GPUEB_MBOX_SET, 1u << GPUFREQ_PIN);
	t0 = dbg_ticks();
	while (!(rd32(GPUEB_MBOX_RECV_STA) & (1u << GPUFREQ_PIN))) {
		if (dbg_ticks() - t0 > 2 * freq) {
			out("  no answer in 2 s (send status ");
			out_hex(rd32(GPUEB_MBOX_SEND_STA));
			out(", receive status ");
			out_hex(rd32(GPUEB_MBOX_RECV_STA));
			out(", GPUEB PC ");
			out_hex(rd32(GPUEB_CFG + 0x71c));
			out(")\n");
			return 0;
		}
	}
	for (uint32_t i = 0; i < GPUFREQ_WORDS; i++) {
		ans[i] = rd32(GPUEB_SMEM + 4u * (GPUFREQ_RECV_SLOT + i));
	}
	wr32(GPUEB_MBOX_CLR, 1u << GPUFREQ_PIN);
	out("  answered after ");
	out_dec((dbg_ticks() - t0) * 1000000u / freq);
	out(" us:");
	for (uint32_t i = 0; i < GPUFREQ_WORDS; i++) {
		out(" ");
		out_hex(ans[i]);
	}
	out("\n");
	return 1;
}

#if GPU_PROBE >= 3
/*
 * GPU_PROBE=3: with the GPU on, boot its MCU on the CSF firmware, as Linux's panthor does (panthor_fw.c, panthor_mmu.c,
 * panthor_gpu.c; register names from panthor_regs.h): the firmware (Isaac's own phone's mali_csffw.bin, the payload's
 * "gpufw" entry) is copied section by section into free DRAM, a page table maps each section at the MCU address its
 * header asks for, address space 0 is given that table, the GPU is soft reset, its L2 powered and the MCU started.
 * Booted, the firmware raises the global interface interrupt and fills its global control block at the shared
 * section's start; the probe reads it and pings the firmware once. Then the MCU is halted and disabled and the GPU
 * reset again, so nothing of it reads or writes that DRAM any more, before the GPUEB powers it off.
 *
 * The loader runs with the MMU off: its stores reach DRAM uncached, so everything is mapped for the GPU non-cacheable
 * (the device tree says the GPU is not coherent) and no cache has to be cleaned.
 */
#define CSF_SCRATCH             0x180000000ULL      /* free System RAM, in XNU's window (platform.h); Android's again after */
#define CSF_PT_POOL             CSF_SCRATCH         /* page tables: 64 pages */
#define CSF_PT_PAGES            64u
#define CSF_SEC_POOL            (CSF_SCRATCH + 0x100000ULL)
#define CSF_SEC_POOL_END        (CSF_SCRATCH + 0x1000000ULL)
#define CSF_SHARED_VA           0x04000000u
#define CSF_MAX_SECTIONS        16u
/*
 * GPU_PROBE=4: one command stream group with one queue, as panthor's scheduler programs them (panthor_sched.c:
 * cs_slot_prog_locked, csg_slot_prog_locked, csgs_upd_ctx_apply_locked; panthor_fw.c: panthor_fw_init_global_iface),
 * running a five-instruction stream (encodings: Mesa's src/panfrost/genxml/v10.xml, "CS" structs) that stores two words
 * into a page of ours. The group runs in address space 1, given the same page table as the MCU's, so the queue's
 * pages sit at the same addresses for the MCU and for the stream.
 */
#define CSF_JOB_VA              0x05000000u      /* the MCU sees 28 bits: 0x10001000 was read as 0x1000 (probe 4) */
#define CSF_RING_VA             (CSF_JOB_VA + 0x0000)
#define CSF_QIN_VA              (CSF_JOB_VA + 0x1000)
#define CSF_QOUT_VA             (CSF_JOB_VA + 0x2000)
#define CSF_TARGET_VA           (CSF_JOB_VA + 0x3000)
#define CSF_SUSPEND_VA          (CSF_JOB_VA + 0x4000)
#define CSF_JOB_PAGES           (4u + 256u)          /* the four pages and up to 1 MB of suspend buffer */
#define CSF_JOB_MAGIC0          0xc0ffee01u
#define CSF_JOB_MAGIC1          0x600dcafeu


#define GPU_FAULT_STATUS        0x3c
#define GPU_FAULT_ADDR_LO       0x40
#define GPU_INT_RAWSTAT         0x20
#define GPU_INT_CLEAR           0x24
#define GPU_CMD                 0x30
#define GPU_STATUS              0x34
#define GPU_L2_PRESENT_LO       0x120
#define L2_READY_LO             0x160
#define L2_PWRON_LO             0x1a0
#define L2_PWRTRANS_LO          0x220
#define GPU_COHERENCY_PROTOCOL  0x304
#define MCU_CONTROL             0x700
#define MCU_STATUS              0x704
#define JOB_INT_RAWSTAT         0x1000
#define JOB_INT_CLEAR           0x1004
#define MMU_INT_RAWSTAT         0x2000
#define MMU_INT_CLEAR           0x2004
#define AS0                     0x2400
#define AS1                     0x2440
#define CSF_DOORBELL0           0x80000

static inline void wr8(uint64_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

struct csf_section {
	uint32_t va, va_end, flags;
	uint64_t pa;
};

static struct csf_section csf_sec[CSF_MAX_SECTIONS];
static uint32_t csf_nsec, csf_pt_used;

static uint32_t mali_rd(uint32_t off)
{
	return rd32(MALI_REGS + off);
}

static void mali_wr(uint32_t off, uint32_t v)
{
	wr32(MALI_REGS + off, v);
}

/* wait for (reg & mask) == want, up to us microseconds; 1 if it came */
static int mali_wait(uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t t0 = dbg_ticks(), lim = dbg_freq() * us / 1000000u;

	while ((mali_rd(off) & mask) != want) {
		if (dbg_ticks() - t0 > lim) {
			return 0;
		}
	}
	return 1;
}

static void mali_show(const char *name, uint32_t off)
{
	gpu_reg(name, MALI_REGS + off);
}

static void csf_faults(void)
{
	mali_show("MCU_STATUS", MCU_STATUS);
	mali_show("GPU_STATUS", GPU_STATUS);
	mali_show("GPU_INT_RAWSTAT", GPU_INT_RAWSTAT);
	mali_show("GPU_FAULTSTATUS", GPU_FAULT_STATUS);
	mali_show("GPU_FAULTADDRESS", GPU_FAULT_ADDR_LO);
	mali_show("JOB_INT_RAWSTAT", JOB_INT_RAWSTAT);
	mali_show("MMU_INT_RAWSTAT", MMU_INT_RAWSTAT);
	mali_show("AS0_FAULTSTATUS", AS0 + 0x1c);
	mali_show("AS0_FAULTADDRESS_LO", AS0 + 0x20);
	mali_show("AS0_FAULTADDRESS_HI", AS0 + 0x24);
	mali_show("AS0_STATUS", AS0 + 0x28);
	plog_flush();
}

static uint64_t csf_pt_alloc(void)
{
	if (csf_pt_used >= CSF_PT_PAGES) {
		return 0;
	}
	uint64_t pa = CSF_PT_POOL + 0x1000ULL * csf_pt_used++;
	for (uint32_t i = 0; i < 0x1000; i += 4) {
		wr32(pa + i, 0);
	}
	return pa;
}

/* one 4 KB page, LPAE stage 1 as io-pgtable-arm writes it for panthor; attribute index 0 = non-cacheable */
static int csf_map_page(uint64_t l0, uint64_t va, uint64_t pa, uint32_t flags)
{
	uint64_t t = l0;

	for (int lvl = 0; lvl < 3; lvl++) {
		uint64_t slot = t + 8u * ((va >> (39 - 9 * lvl)) & 0x1ff);
		uint64_t e = (uint64_t)rd32(slot) | ((uint64_t)rd32(slot + 4) << 32);

		if (!(e & 1)) {
			uint64_t n = csf_pt_alloc();
			if (!n) {
				return 0;
			}
			e = n | 3u;                                 /* table */
			wr32(slot, (uint32_t)e);
			wr32(slot + 4, (uint32_t)(e >> 32));
		}
		t = e & 0xfffffffff000ULL;
	}
	uint64_t pte = pa | 3u                                  /* page */
	    | (0u << 2)                                         /* AttrIndx 0 */
	    | (1u << 6)                                         /* AP: unprivileged */
	    | (2u << 8)                                         /* outer shareable (io-pgtable: no IOMMU_CACHE) */
	    | (1u << 10);                                       /* access flag */
	if (!(flags & 2u)) {
		pte |= 2u << 6;                                 /* read only */
	}
	if (!(flags & 4u)) {
		pte |= 3ULL << 53;                              /* no execute */
	}
	uint64_t slot = t + 8u * ((va >> 12) & 0x1ff);
	wr32(slot, (uint32_t)pte);
	wr32(slot + 4, (uint32_t)(pte >> 32));
	return 1;
}

static uint64_t csf_va_to_pa(uint32_t va)
{
	for (uint32_t i = 0; i < csf_nsec; i++) {
		if (va >= csf_sec[i].va && va < csf_sec[i].va_end) {
			return csf_sec[i].pa + (va - csf_sec[i].va);
		}
	}
	return 0;
}

/* the firmware's sections into DRAM and the page table; 0 if anything is off */
static uint64_t csf_load(const uint8_t *fw, uint32_t fw_size)
{
	uint64_t l0, next = CSF_SEC_POOL;
	uint32_t off;

	if (fw_size < 20 || *(const uint32_t *)fw != 0xc3f13a6eu) {
		out("  not a CSF firmware (magic)\n");
		return 0;
	}
	out("  firmware ");
	out_dec(fw_size);
	out(" bytes, version ");
	out_dec(fw[5]);
	out(".");
	out_dec(fw[4]);
	out(", hash ");
	out_hex(*(const uint32_t *)(fw + 8));
	out("\n");
	csf_pt_used = 0;
	csf_nsec = 0;
	uint32_t entries_end = *(const uint32_t *)(fw + 16);   /* the header's size: where the entries stop */
	if (entries_end > fw_size) {
		out("  header size beyond the file\n");
		return 0;
	}
	l0 = csf_pt_alloc();
	off = 20;
	while (off + 4 <= entries_end) {
		uint32_t ehdr = *(const uint32_t *)(fw + off), esz = (ehdr >> 8) & 0xff, type = ehdr & 0xff;

		if (esz < 4 || (esz & 3) || off + esz > entries_end) {
			out("  bad entry at ");
			out_hex(off);
			out("\n");
			return 0;
		}
		if (type == 0 && esz >= 24) {
			const uint32_t *w = (const uint32_t *)(fw + off + 4);
			uint32_t flags = w[0], va = w[1], va_end = w[2], ds = w[3], de = w[4];

			out("  section flags ");
			out_hex(flags);
			out(" va ");
			out_hex(va);
			out("-");
			out_hex(va_end);
			out(" data ");
			out_hex(ds);
			out("-");
			out_hex(de);
			if (flags & (1u << 5)) {
				out(": protected mode, skipped (as panthor)\n");
			} else if ((va & 0xfff) || (va_end & 0xfff) || va_end <= va || de < ds || de > fw_size ||
			    de - ds > va_end - va || csf_nsec >= CSF_MAX_SECTIONS ||
			    next + (va_end - va) > CSF_SEC_POOL_END) {
				out(": cannot be placed\n");
				return 0;
			} else {
				uint32_t len = va_end - va;

				/* bytes: the data's start need not be 4-aligned and this DRAM is device memory to us */
				for (uint32_t i = 0; i < de - ds; i++) {
					wr8(next + i, fw[ds + i]);
				}
				for (uint32_t i = de - ds; i < len; i++) {
					wr8(next + i, 0);
				}
				for (uint32_t p = 0; p < len; p += 0x1000) {
					if (!csf_map_page(l0, va + p, next + p, flags)) {
						out(": out of page tables\n");
						return 0;
					}
				}
				csf_sec[csf_nsec].va = va;
				csf_sec[csf_nsec].va_end = va_end;
				csf_sec[csf_nsec].flags = flags;
				csf_sec[csf_nsec].pa = next;
				csf_nsec++;
				out(" -> ");
				out_hex(next);
				out("\n");
				next += len;
			}
		}
		off += esz;
	}
#if GPU_PROBE >= 4
	if (csf_nsec >= CSF_MAX_SECTIONS || next + 0x1000ULL * CSF_JOB_PAGES > CSF_SEC_POOL_END) {
		out("  no room for the job's pages\n");
		return 0;
	}
	for (uint32_t i = 0; i < 0x1000u * CSF_JOB_PAGES; i += 4) {
		wr32(next + i, 0);
	}
	for (uint32_t p = 0; p < 0x1000u * CSF_JOB_PAGES; p += 0x1000) {
		if (!csf_map_page(l0, CSF_JOB_VA + p, next + p, 3u)) {      /* read-write, no execute */
			out("  out of page tables\n");
			return 0;
		}
	}
	csf_sec[csf_nsec].va = CSF_JOB_VA;
	csf_sec[csf_nsec].va_end = CSF_JOB_VA + 0x1000u * CSF_JOB_PAGES;
	csf_sec[csf_nsec].flags = 3u;
	csf_sec[csf_nsec].pa = next;
	csf_nsec++;
	out("  the job's pages: ");
	out_hex(CSF_JOB_VA);
	out(" -> ");
	out_hex(next);
	out("\n");
#endif
	__asm__ volatile("dsb sy");
	out("  ");
	out_dec(csf_nsec);
	out(" sections, ");
	out_dec(csf_pt_used);
	out(" page-table pages, top table at ");
	out_hex(l0);
	out("\n");
	if (!csf_va_to_pa(CSF_SHARED_VA)) {
		out("  no shared section at 0x4000000\n");
		return 0;
	}
	return l0;
}

static void csf_soft_reset(void)
{
	mali_wr(GPU_INT_CLEAR, 1u << 8);
	mali_wr(GPU_CMD, 0x101);                                /* SOFT_RESET */
	out(mali_wait(GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 100000) ? "  reset done\n" : "  reset: no RESET_COMPLETED in 100 ms\n");
	mali_wr(GPU_INT_CLEAR, 0xffffffffu);
	mali_wr(JOB_INT_CLEAR, 0xffffffffu);
	mali_wr(MMU_INT_CLEAR, 0xffffffffu);
}

static int csf_as_cmd(uint32_t as, uint32_t cmd)
{
	if (!mali_wait(as + 0x28, 1, 0, 100000)) {
		out("  address space stays busy\n");
		return 0;
	}
	mali_wr(as + 0x18, cmd);
	if (!mali_wait(as + 0x28, 1, 0, 100000)) {
		out("  address space command did not finish\n");
		return 0;
	}
	return 1;
}

static void csf_stop(void)
{
	out("gpu: stopping the MCU\n");
	plog_flush();
	mali_wr(MCU_CONTROL, 0);                                /* DISABLE */
	out(mali_wait(MCU_STATUS, 0xffffffffu, 0, 100000) ? "  MCU disabled\n" : "  MCU did not stop in 100 ms\n");
	csf_soft_reset();                                       /* also forgets AS0: the DRAM is not touched again */
	mali_show("MCU_STATUS", MCU_STATUS);
	mali_show("AS0_TRANSTAB_LO", AS0);
	plog_flush();
}

#if GPU_PROBE >= 4
static uint64_t csf_cs_instr(uint32_t op, uint64_t data)
{
	return ((uint64_t)op << 56) | (data & 0x00ffffffffffffffULL);
}

static void wr64(uint64_t a, uint64_t v)
{
	wr32(a, (uint32_t)v);
	wr32(a + 4, (uint32_t)(v >> 32));
}

/* req's bits in mask made to differ from ack's (panthor_fw_toggle_reqs) */
static void csf_toggle(uint64_t req_pa, uint32_t ack, uint32_t mask)
{
	wr32(req_pa, (rd32(req_pa) & ~mask) | ((ack ^ mask) & mask));
}

/* wait until (ack & mask) == (req & mask), up to ms */
static int csf_wait_ack(uint64_t req_pa, uint64_t ack_pa, uint32_t mask, uint32_t ms)
{
	uint64_t t0 = dbg_ticks(), lim = dbg_freq() * ms / 1000u;

	while ((rd32(ack_pa) & mask) != (rd32(req_pa) & mask)) {
		if (dbg_ticks() - t0 > lim) {
			return 0;
		}
	}
	return 1;
}

static uint32_t csf_timer(uint32_t us)
{
	uint64_t c = ((uint64_t)us * dbg_freq() + (1000000ULL << 10) - 1) / (1000000ULL << 10);
	return (uint32_t)(c & 0x7fffffffu);
}

static void csf_show_cs(uint64_t cso)
{
	out("  CS output: ack ");
	out_hex(rd32(cso));
	out(", cmd ptr ");
	out_hex(rd32(cso + 0x40));
	out(", wait ");
	out_hex(rd32(cso + 0x48));
	out(", blocked ");
	out_hex(rd32(cso + 0x60));
	out(", fault ");
	out_hex(rd32(cso + 0x80));
	out(" info ");
	out_hex(rd32(cso + 0x88));
	out(", fatal ");
	out_hex(rd32(cso + 0x90 - 0xc));
	out(" info ");
	out_hex(rd32(cso + 0x90));
	out("\n");
}

static void csf_job(uint64_t ctl, uint64_t gin, uint64_t gout)
{
	uint32_t shader = mali_rd(0x100);
	uint64_t csg_ctl = ctl + 0x1000, cs_ctl = csg_ctl + 0x40;
	uint64_t csgi = csf_va_to_pa(rd32(csg_ctl + 4)), csgo = csf_va_to_pa(rd32(csg_ctl + 8));
	uint64_t csi = csf_va_to_pa(rd32(cs_ctl + 4)), cso = csf_va_to_pa(rd32(cs_ctl + 8));
	uint64_t ring = csf_va_to_pa(CSF_RING_VA), qin = csf_va_to_pa(CSF_QIN_VA), qout = csf_va_to_pa(CSF_QOUT_VA);
	uint64_t target = csf_va_to_pa(CSF_TARGET_VA);
	uint32_t suspend = rd32(csg_ctl + 0xc), n;

	out("gpu: command stream group 0: features ");
	out_hex(rd32(csg_ctl));
	out(", suspend size ");
	out_hex(suspend);
	out(", streams ");
	out_dec(rd32(csg_ctl + 0x14));
	out(" (stride ");
	out_hex(rd32(csg_ctl + 0x18));
	out("), stream 0 features ");
	out_hex(rd32(cs_ctl));
	out("\n");
	if (!csgi || !csgo || !csi || !cso || !ring || !target || suspend > 256u * 0x1000u) {
		out("  an interface or our pages are not where expected: no job\n");
		plog_flush();
		return;
	}

	/* global setup, as panthor_fw_init_global_iface */
	out("gpu: global setup\n");
	plog_flush();
	wr64(gin + 0x18, shader);                               /* core_en_mask */
	wr32(gin + 0x14, csf_timer(10000));                     /* poweroff_timer */
	wr32(gin + 0x10, (uint32_t)((5ULL * 500 * 1024 * 1024) >> 10)); /* progress_timer */
	wr32(gin + 0x80, csf_timer(800));                       /* idle_timer */
	wr32(gin + 0x04, (1u << 2) | (1u << 8) | (1u << 1) | (1u << 3) | (1u << 10) | (1u << 26));
	wr32(gin, rd32(gin) | (1u << 10) | (1u << 7));          /* GLB_IDLE_EN, GLB_COUNTER_EN */
	csf_toggle(gin, rd32(gout), (1u << 2) | (1u << 3) | (1u << 1));
	__asm__ volatile("dsb sy");
	mali_wr(CSF_DOORBELL0, 1);
	out(csf_wait_ack(gin, gout, (1u << 2) | (1u << 3) | (1u << 1), 100) ? "  acknowledged\n" : "  not acknowledged in 100 ms\n");

	/* AS1 = the MCU's table */
	mali_wr(AS1 + 0x00, mali_rd(AS0 + 0x00));
	mali_wr(AS1 + 0x04, mali_rd(AS0 + 0x04));
	mali_wr(AS1 + 0x08, 0x4c4c4c4cu);
	mali_wr(AS1 + 0x0c, 0x4c4c4c4cu);
	mali_wr(AS1 + 0x30, mali_rd(AS0 + 0x30));
	mali_wr(AS1 + 0x34, 0);
	if (!csf_as_cmd(AS1, 1)) {
		return;
	}

	/* the stream: r2:r3 = target; r0, r1 = the words; store both; wait for the store */
	n = 0;
	wr64(ring + 8 * n++, csf_cs_instr(1, (2ULL << 48) | CSF_TARGET_VA));           /* MOVE48 r2, target */
	wr64(ring + 8 * n++, csf_cs_instr(2, (0ULL << 48) | CSF_JOB_MAGIC0));          /* MOVE32 r0 */
	wr64(ring + 8 * n++, csf_cs_instr(2, (1ULL << 48) | CSF_JOB_MAGIC1));          /* MOVE32 r1 */
	wr64(ring + 8 * n++, csf_cs_instr(21, (0ULL << 48) | (2ULL << 40) | (3ULL << 16))); /* STORE_MULTIPLE r0-r1 -> [r2] */
	wr64(ring + 8 * n++, csf_cs_instr(3, 0xffULL << 16));                           /* WAIT all scoreboards */
	wr32(target, 0);
	wr32(target + 4, 0);
	wr64(qin, 0);                                           /* insert */
	wr64(qin + 8, 0);                                       /* extract */
	wr64(qout, 0);

	/* the queue, as cs_slot_prog_locked: priority 0, doorbell 1 */
	out("gpu: queue and group start\n");
	plog_flush();
	wr64(csi + 0x10, CSF_RING_VA);
	wr32(csi + 0x18, 0x1000);
	wr64(csi + 0x30, CSF_QIN_VA);
	wr64(csi + 0x38, CSF_QOUT_VA);
	wr32(csi + 0x04, 1u << 8);                              /* CS_CONFIG_DOORBELL(1) */
	wr32(csi + 0x0c, 0xffffffffu);
	wr32(csi, (rd32(csi) & ~((1u << 8) | (1u << 10) | 7u)) | (1u << 8) | (1u << 10) | 1u); /* IDLE_SYNC_WAIT|IDLE_EMPTY|START */

	/* the group, as csg_slot_prog_locked and the scheduler's start */
	wr64(csgi + 0x20, shader);                              /* allow_compute */
	wr64(csgi + 0x28, shader);                              /* allow_fragment */
	wr32(csgi + 0x30, 1);                                   /* allow_other (tiler) */
	n = (uint32_t)__builtin_popcount(shader);
	wr32(csgi + 0x34, n | (n << 8) | (1u << 16) | (0xfu << 28)); /* endpoint_req: cores, tiler 1, priority 15 */
	wr64(csgi + 0x40, CSF_SUSPEND_VA);
	wr64(csgi + 0x48, 0);
	wr32(csgi + 0x50, 1);                                   /* config: address space 1 */
	wr32(csgi + 0x04, 0xffffffffu);
	csf_toggle(csgi + 0x08, rd32(csgo + 0x08), 1u);         /* doorbell_req: stream 0 */
	{
		uint32_t req = rd32(csgi), ack = rd32(csgo);
		req = (req & ~7u) | 1u;                         /* CSG_STATE_START */
		req = (req & ~(1u << 4)) | ((ack ^ (1u << 4)) & (1u << 4)); /* ENDPOINT_CONFIG */
		wr32(csgi, req);
	}
	csf_toggle(gin + 0x08, rd32(gout + 0x08), 1u);          /* global doorbell_req: group 0 */
	__asm__ volatile("dsb sy");
	mali_wr(CSF_DOORBELL0, 1);
	if (!csf_wait_ack(csgi, csgo, 7u | (1u << 4), 100)) {
		out("  the group did not start in 100 ms (CSG req ");
		out_hex(rd32(csgi));
		out(", ack ");
		out_hex(rd32(csgo));
		out(")\n");
		csf_show_cs(cso);
		csf_faults();
		return;
	}
	out("  group running (CSG ack ");
	out_hex(rd32(csgo));
	out(", state ");
	out_hex(rd32(csgo + 0x18));
	out("), CS ack ");
	out_hex(rd32(cso));
	out("\n");

	/* submit, as queue_run_job: insert moves to the stream's end, then the queue's doorbell */
	out("gpu: submit\n");
	plog_flush();
	wr64(qin, 8u * 5u);
	__asm__ volatile("dsb sy");
	mali_wr(CSF_DOORBELL0 + 0x10000, 1);
	uint64_t t0 = dbg_ticks();
	while (rd32(target + 4) != CSF_JOB_MAGIC1 && dbg_ticks() - t0 < dbg_freq() / 2) {
	}
	out("  after ");
	out_dec((dbg_ticks() - t0) * 1000000u / dbg_freq());
	out(" us the target holds ");
	out_hex(rd32(target));
	out(" ");
	out_hex(rd32(target + 4));
	out(rd32(target) == CSF_JOB_MAGIC0 && rd32(target + 4) == CSF_JOB_MAGIC1 ?
	    ": THE GPU RAN OUR COMMAND STREAM\n" : ": not written\n");
	out("  queue output: extract ");
	out_hex(rd32(qout));
	out(", active ");
	out_hex(rd32(qout + 8));
	out("\n");
	csf_show_cs(cso);
	plog_flush();

	/* terminate the group */
	out("gpu: group terminate\n");
	wr32(csgi, rd32(csgi) & ~7u);                           /* CSG_STATE_TERMINATE */
	csf_toggle(gin + 0x08, rd32(gout + 0x08), 1u);
	__asm__ volatile("dsb sy");
	mali_wr(CSF_DOORBELL0, 1);
	out(csf_wait_ack(csgi, csgo, 7u, 100) ? "  terminated\n" : "  not terminated in 100 ms\n");
	mali_show("MMU_INT_RAWSTAT", MMU_INT_RAWSTAT);
	mali_show("AS1_FAULTSTATUS", AS1 + 0x1c);
	mali_show("AS1_FAULTADDRESS_LO", AS1 + 0x20);
	plog_flush();
}
#endif /* GPU_PROBE >= 4 */

static void gpu_csf_boot(void)
{
	const uint8_t *fw;
	uint32_t fw_size = 0, l2;
	uint64_t l0, t0;

	out("gpu: CSF firmware and MCU (GPU_PROBE=3)\n");
	fw = payload_entry("gpufw", &fw_size);
	if (!fw) {
		out("  the image has no \"gpufw\" payload entry: no MCU boot\n");
		plog_flush();
		return;
	}
	l0 = csf_load(fw, fw_size);
	plog_flush();
	if (!l0) {
		return;
	}

	out("gpu: soft reset\n");
	plog_flush();
	csf_soft_reset();
	mali_wr(GPU_COHERENCY_PROTOCOL, 31);                    /* NONE: the device tree's system-coherency = 0 */

	out("gpu: L2 on\n");
	plog_flush();
	l2 = mali_rd(GPU_L2_PRESENT_LO);
	mali_wr(L2_PWRON_LO, l2);
	if (!mali_wait(L2_PWRTRANS_LO, l2, 0, 20000) || !mali_wait(L2_READY_LO, l2, l2, 20000)) {
		out("  L2 did not come up\n");
		mali_show("L2_PWRTRANS_LO", L2_PWRTRANS_LO);
		mali_show("L2_READY_LO", L2_READY_LO);
		csf_faults();
		csf_stop();
		return;
	}
	mali_show("L2_READY_LO", L2_READY_LO);
	mali_show("MMU_INT_RAWSTAT", MMU_INT_RAWSTAT);

	out("gpu: address space 0 (the MCU's)\n");
	plog_flush();
	mali_wr(AS0 + 0x00, (uint32_t)l0);                      /* TRANSTAB */
	mali_wr(AS0 + 0x04, (uint32_t)(l0 >> 32));
	mali_wr(AS0 + 0x08, 0x4c4c4c4cu);                       /* MEMATTR: all 8 non-cacheable (mair_to_memattr of 0x44) */
	mali_wr(AS0 + 0x0c, 0x4c4c4c4cu);
	mali_wr(AS0 + 0x30, (1u << 24) | (7u << 6) | 6u);       /* TRANSCFG: walks NC, 48-bit VA (INA 55-48), AARCH64_4K */
	mali_wr(AS0 + 0x34, 0);
	if (!csf_as_cmd(AS0, 1)) {                                   /* UPDATE */
		csf_faults();
		csf_stop();
		return;
	}
	mali_show("AS0_TRANSCFG_LO", AS0 + 0x30);
	mali_show("MMU_INT_RAWSTAT", MMU_INT_RAWSTAT);

	out("gpu: MCU_CONTROL = AUTO\n");
	plog_flush();
	mali_wr(JOB_INT_CLEAR, 0xffffffffu);
	mali_wr(MCU_CONTROL, 2);
	t0 = dbg_ticks();
	if (!mali_wait(JOB_INT_RAWSTAT, 1u << 31, 1u << 31, 1000000)) {
		out("  no global interface interrupt in 1 s\n");
		csf_faults();
		csf_stop();
		return;
	}
	out("  booted after ");
	out_dec((dbg_ticks() - t0) * 1000000u / dbg_freq());
	out(" us\n");
	mali_wr(JOB_INT_CLEAR, 1u << 31);
	mali_show("MCU_STATUS", MCU_STATUS);
	mali_show("MMU_INT_RAWSTAT", MMU_INT_RAWSTAT);

	uint64_t ctl = csf_va_to_pa(CSF_SHARED_VA);
	uint32_t version = rd32(ctl), in_va = rd32(ctl + 8), out_va = rd32(ctl + 12);
	out("gpu: global control interface\n");
	out("  version ");
	out_dec(version >> 24);
	out(".");
	out_dec((version >> 16) & 0xff);
	out(".");
	out_dec(version & 0xffff);
	out(" (");
	out_hex(version);
	out("), features ");
	out_hex(rd32(ctl + 4));
	out(", input ");
	out_hex(in_va);
	out(", output ");
	out_hex(out_va);
	out("\n  groups ");
	out_dec(rd32(ctl + 16));
	out(", group stride ");
	out_hex(rd32(ctl + 20));
	out(", perfcnt size ");
	out_hex(rd32(ctl + 24));
	out(", instr features ");
	out_hex(rd32(ctl + 28));
	out("\n");
	plog_flush();

	uint64_t in = csf_va_to_pa(in_va), outp = csf_va_to_pa(out_va);
	if (in && outp) {
		uint32_t req = rd32(in), ack = rd32(outp), want;

		out("  GLB_REQ ");
		out_hex(req);
		out(", GLB_ACK ");
		out_hex(ack);
		out("\ngpu: ping\n");
		plog_flush();
		want = (req & ~(1u << 8)) | (~ack & (1u << 8));   /* toggle GLB_PING against the ack */
		wr32(in, want);
		__asm__ volatile("dsb sy");
		mali_wr(CSF_DOORBELL0, 1);
		t0 = dbg_ticks();
		while ((rd32(outp) & (1u << 8)) != (want & (1u << 8)) && dbg_ticks() - t0 < dbg_freq() / 10) {
		}
		if ((rd32(outp) & (1u << 8)) == (want & (1u << 8))) {
			out("  the firmware answered the ping after ");
			out_dec((dbg_ticks() - t0) * 1000000u / dbg_freq());
			out(" us (GLB_ACK ");
			out_hex(rd32(outp));
			out(")\n");
#if GPU_PROBE >= 4
			plog_flush();
			csf_job(ctl, in, outp);
#endif
		} else {
			out("  no answer to the ping in 100 ms (GLB_ACK ");
			out_hex(rd32(outp));
			out(")\n");
		}
		plog_flush();

		/* halt, as panthor_fw_halt_mcu: GLB_STATE from interface 4.1, the GLB_HALT bit before */
		out("gpu: halt request\n");
		req = rd32(in);
		if (version >= 0x04010000u) {
			req = (req & ~(7u << 12)) | (1u << 12);
		} else {
			req |= 1u;
		}
		wr32(in, req);
		__asm__ volatile("dsb sy");
		mali_wr(CSF_DOORBELL0, 1);
		out(mali_wait(MCU_STATUS, 0xffffffffu, 2, 100000) ? "  MCU halted\n" : "  MCU did not halt in 100 ms\n");
	} else {
		out("  input or output block outside the sections\n");
	}
	csf_faults();
	csf_stop();
}
#endif /* GPU_PROBE >= 3 */

static void gpu_power_test(void)
{
	uint64_t mem = 0, mem_size = 0;
	uint32_t msg[GPUFREQ_WORDS], ans[GPUFREQ_WORDS];
	int on;

	out("gpu: asking the GPUEB (GPU_PROBE=2)\n");
	if (!gpueb_mem_from_dtb(&mem, &mem_size)) {
		out("  LK's tree has no gpueb_mem_addr / gpueb_mem_size: nothing is asked\n");
		plog_flush();
		return;
	}
	out("  its memory from LK's tree: ");
	out_hex(mem);
	out(" + ");
	out_hex(mem_size);
	out("\n");
	if (mem < 0x40000000ULL || mem >= 0x800000000ULL || mem_size < GPUFREQ_MEM_SIZE) {
		out("  outside what the GPUEB can reach, or too small: nothing is asked\n");
		plog_flush();
		return;
	}
	for (uint32_t i = 0; i < GPUFREQ_MEM_SIZE; i += 4) {
		wr32(mem + i, 0);
	}
	__asm__ volatile("dsb sy");

	out("gpu: CMD_INIT_SHARED_MEM\n");
	plog_flush();
	memset(msg, 0, sizeof(msg));
	msg[0] = CMD_INIT_SHARED_MEM;
	msg[2] = (uint32_t)mem;
	msg[3] = (uint32_t)(mem >> 32);
	msg[4] = GPUFREQ_MEM_SIZE;
	if (!gpufreq_ipi(msg, ans)) {
		plog_flush();
		return;
	}
	out("  status page, first words:");
	for (uint32_t i = 0; i < 16; i++) {
		out(" ");
		out_hex(rd32(mem + 4u * i));
	}
	out("\n");

	out("gpu: CMD_POWER_CONTROL on\n");
	plog_flush();
	memset(msg, 0, sizeof(msg));
	msg[0] = CMD_POWER_CONTROL;
	msg[2] = 1;                                 /* GPU_PWR_ON */
	on = gpufreq_ipi(msg, ans);
	if (on) {
		out("  return value (the power count, 1 = now on): ");
		out_dec(ans[2]);
		out("\n");
		dbg_delay_ms(5);
		out("gpu: after power on\n");
		gpu_pmic("VGPU (MT6368 VBUCK2)", MT6368_SID, MT6368_VBUCK2_EN, 2, MT6368_VBUCK2_VOSEL, 0xff, 0);
		gpu_pmic("VSRAM (MT6363 VSRAM_MDFE)", MT6363_SID, MT6363_VSRAM_MDFE_EN, 5, MT6363_VSRAM_MDFE_VOSEL, 0x7f, 400000);
		gpu_state();
#if GPU_PROBE >= 3
		if (ans[2] >= 1 && ((rd32(MFG_RPC_MFG1_PWR_CON) >> 30) & 3u) == 3u) {
			gpu_csf_boot();
		} else {
			out("gpu: the GPU did not come on: no MCU boot\n");
		}
#endif
	}

	out("gpu: CMD_POWER_CONTROL off\n");
	plog_flush();
	memset(msg, 0, sizeof(msg));
	msg[0] = CMD_POWER_CONTROL;
	msg[2] = 0;                                 /* GPU_PWR_OFF */
	if (gpufreq_ipi(msg, ans)) {
		out("  return value (the power count, 0 = off again): ");
		out_dec(ans[2]);
		out("\n");
	}
	out("gpu: after power off\n");
	out_hex(rd32(MFG_RPC_PWR_STA));
	out(" MFG_RPC_PWR_CON_STATUS, ");
	out_hex(rd32(MFG_RPC_MFG1_PWR_CON));
	out(" MFG1_PWR_CON\n");
	plog_flush();
}
#endif /* GPU_PROBE >= 2 || XNU_GPU */

#if XNU_GPU
/*
 * XNU_GPU=1: what the kernel's GPU driver (xnu/files/bsd/dev/redwood_mtkgpu.c) cannot find by itself. The firmware (the
 * payload's "gpufw" entry) is copied to the start of the GPU's memory, 0x230000000, above XNU's window and the loader's
 * 2 MB; the GPUEB's memory comes from LK's tree. 1 and both if all is there.
 */
#define XNU_GPU_MEM_PA          0x230000000ULL
#define XNU_GPU_FW_MAX          0x100000u

static int gpu_for_xnu(uint32_t *fw_size, uint64_t *eb_mem)
{
	uint32_t size = 0;
	uint64_t eb_size = 0;
	const uint8_t *fw = payload_entry("gpufw", &size);

	if (!fw || size < 20 || size > XNU_GPU_FW_MAX || *(const uint32_t *)fw != 0xc3f13a6eu) {
		out("gpu: no CSF firmware in the payload (\"gpufw\"): the kernel's GPU driver stays off\n");
		return 0;
	}
	if (!gpueb_mem_from_dtb(eb_mem, &eb_size) || eb_size < GPUFREQ_MEM_SIZE) {
		out("gpu: no gpueb_mem_addr in LK's tree: the kernel's GPU driver stays off\n");
		return 0;
	}
	for (uint32_t i = 0; i < size; i += 4) {      /* the entry is 16-aligned and padded */
		wr32(XNU_GPU_MEM_PA + i, *(const volatile uint32_t *)(fw + i));
	}
	__asm__ volatile("dsb sy");
	*fw_size = size;
	out("gpu: firmware ");
	out_dec(size);
	out(" bytes to ");
	out_hex(XNU_GPU_MEM_PA);
	out(", GPUEB memory ");
	out_hex(*eb_mem);
	out("\n");
	return 1;
}
#endif

static void gpu_probe(void)
{
	out("GPU probe (GPU_PROBE=");
	out_dec(GPU_PROBE);
	out(GPU_PROBE >= 4 ? ": the GPUEB powers the GPU, its MCU boots, one command stream runs, then all off)\n" :
	    GPU_PROBE >= 3 ? ": the GPUEB powers the GPU, its MCU boots the CSF firmware, then all off)\n" :
	    GPU_PROBE >= 2 ? ": read, then the GPUEB is asked to power the GPU on and off)\n" : ", read only)\n");
	if (gpu_state() == 0) {
		out("GPU probe done\n");
		return;
	}
#if GPU_PROBE >= 2
	gpu_power_test();
#endif
	out("GPU probe done\n");
	plog_flush();
}

#endif /* MTKGPU_H */

