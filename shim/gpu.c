/*
 * Adreno 615 bring-up on bonito (SDM670), shim stage. Ported from ios-flame's
 * shim/gpuprobe.c (Pixel 4 / A640, MIT, dipa-studio) with the A615
 * differences taken from mainline Linux drm/msm (a6xx_gmu.c, a6xx_hfi.c,
 * a6xx_gpu.c, a6xx_catalog.c) and bonito's device tree:
 *
 *   - a630 family: legacy GMU. Firmware is a raw ITCM image, HFI v1 (queue
 *     ids 0/4/5, no 4-dword alignment), perf table v1, sdm845 bw table, the
 *     boot sequence adds gfx_rail_on (BOOT_SLUMBER OOB) and SPTPRAC enable,
 *     and OOB requests use the legacy bits (GPU_SET 16/24).
 *   - PDC address offset 0x30080, PDC at 0xb280000 / seq 0xb480000.
 *   - GPU 0x5000000, GMU 0x506a000 (RSCC at GMU + 0x23000), GPUCC 0x5090000,
 *     Adreno SMMU 0x5040000; GPU stream 0, GMU stream 5.
 *   - Zap shader a615_zap (PAS 13) into pil_gpu_region 0x97a15000 (8K).
 *   - GMEM 512K, chip id 0x06010500.
 *
 * ABL leaves the GPU completely off (probe 2026-11-07), so this starts cold.
 * The MMU is off here: every CPU access is Device memory, so the GPU and GMU
 * see our writes without cache maintenance.
 *
 * Output: one short line per step on the screen (no persistent log on
 * bonito). A hung register access stalls the CPU for good; the last line
 * shows where.
 */
#include <stdint.h>
#include "console.h"
#include "gpu.h"

#define GCC		0x00100000UL
#define GPUCC		0x05090000UL
#define GPU		0x05000000UL
#define GREG(o)		(GPU + 4UL * (o))
#define GMU_REG(o)	(GPU + 4UL * (o))	/* GMU offsets are based on the GPU start */
#define SMMU		0x05040000UL
#define RSCC		(0x0506a000UL + 0x23000)
#define RSCC_REG(o)	(RSCC + 4UL * (o))
#define PDC		0x0b280000UL
#define PDC_REG(o)	(PDC + 4UL * (o))
#define PDC_SEQ		0x0b480000UL
#define CMD_DB		0x85fe0000UL	/* reserved-memory aop-cmd-db-mem */

#define GPUCC_CX_GDSCR	0x106c
#define GPUCC_GX_GDSCR	0x100c
#define GPUCC_GMU_RCG	0x1120
#define GPUCC_CXO	0x109c
#define GPUCC_CX_GMU	0x1098

/* GMU registers (a6xx_gmu.xml, dword offsets from the GPU base) */
#define GMU_GX_SPTPRAC_PWR	0x1a881
#define GMU_ITCM		0x1b400
#define GMU_DTCM		0x1c400
#define GMU_BOOT_SLUMBER	0x1cbf8
#define GMU_GX_VOTE_IDX		0x1cbf9
#define GMU_MX_VOTE_IDX		0x1cbfa
#define GMU_FW_VERSION		0x1d3f8
#define GMU_SYSRESET		0x1f800
#define GMU_BOOT_CFG		0x1f801
#define GMU_INIT_RESULT		0x1f81c
#define GMU_CM3_CFG		0x1f82d
#define GMU_PWRCOL_CTRL		0x1f8c0
#define GMU_SPTPRAC_STATUS	0x1f8d0
#define GMU_RPMH_CTRL		0x1f8e8
#define GMU_RPMH_STATE		0x1f8ec
#define GMU_CP_MSG		0x1f900
#define GMU_CP_RESP		0x1f901
#define GMU_HFI_CTRL_STATUS	0x1f980
#define GMU_SFR_ADDR		0x1f982
#define GMU_QTBL_INFO		0x1f984
#define GMU_QTBL_ADDR		0x1f985
#define GMU_HFI_CTRL_INIT	0x1f986
#define GMU_G2H_CLR		0x1f991
#define GMU_G2H_INFO		0x1f992
#define GMU_H2G_SET		0x1f994
#define GMU_ICACHE_CFG		0x1f400
#define GMU_DCACHE_CFG		0x1f401
#define GMU_SYS_BUS		0x1f40f
#define GMU_GENERAL_7		0x1f9cc
#define GMU_RSCC_REQ		0x23b07
#define GMU_RSCC_ACK		0x23b08
#define GMU_FENCE0		0x23b11

/* ---- helpers ---- */

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void wr64(uintptr_t pa, uint64_t v)
{
	wr32(pa, (uint32_t)v);
	wr32(pa + 4, (uint32_t)(v >> 32));
}

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void delay_us(unsigned us)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	uint64_t end = ticks() + f / 1000000 * us;
	while (ticks() < end)
		;
}

static void delay_ms(unsigned ms)
{
	delay_us(1000 * ms);
}

static int wait_bit(uintptr_t reg, uint32_t mask, int set, unsigned ms)
{
	for (unsigned i = 0; i <= 10 * ms; i++) {
		if (!!(rd32(reg) & mask) == set)
			return 0;
		delay_us(100);
	}
	return -1;
}

static void zero32(uintptr_t pa, uint64_t size)
{
	for (uint64_t o = 0; o < size; o += 4)
		wr32(pa + o, 0);
}

static void copy_in(uintptr_t dst, const uint8_t *src, uint32_t len)
{
	for (uint32_t o = 0; o < len; o += 4) {
		uint32_t w = 0;
		for (uint32_t i = 0; i < 4 && o + i < len; i++)
			w |= (uint32_t)src[o + i] << (8 * i);
		wr32(dst + o, w);
	}
}

static uint64_t smc5(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t *r1)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	register uint64_t x4 __asm__("x4") = a4;
	register uint64_t x5 __asm__("x5") = 0;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5)
			 :
			 : "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
			   "memory");
	if (r1)
		*r1 = x1;
	return x0;
}

static int branch_on(const char *name, uintptr_t reg)
{
	wr32(reg, rd32(reg) | 1);
	int rc = wait_bit(reg, 1u << 31, 0, 10);
	con_printf("%s %x%s\n", name, rd32(reg), rc ? " HALTED" : "");
	return rc;
}

/* ---- firmware (shim/gpufw.S) ---- */

extern const uint8_t gmu_fw[], gmu_fw_end[], sqe_fw[], sqe_fw_end[];
extern const uint8_t zap_mdt[], zap_mdt_end[], zap_b02[], zap_b02_end[];

/* ---- memory: page tables and GMU/GPU buffers in high DRAM ---- */

#define GMEM		0x170000000UL
#define GMEM_SIZE	0x400000UL
#define GMU_L1		(GMEM + 0x0000)
#define GMU_L2		(GMEM + 0x1000)		/* VA 0x40000000-0x7fffffff */
#define GMU_L3		(GMEM + 0x2000)		/* VA 0x60000000-0x601fffff */
#define GMEM_DUMMY	(GMEM + 0x10000)
#define GMEM_DEBUG	(GMEM + 0x11000)
#define GMEM_LOG	(GMEM + 0x15000)
#define GMEM_HFI	(GMEM + 0x19000)
#define VA_DUMMY	0x60000000u
#define VA_DEBUG	0x60001000u
#define VA_LOG		0x60005000u
#define VA_HFI		0x60009000u
#define GT_L1		(GMEM + 0x100000)
#define GT_L2		(GMEM + 0x101000)
#define GT_L3		(GMEM + 0x102000)	/* VA 0x10000000-0x101fffff */
#define G_SQE		(GMEM + 0x200000)
#define G_RING		(GMEM + 0x210000)
#define G_TARGET	(GMEM + 0x220000)
#define ZAP_META	(GMEM + 0x230000)
#define GVA_SQE		0x10000000u
#define GVA_RING	0x10010000u
#define GVA_TARGET	0x10020000u
#define GVA_FB		0x20000000u
#define RING_DW		(0x8000 / 4)

/* ---- Adreno SMMU (arm,smmu-v2) ---- */

#define SMMU_GR1	(SMMU + 0x1000)
#define SMMU_CB(n)	(SMMU + 0x8000 + 0x1000UL * (n))
#define SMR(n)		(SMMU + 0x800 + 4 * (n))
#define S2CR(n)		(SMMU + 0xc00 + 4 * (n))
#define CBAR(n)		(SMMU_GR1 + 4 * (n))
#define CBA2R(n)	(SMMU_GR1 + 0x800 + 4 * (n))
#define CB_SCTLR	0x00
#define CB_TCR2		0x10
#define CB_TTBR0	0x20
#define CB_TCR		0x30
#define CB_MAIR0	0x38
#define CB_FSR		0x58
#define CB_FAR		0x60
#define CB_FSYNR0	0x68

#define GMU_SID		5
#define GPU_SID		0

static int n_smr, n_cb;

/* Stage-1 AArch64 4K page, AF, inner shareable, AttrIndx 0 (NC). priv: AP=00. */
static void map_pages(uintptr_t l3, uint32_t va, uintptr_t pa, uint32_t size, int priv)
{
	for (uint32_t o = 0; o < size; o += 0x1000) {
		uint32_t idx = ((va + o) >> 12) & 0x1ff;
		wr64(l3 + 8 * idx, (pa + o) | (1u << 10) | (3u << 8) | (priv ? 0 : (1u << 6)) | 3u);
	}
}

static int free_smr(void)
{
	for (int i = 0; i < n_smr; i++)
		if (!(rd32(SMR(i)) >> 31))
			return i;
	return -1;
}

static int free_cb(int after)
{
	uint32_t used = 0;
	for (int i = 0; i < n_smr; i++)
		if ((rd32(SMR(i)) >> 31) && ((rd32(S2CR(i)) >> 16) & 3) == 0)
			used |= 1u << (rd32(S2CR(i)) & 0xff);
	for (int i = after + 1; i < n_cb; i++)
		if (!(used & (1u << i)) && !(rd32(SMMU_CB(i) + CB_SCTLR) & 1))
			return i;
	return -1;
}

static void cb_setup(int cb, uintptr_t ttbr, int smr, uint32_t sid)
{
	uintptr_t c = SMMU_CB(cb);
	wr32(c + CB_SCTLR, 0);
	wr32(CBA2R(cb), 1);						/* AArch64 descriptors */
	wr32(CBAR(cb), (1u << 16) | (0xfu << 12) | (3u << 8) | (cb + 1));	/* S1, S2 bypass, VMID */
	wr32(c + CB_TCR2, 2);						/* 40-bit PA */
	wr64(c + CB_TTBR0, ttbr);
	wr32(c + CB_TCR, 32 | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23));
	wr32(c + CB_MAIR0, 0x0000ff44);
	wr32(c + CB_FSR, 0xffffffff);
	wr32(c + CB_SCTLR, 1 | 2 | 4 | 0x20 | 0x40);			/* M TRE AFE CFRE CFIE */
	wr32(SMMU + 0x68, 0);						/* TLBIALLNSNH */
	wr32(SMMU + 0x70, 0);						/* TLBGSYNC */
	wait_bit(SMMU + 0x74, 1, 0, 10);
	wr32(S2CR(smr), (uint32_t)cb);
	wr32(SMR(smr), (1u << 31) | sid);
}

static void cb_fault(const char *who, int cb)
{
	uintptr_t c = SMMU_CB(cb);
	con_printf("%s cb%d fsr %x far %x:%x syn %x gfsr %x\n", who, cb, rd32(c + CB_FSR),
		   rd32(c + CB_FAR + 4), rd32(c + CB_FAR), rd32(c + CB_FSYNR0), rd32(SMMU + 0x48));
}

/* ---- stage 1: CX power ---- */

static int cx_up(void)
{
	con_printf("1 gcc ahb %x axi %x memnoc %x snoc %x\n", rd32(GCC + 0x71004), rd32(GCC + 0x44038),
		   rd32(GCC + 0x7100c), rd32(GCC + 0x71018));
	branch_on("gcc gpu_cfg_ahb", GCC + 0x71004);
	branch_on("gcc ddrss_gpu_axi", GCC + 0x44038);
	branch_on("gcc gpu_memnoc_gfx", GCC + 0x7100c);
	branch_on("gcc gpu_snoc_dvm", GCC + 0x71018);

	uintptr_t gdsc = GPUCC + GPUCC_CX_GDSCR;
	wr32(gdsc, rd32(gdsc) & ~1u);					/* clear SW_COLLAPSE */
	int rc = wait_bit(gdsc, 1u << 31, 1, 100);
	con_printf("cx gdsc %x %s\n", rd32(gdsc), rc ? "TIMEOUT" : "on");
	if (rc)
		return -1;

	/* gmu_clk_src: XO, divider 1 (HID 1), update */
	uintptr_t rcg = GPUCC + GPUCC_GMU_RCG;
	wr32(rcg + 4, (rd32(rcg + 4) & ~0x71fu) | 1u);
	wr32(rcg, rd32(rcg) | 1u);
	rc = wait_bit(rcg, 1, 0, 10);
	con_printf("gmu rcg %x cfg %x%s\n", rd32(rcg), rd32(rcg + 4), rc ? " UPD TIMEOUT" : "");
	branch_on("gpucc cxo", GPUCC + GPUCC_CXO);
	if (branch_on("gpucc cx_gmu", GPUCC + GPUCC_CX_GMU))
		return -1;

	con_printf("gmu fw %x init %x rpmh %x sptprac %x\n", rd32(GMU_REG(GMU_FW_VERSION)),
		   rd32(GMU_REG(GMU_INIT_RESULT)), rd32(GMU_REG(GMU_RPMH_STATE)),
		   rd32(GMU_REG(GMU_SPTPRAC_STATUS)));
	uintptr_t itcm = GMU_REG(GMU_ITCM), dtcm = GMU_REG(GMU_DTCM);
	wr32(itcm, 0x464c414d);
	wr32(dtcm, 0x45475055);
	con_printf("itcm %x dtcm %x (want 464c414d 45475055)\n", rd32(itcm), rd32(dtcm));

	uint32_t idr0 = rd32(SMMU + 0x20), idr1 = rd32(SMMU + 0x24);
	n_smr = idr0 & 0xff;
	n_cb = idr1 & 0xff;
	con_printf("smmu cr0 %x idr0 %x idr1 %x: %d smr %d cb\n", rd32(SMMU), idr0, idr1, n_smr, n_cb);
	if (n_smr > 32)
		n_smr = 32;
	if (n_cb > 32)
		n_cb = 32;
	return (n_smr && n_cb) ? 0 : -1;
}

/* ---- stage 2: GMU boot ---- */

static int gmu_smmu(void)
{
	int smr = free_smr(), cb = free_cb(-1);
	con_printf("2 gmu smmu: smr %d cb %d\n", smr, cb);
	if (smr < 0 || cb < 0)
		return -1;
	zero32(GMEM, 0x30000);
	wr64(GMU_L1 + 8, GMU_L2 | 3);
	wr64(GMU_L2 + 8 * ((VA_DUMMY >> 21) & 0x1ff), GMU_L3 | 3);
	map_pages(GMU_L3, VA_DUMMY, GMEM_DUMMY, 0x1000, 0);
	map_pages(GMU_L3, VA_DEBUG, GMEM_DEBUG, 0x4000, 0);
	map_pages(GMU_L3, VA_LOG, GMEM_LOG, 0x4000, 0);
	map_pages(GMU_L3, VA_HFI, GMEM_HFI, 0x4000, 0);
	cb_setup(cb, GMU_L1, smr, GMU_SID);
	con_printf("smr%d %x s2cr %x sctlr %x\n", smr, rd32(SMR(smr)), rd32(S2CR(smr)),
		   rd32(SMMU_CB(cb) + CB_SCTLR));
	return cb;
}

/* a6xx_gmu_rpmh_init, a630 family (pdc_address_offset 0x30080) */
static void rpmh_init(void)
{
	wr32(RSCC_REG(0x0004), 1u << 24);
	wr32(RSCC_REG(0x000b), 1);
	wr32(RSCC_REG(0x000e), 0);
	wr32(RSCC_REG(0x000d), 0);
	wr32(RSCC_REG(0x000e + 2), 0);
	wr32(RSCC_REG(0x000d + 2), 0);
	wr32(RSCC_REG(0x000e + 4), 0x80000000);
	wr32(RSCC_REG(0x000d + 4), 0);
	wr32(RSCC_REG(0x0100), 0);
	wr32(RSCC_REG(0x0008), 0x4520);
	wr32(RSCC_REG(0x0009), 0x4510);
	wr32(RSCC_REG(0x000a), 0x4514);
	wr32(RSCC_REG(0x0180), 0xa7a506a0);
	wr32(RSCC_REG(0x0181), 0xa1e6a6e7);
	wr32(RSCC_REG(0x0182), 0xa2e081e1);
	wr32(RSCC_REG(0x0183), 0xe9a982e2);
	wr32(RSCC_REG(0x0184), 0x0020e8a8);

	wr32(PDC_SEQ + 0, 0xfebea1e1);
	wr32(PDC_SEQ + 4, 0xa5a4a3a2);
	wr32(PDC_SEQ + 8, 0x8382a6e0);
	wr32(PDC_SEQ + 12, 0xbce3e284);
	wr32(PDC_SEQ + 16, 0x002081fc);

	wr32(PDC_REG(0x1573), 7);
	wr32(PDC_REG(0x1574), 0);
	wr32(PDC_REG(0x1572), 0);
	wr32(PDC_REG(0x1575), 0x10108);
	wr32(PDC_REG(0x1576), 0x30010);
	wr32(PDC_REG(0x1577), 1);
	wr32(PDC_REG(0x1575 + 4), 0x10108);
	wr32(PDC_REG(0x1576 + 4), 0x30000);
	wr32(PDC_REG(0x1577 + 4), 0);
	wr32(PDC_REG(0x1575 + 8), 0x10108);
	wr32(PDC_REG(0x1576 + 8), 0x30080);
	wr32(PDC_REG(0x1577 + 8), 0);

	wr32(PDC_REG(0x15d7), 7);
	wr32(PDC_REG(0x15d8), 0);
	wr32(PDC_REG(0x15d6), 0);
	wr32(PDC_REG(0x15d9), 0x10108);
	wr32(PDC_REG(0x15da), 0x30010);
	wr32(PDC_REG(0x15db), 2);
	wr32(PDC_REG(0x15d9 + 4), 0x10108);
	wr32(PDC_REG(0x15da + 4), 0x30000);
	wr32(PDC_REG(0x15db + 4), 3);				/* not a618/a619/a650: 3 */
	wr32(PDC_REG(0x15d9 + 8), 0x10108);
	wr32(PDC_REG(0x15da + 8), 0x30080);
	wr32(PDC_REG(0x15db + 8), 3);

	wr32(PDC_REG(0x1148), 0);
	wr32(PDC_REG(0x1140), 0x80000001);
	dsb();
}

/* HFI queue table: header (6 dwords) + 3 queue headers (12 dwords); legacy ids 0, 4, 5 */
static void hfi_table(void)
{
	static const uint32_t ids[3] = { 0, 4, 5 };
	uintptr_t t = GMEM_HFI;
	wr32(t + 0, 0);
	wr32(t + 4, 6 * 4 + 3 * 12 * 4);
	wr32(t + 8, 6);
	wr32(t + 12, 12);
	wr32(t + 16, 3);
	wr32(t + 20, 3);
	for (int q = 0; q < 3; q++) {
		uintptr_t h = t + 24 + 48 * q;
		wr32(h + 0, 1);					/* status */
		wr32(h + 4, VA_HFI + 0x1000 * (q + 1));		/* iova */
		wr32(h + 8, (10u << 8) | ids[q]);		/* type */
		wr32(h + 12, 0x1000 >> 2);			/* size in dwords */
		wr32(h + 24, 1);				/* rx watermark */
		wr32(h + 28, 1);				/* tx watermark */
		wr32(h + 32, 1);				/* rx request */
		wr32(h + 36, 1);				/* tx request */
	}
}

/* RPMh levels (qcom-rpmpd.h) and bonito's GPU OPPs every speed bin has (0x0f). */
static const uint32_t gpu_khz[] = { 0, 180000, 267000, 355000, 430000 };
static const uint32_t gpu_lvl[] = { 0, 48, 64, 128, 192 };
#define N_GPU	5
static const uint32_t gmu_khz[] = { 0, 200000 };
static const uint32_t gmu_lvl[] = { 0, 48 };
#define N_GMU	2
static uint32_t gx_votes[N_GPU], cx_votes[N_GMU];

/* cmd-db aux data (u16 levels) for an id such as "gfx.lvl" */
static int cmddb_aux(const char *id, uintptr_t *aux, uint32_t *len)
{
	uintptr_t db = CMD_DB;
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uintptr_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (int r = 0; r < 8; r++) {
		uintptr_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uintptr_t ent = data + hoff + 24 * e;
			int match = 1;
			for (int i = 0; i < 8; i++) {
				char ch = (char)(rd32(ent + (i & ~3)) >> (8 * (i & 3)));
				if (ch != id[i]) {
					match = 0;
					break;
				}
				if (!id[i])
					break;
			}
			if (!match)
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t aux16(uintptr_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

/* a6xx_gmu_rpmh_arc_votes_init */
static int arc_votes(uint32_t *votes, const uint32_t *lvl, int n, const char *pri_id, const char *sec_id)
{
	uintptr_t pri, sec;
	uint32_t plen, slen;
	if (cmddb_aux(pri_id, &pri, &plen) || cmddb_aux(sec_id, &sec, &slen))
		return -1;
	plen /= 2;
	slen /= 2;
	for (int i = 0; i < n; i++) {
		uint32_t pindex = 0, sindex = 0, j;
		for (j = 0; j < plen; j++)
			if (aux16(pri, j) >= lvl[i]) {
				pindex = j;
				break;
			}
		if (j == plen)
			return -1;
		for (j = 0; j < slen; j++) {
			if (aux16(sec, j) >= lvl[i]) {
				sindex = j;
				break;
			} else if (aux16(sec, j)) {
				sindex = j;
			}
		}
		votes[i] = (aux16(pri, pindex) << 16) | (sindex << 8) | pindex;
	}
	return 0;
}

/* legacy OOB request: set bit, wait for ack bit, clear the ack */
static int oob(const char *name, int set, int ack)
{
	wr32(GMU_REG(GMU_H2G_SET), 1u << set);
	int rc = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << ack, 1, 100);
	wr32(GMU_REG(GMU_G2H_CLR), 1u << ack);
	con_printf("oob %s %s (info %x)\n", name, rc ? "TIMEOUT" : "acked", rd32(GMU_REG(GMU_G2H_INFO)));
	return rc;
}

static int gmu_boot(int *cb_out)
{
	int cb = gmu_smmu();
	if (cb < 0)
		return -1;
	*cb_out = cb;

	if (arc_votes(gx_votes, gpu_lvl, N_GPU, "gfx.lvl", "mx.lvl") ||
	    arc_votes(cx_votes, gmu_lvl, N_GMU, "cx.lvl", "mx.lvl")) {
		con_printf("cmd-db levels not found (db magic %x)\n", rd32(CMD_DB + 4));
		return -1;
	}
	con_printf("votes gx %x %x %x %x %x cx %x\n", gx_votes[0], gx_votes[1], gx_votes[2], gx_votes[3],
		   gx_votes[4], cx_votes[1]);

	con_printf("rscc status0 %x pdc enable %x\n", rd32(RSCC_REG(0x0004)), rd32(PDC_REG(0x1140)));
	rpmh_init();
	hfi_table();

	/* a6xx_gmu_fw_start */
	wr32(GMU_REG(GMU_GENERAL_7), 1);				/* TCM retention */
	/*
	 * No a6xx_rpmh_start here: Linux only wakes the RSC when resuming from
	 * its own a6xx_rpmh_stop (GMU_STATUS_PDC_SLEEP); a cold boot skips it.
	 * (Probe 1 on bonito: the wake request was never acked.)
	 */
	con_printf("rscc status0 %x seq busy %x\n", rd32(RSCC_REG(0x0004)), rd32(RSCC_REG(0x0101)));

	/* legacy firmware: raw ITCM image */
	uint32_t flen = (uint32_t)(gmu_fw_end - gmu_fw);
	if (flen > 0x8000)
		return -1;
	copy_in(GMU_REG(GMU_ITCM), gmu_fw, flen);
	dsb();

	wr32(GMU_REG(GMU_INIT_RESULT), 0);
	wr32(GMU_REG(GMU_BOOT_CFG), 2);
	wr32(GMU_REG(GMU_QTBL_ADDR), VA_HFI);
	wr32(GMU_REG(GMU_QTBL_INFO), 1);
	wr32(GMU_REG(GMU_FENCE0), (1u << 31) | (0xau << 18) | 0xa0);
	wr32(GMU_REG(GMU_CM3_CFG), 0x4052);
	wr32(GMU_REG(GMU_SFR_ADDR), 0x06015000);		/* GMU chip id layout of 615.0.0 */
	wr32(GMU_REG(GMU_CP_MSG), VA_LOG | (0x4000 / 0x1000 - 1));
	/* a6xx_gmu_power_config: idle level active (no SPTP/IFPC), RPMh client on */
	wr32(GMU_REG(GMU_SYS_BUS), 1);
	wr32(GMU_REG(GMU_ICACHE_CFG), 1);
	wr32(GMU_REG(GMU_DCACHE_CFG), 1);
	wr32(GMU_REG(GMU_PWRCOL_CTRL), 0x9c40400);
	wr32(GMU_REG(GMU_RPMH_CTRL), rd32(GMU_REG(GMU_RPMH_CTRL)) |
	     1 | (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11));

	/* a6xx_gmu_start */
	uint32_t v = rd32(GMU_REG(GMU_DTCM) + 0xff8 * 4);
	uint32_t mask = v <= 0x20010004 ? 0xffffffff : 0x1ff;
	uint32_t want = v <= 0x20010004 ? 0xbabeface : 0x100;
	wr32(GMU_REG(GMU_SYSRESET), 1);
	wr32(GMU_REG(GMU_CP_RESP), 0);
	wr32(GMU_REG(GMU_SYSRESET), 0);
	uint32_t r = 0;
	int ok = 0;
	for (int i = 0; i < 1000; i++) {
		r = rd32(GMU_REG(GMU_INIT_RESULT));
		if ((r & mask) == want) {
			ok = 1;
			break;
		}
		delay_us(100);
	}
	con_printf("gmu %s: init %x (dtcm %x) fw %x\n", ok ? "BOOTED" : "TIMEOUT", r, v,
		   rd32(GMU_REG(GMU_FW_VERSION)));
	if (!ok) {
		cb_fault("gmu", cb);
		return -1;
	}

	/* legacy: a6xx_gmu_gfx_rail_on + a6xx_sptprac_enable */
	wr32(GMU_REG(GMU_BOOT_SLUMBER), 0);
	wr32(GMU_REG(GMU_GX_VOTE_IDX), gx_votes[N_GPU - 1] & 0xff);
	wr32(GMU_REG(GMU_MX_VOTE_IDX), (gx_votes[N_GPU - 1] >> 8) & 0xff);
	if (oob("boot_slumber", 22, 30))
		return -1;
	wr32(GMU_REG(GMU_GX_SPTPRAC_PWR), 0x778000);
	int rc = -1;
	for (int i = 0; i < 100 && rc; i++) {	/* a6xx_sptprac_enable: (status & 0x38) == 0x28 */
		if ((rd32(GMU_REG(GMU_SPTPRAC_STATUS)) & 0x38) == 0x28)
			rc = 0;
		else
			delay_us(10);
	}
	con_printf("sptprac %x %s\n", rd32(GMU_REG(GMU_SPTPRAC_STATUS)), rc ? "NOT ON" : "on");

	/* a6xx_gmu_hfi_start */
	wr32(GMU_REG(GMU_HFI_CTRL_INIT), 1);
	rc = wait_bit(GMU_REG(GMU_HFI_CTRL_STATUS), 1, 1, 10);
	con_printf("hfi ctrl %x %s\n", rd32(GMU_REG(GMU_HFI_CTRL_STATUS)), rc ? "TIMEOUT" : "up");
	return rc;
}

/* ---- stage 3: HFI v1 ---- */

#define QHDR(q)		(GMEM_HFI + 24 + 48 * (q))
#define QDATA(q)	(GMEM_HFI + 0x1000 * ((q) + 1))
#define QSIZE		1024

static uint32_t hfi_seq;

static int hfi_send(uint32_t id, uint32_t *msg, uint32_t dwords)
{
	uint32_t seq = ++hfi_seq & 0xfff;
	msg[0] = (seq << 20) | (0u << 16) | (dwords << 8) | id;
	uintptr_t h = QHDR(0);
	uint32_t w = rd32(h + 44);
	for (uint32_t i = 0; i < dwords; i++) {
		wr32(QDATA(0) + 4 * w, msg[i]);
		w = (w + 1) % QSIZE;
	}
	dsb();
	wr32(h + 44, w);
	wr32(GMU_REG(GMU_H2G_SET), 1);

	for (int t = 0; t < 2000; t++) {
		if (rd32(GMU_REG(GMU_G2H_INFO)) & 1) {
			wr32(GMU_REG(GMU_G2H_CLR), 1);
			uintptr_t rh = QHDR(1);
			while (rd32(rh + 40) != rd32(rh + 44)) {
				uint32_t r = rd32(rh + 40), hdr = rd32(QDATA(1) + 4 * r);
				uint32_t n = (hdr >> 8) & 0xff, buf[24] = { 0 };
				for (uint32_t i = 0; i < n; i++) {
					if (i < 24)
						buf[i] = rd32(QDATA(1) + 4 * r);
					r = (r + 1) % QSIZE;
				}
				wr32(rh + 40, r);
				if ((hdr & 0xff) == 100) {
					con_printf("hfi: gmu error %x %x\n", buf[1], buf[2]);
					continue;
				}
				if ((buf[1] >> 20) == seq)
					return (int)buf[2];
			}
		}
		delay_ms(1);
	}
	con_printf("hfi: msg %u: no ack (info %x)\n", id, rd32(GMU_REG(GMU_G2H_INFO)));
	return -1;
}

static int hfi_start(void)
{
	static uint32_t msg[192];
	int rc;

	for (int i = 0; i < 192; i++)
		msg[i] = 0;
	msg[1] = 0;				/* seg_id */
	msg[2] = VA_DEBUG;
	msg[3] = 0x4000;
	msg[4] = 1;				/* boot_state: GMU_COLD_BOOT */
	rc = hfi_send(0, msg, 5);
	con_printf("3 hfi init -> %d\n", rc);
	if (rc)
		return -1;

	msg[1] = (1u << 28) | (1u << 19) | (1u << 17);
	rc = hfi_send(1, msg, 2);
	con_printf("hfi fw version -> %d\n", rc);

	/* perf table v1: header, num gpu, num gmu, gx[16] {vote, freq}, cx[4] {vote, freq} */
	for (int i = 0; i < 192; i++)
		msg[i] = 0;
	msg[1] = N_GPU;
	msg[2] = N_GMU;
	for (int i = 0; i < N_GPU; i++) {
		msg[3 + 2 * i] = gx_votes[i];
		msg[4 + 2 * i] = gpu_khz[i];
	}
	for (int i = 0; i < N_GMU; i++) {
		msg[3 + 32 + 2 * i] = cx_votes[i];
		msg[4 + 32 + 2 * i] = gmu_khz[i];
	}
	rc = hfi_send(4, msg, 3 + 32 + 8);
	con_printf("hfi perf table -> %d\n", rc);

	/* bw table: a6xx_build_bw_table (sdm845 family) */
	for (int i = 0; i < 192; i++)
		msg[i] = 0;
	msg[1] = 1;				/* bw levels */
	msg[2] = 3;				/* cnoc cmds */
	msg[3] = 3;				/* ddr cmds */
	msg[4] = 0x05;				/* cnoc wait bitmask */
	msg[5] = 0x07;				/* ddr wait bitmask */
	msg[6] = 0x50034;			/* cnoc addrs [6] at 6..11 */
	msg[7] = 0x5007c;
	msg[8] = 0x5004c;
	msg[12] = 0x40000000;			/* cnoc data [2][6] at 12..23 */
	msg[13] = 0x00000000;
	msg[14] = 0x40000000;
	msg[18] = 0x60000001;
	msg[19] = 0x20000001;
	msg[20] = 0x60000001;
	msg[24] = 0x50000;			/* ddr addrs [8] at 24..31 */
	msg[25] = 0x5005c;
	msg[26] = 0x5000c;
	msg[32] = 0x40000000;			/* ddr data [16][8] at 32.. */
	msg[33] = 0x40000000;
	msg[34] = 0x40000000;
	rc = hfi_send(3, msg, 32 + 128);
	con_printf("hfi bw table -> %d\n", rc);

	rc = hfi_send(5, msg, 1);		/* TEST: no more messages until next boot */
	con_printf("hfi test -> %d\n", rc);
	return 0;
}

/* ---- stage 4: zap shader through TrustZone PAS ---- */

#define PAS_GPU		13
#define ZAP_REGION	0x97a15000UL	/* reserved-memory gpu@97a15000 */
#define ZAP_SIZE	0x2000UL
#define SCM_PIL(cmd)	(0x42000000UL | (2u << 8) | (cmd))

static int zap_load(void)
{
	uint64_t r1 = 0;
	int64_t rc = (int64_t)smc5(SCM_PIL(7), 1, PAS_GPU, 0, 0, &r1);		/* IS_SUPPORTED */
	con_printf("4 zap: pas supported rc %ld r1 %lx\n", rc, r1);

	uint32_t mlen = (uint32_t)(zap_mdt_end - zap_mdt);
	copy_in(ZAP_META, zap_mdt, mlen);
	dsb();
	rc = (int64_t)smc5(SCM_PIL(1), 2 | (2u << 6), PAS_GPU, ZAP_META, 0, &r1);	/* INIT_IMAGE */
	con_printf("zap: init image rc %ld r1 %lx\n", rc, r1);
	if (rc || r1)
		return -1;
	rc = (int64_t)smc5(SCM_PIL(2), 3, PAS_GPU, ZAP_REGION, ZAP_SIZE, &r1);	/* MEM_SETUP */
	con_printf("zap: mem setup rc %ld r1 %lx\n", rc, r1);
	if (rc || r1)
		return -1;
	/* relocatable: the one LOAD segment (paddr 0x5000) at the region base */
	zero32(ZAP_REGION, ZAP_SIZE);
	copy_in(ZAP_REGION, zap_b02, (uint32_t)(zap_b02_end - zap_b02));
	dsb();
	rc = (int64_t)smc5(SCM_PIL(5), 1, PAS_GPU, 0, 0, &r1);			/* AUTH_AND_RESET */
	con_printf("zap: auth and reset rc %ld r1 %lx\n", rc, r1);
	return (rc || r1) ? -1 : 0;
}

/* ---- stage 5: SQE, ringbuffer, first commands ---- */

static uint32_t wptr;

static uint32_t pm4_parity(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^
				   (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static void ring(uint32_t v)
{
	wr32(G_RING + 4 * wptr, v);
	wptr = (wptr + 1) % RING_DW;
}

static void pkt7(uint32_t op, uint32_t cnt)
{
	ring(0x70000000 | cnt | (pm4_parity(cnt) << 15) | ((op & 0x7f) << 16) | (pm4_parity(op) << 23));
}

static int flush_wait(const char *what)
{
	dsb();
	wr32(GREG(0x807), wptr);			/* CP_RB_WPTR */
	int i;
	for (i = 0; i < 1000; i++) {
		if (rd32(GREG(0x806)) == wptr && !(rd32(GREG(0x210)) & (1u << 23)))
			break;
		delay_ms(1);
	}
	con_printf("%s: rptr %x wptr %x st %x hwf %x %s\n", what, rd32(GREG(0x806)), wptr, rd32(GREG(0x210)),
		   rd32(GREG(0x821)), i < 1000 ? "idle" : "TIMEOUT");
	return i < 1000 ? 0 : -1;
}

/*
 * Visible proof: the GPU paints a band into the panel framebuffer (one
 * gradient row by CP_MEM_WRITE, then CP_MEMCPY copies it down).
 */
#define FB_PA		0x9c000000UL
#define BAND_Y		1500
#define BAND_H		400

static void gpu_paint(int cb)
{
	for (int i = 0; i < 5; i++)	/* 10 MB as 2 MB blocks, AP=01, NC */
		wr64(GT_L2 + 8 * (((GVA_FB >> 21) & 0x1ff) + i),
		     (FB_PA + 0x200000UL * i) | (1u << 10) | (3u << 8) | (1u << 6) | 1u);
	uintptr_t c = SMMU_CB(cb);
	wr32(c + 0x618, 0);		/* CB TLBIALL */
	wr32(c + 0x7f0, 0);		/* CB TLBSYNC */
	wait_bit(c + 0x7f4, 1, 0, 10);

	uint32_t row = GVA_FB + BAND_Y * FB_STRIDE;
	pkt7(0x3d, 2 + FB_W);		/* CP_MEM_WRITE one row */
	ring(row);
	ring(0);
	for (uint32_t x = 0; x < FB_W; x++) {
		uint32_t r = x * 255 / (FB_W - 1), b = 255 - r, g = (x / 60) % 2 ? 200 : 40;
		ring(0xff000000 | (r << 16) | (g << 8) | b);
	}
	pkt7(0x26, 0);
	flush_wait("paint row");
	for (uint32_t y = 1; y < BAND_H; y++) {
		pkt7(0x75, 5);		/* CP_MEMCPY dwords, src, dst */
		ring(FB_W);
		ring(row);
		ring(0);
		ring(row + y * FB_STRIDE);
		ring(0);
		if ((y % 100) == 0 && flush_wait("paint copy"))
			return;
	}
	pkt7(0x26, 0);
	flush_wait("paint done");
}


/* ---- stage 6: display pipes (read-only) and the 2D engine ---- */

#define MDP_TOP		0x0ae01000UL	/* dpu "mdp" block; offsets from the sdm845/sdm670 catalog */
static void mdp_dump(void)
{
	static const struct { char n[5]; uint32_t off; } p[] = {
		{ "vig0", 0x4000 }, { "vig1", 0x6000 }, { "dma0", 0x24000 }, { "dma1", 0x26000 }, { "dma2", 0x28000 },
	};
	for (unsigned i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		uintptr_t b = MDP_TOP + p[i].off;
		con_printf("6 %s size %x out %x xy %x addr %x stride %x fmt %x\n", p[i].n, rd32(b + 0x00),
			   rd32(b + 0x0c), rd32(b + 0x10), rd32(b + 0x14), rd32(b + 0x24), rd32(b + 0x30));
	}
	for (int c = 0; c < 4; c++) {
		uintptr_t b = MDP_TOP + 0x1000 + 0x200UL * c;
		con_printf("ctl%d layer0 %x top %x flush %x\n", c, rd32(b + 0x00), rd32(b + 0x14), rd32(b + 0x18));
	}
}

/* turnip's r2d blit (ios-flame a6_blit_ib, captured on the A640) with the A615 (a6xx gen1) magic
 * registers from Mesa freedreno_devices.py, one CCU, and blit_wfi_quirk. */
static const uint32_t blit_init[][2] = {
	{ 0x9804, 0x1f }, { 0x9805, 0 }, { 0xa0f8, 0 }, { 0xb600, 0x00108000 }, { 0x8600, 0x880 },
	{ 0xae03, 0x430 }, { 0xae00, 0 }, { 0xbe04, 0x80000 }, { 0x9600, 0 }, { 0x0e12, 1 },
	{ 0x8e01, 1 }, { 0x8e04, 0x04100000 },
	{ 0xae04, 0 }, { 0xae0f, 0x3f }, { 0xb605, 0x44 },
	{ 0xbe00, 0x80 }, { 0xbe01, 0 }, { 0xab20, 0 }, { 0xb182, 0 }, { 0xbb11, 0 }, { 0xa9a8, 0 },
	{ 0xab00, 5 }, { 0xa009, 3 }, { 0x8811, 0x10 }, { 0x8110, 0 }, { 0x8818, 0 }, { 0x8819, 0 },
	{ 0x881a, 0 }, { 0x881b, 0 }, { 0x881c, 0 }, { 0x881d, 0 }, { 0x881e, 0 }, { 0x88f0, 0 },
	{ 0x9236, 0 }, { 0x9300, 0 }, { 0x9306, 1 }, { 0xb183, 0 }, { 0x80af, 0 }, { 0x8099, 0 },
	{ 0x980a, 0 }, { 0x9210, 0 }, { 0x9211, 0 }, { 0x9602, 0 }, { 0x9e72, 0 }, { 0xb309, 0xa2 },
	{ 0xb986, 0xfc }, { 0xa007, 0 }, { 0x8864, 0 }, { 0xb800, 0x102 },
	{ 0x8e07, 0x08000000 },		/* RB_CCU_CNTL: sysmem, color offset for 1 CCU */
};

static uint32_t pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000 | cnt | (pm4_parity(cnt) << 7) | ((reg & 0x3ffff) << 8) | (pm4_parity(reg) << 27);
}

#define G_SCRATCH	(GMEM + 0x228000)	/* border colours + event timestamps */
#define GVA_SCRATCH	(GVA_RING + 0x18000)
#define G_SRC		0x171000000UL		/* 1125x2436 test image, 4608-byte rows (11.2 MB) */
#define GVA_SRC		0x40000000u
#define SRC_W		1125
#define SRC_H		2436
#define SRC_PITCH	4608
#define DST_PITCH	4352

static void gpu_blit_test(int cb, int full)
{
	/* the source: CPU-drawn pattern (checker + border), mapped with 2 MB blocks */
	for (uint32_t y = full ? SRC_H : 0; y < SRC_H; y++)
		for (uint32_t x = 0; x < SRC_W; x++) {
			uint32_t c = ((x / 75 + y / 75) & 1) ? 0xff2040ff : 0xffffd020;
			if (x < 20 || y < 20 || x >= SRC_W - 20 || y >= SRC_H - 20)
				c = 0xff00ff00;
			wr32(G_SRC + y * SRC_PITCH + 4 * x, c);
		}
	uintptr_t l2b = GMEM + 0x103000;		/* L2 for VA 1-2 GB */
	zero32(l2b, 0x1000);
	wr64(GT_L1 + 8, l2b | 3);
	for (int i = 0; i < 6; i++)
		wr64(l2b + 8 * i, (G_SRC + 0x200000UL * i) | (1u << 10) | (3u << 8) | (1u << 6) | 1u);
	map_pages(GT_L3, GVA_SCRATCH, G_SCRATCH, 0x1000, 0);
	zero32(G_SCRATCH, 0x1000);
	uintptr_t c = SMMU_CB(cb);
	wr32(c + 0x618, 0);
	wr32(c + 0x7f0, 0);
	wait_bit(c + 0x7f4, 1, 0, 10);

	/* destination, pitch 4352: a 600-row band below the log, or the whole panel (fit) */
	uint32_t dh = 600, dw = SRC_W * dh / SRC_H, dx = 780, dy = 1500;
	if (full) {
		dh = FB_H;
		dw = (SRC_W * FB_H + SRC_H / 2) / SRC_H;
		dx = (FB_W - dw) / 2;
		dy = 0;
	}
	uint64_t border = GVA_SCRATCH + 0x800, ts = GVA_SCRATCH + 0x900;
#define EV(e)		do { pkt7(0x46, 1); ring(e); } while (0)
#define EV_TS(e)	do { pkt7(0x46, 4); ring(e); ring((uint32_t)ts); ring(0); ring(0); } while (0)
#define REG(r, v)	do { ring(pkt4((r), 1)); ring(v); } while (0)
	EV(0x31);
	REG(0xbb08, 0xfffff);
	pkt7(0x26, 0);
	for (unsigned i = 0; i < sizeof(blit_init) / sizeof(blit_init[0]); i++)
		REG(blit_init[i][0], blit_init[i][1]);
	ring(pkt4(0xb302, 2)); ring((uint32_t)border); ring(0);
	ring(pkt4(0xb180, 2)); ring((uint32_t)border); ring(0);
	EV_TS(0x1d);
	EV(0x19);
	EV(0x31);
	pkt7(0x26, 0);
	if (flush_wait("blit init"))
		return;
	REG(0x840a, 0);			/* GRAS_A2D_SCISSOR_TL: no clip (reset value clips on the A615) */
	REG(0x840b, 0x3fff | (0x3fffu << 16));
	REG(0x80f0, 0);			/* GRAS_SC_WINDOW_SCISSOR_TL/BR */
	REG(0x80f1, 0x3fff | (0x3fffu << 16));
	REG(0x8c01, 0);
	REG(0x8c00, 0x00f03000);
	REG(0x8400, 0x00f03000);
	REG(0xacc0, 0xf180);
	ring(pkt4(0x8405, 2)); ring(dx | (dy << 16)); ring((dx + dw - 1) | ((dy + dh - 1) << 16));
	ring(pkt4(0x8401, 4)); ring(0); ring((SRC_W - 1) << 8); ring(0); ring((SRC_H - 1) << 8);
	ring(pkt4(0xb4c0, 5)); ring(0x00500430 | 0x10000); ring(SRC_W | (SRC_H << 15));
	ring(GVA_SRC); ring(0); ring(SRC_PITCH << 3);
	ring(pkt4(0xb4ca, 3)); ring(GVA_SRC); ring(0); ring(0);
	ring(pkt4(0x8c17, 4)); ring(0x430); ring(GVA_FB); ring(0); ring(DST_PITCH >> 6);
	ring(pkt4(0x8c20, 3)); ring(GVA_FB); ring(0); ring(0);
	pkt7(0x26, 0);			/* blit_wfi_quirk */
	pkt7(0x2c, 1); ring(3);		/* CP_BLIT, BLIT_OP_SCALE */
	pkt7(0x26, 0);
	EV_TS(0x1d);
	EV_TS(0x1c);
	EV_TS(0x04);
	flush_wait("blit");
#undef EV
#undef EV_TS
#undef REG
	uint32_t mid = rd32(FB_PA + (dy + dh / 2) * DST_PITCH + 4 * (dx + dw / 2));
	con_printf("blit %ux%u at %u,%u: fb mid %x\n", dw, dh, dx, dy, mid);
}

static void gpu_run(int gmu_cb)
{
	int smr = free_smr(), cb = free_cb(gmu_cb);
	con_printf("5 gpu smmu: smr %d cb %d\n", smr, cb);
	if (smr < 0 || cb < 0)
		return;
	zero32(GT_L1, 0x3000);
	zero32(G_SQE, 0x21000);
	wr64(GT_L1 + 0, GT_L2 | 3);
	wr64(GT_L2 + 8 * ((GVA_SQE >> 21) & 0x1ff), GT_L3 | 3);
	map_pages(GT_L3, GVA_SQE, G_SQE, 0x10000, 0);
	map_pages(GT_L3, GVA_RING, G_RING, 0x8000, 0);
	map_pages(GT_L3, GVA_TARGET, G_TARGET, 0x1000, 0);
	wr32(G_TARGET, 0x11111111);
	cb_setup(cb, GT_L1, smr, GPU_SID);

	/* GPU_SET OOB (legacy bits): the GMU powers the GX side */
	oob("gpu_set", 16, 24);
	con_printf("gx gdsc %x rpmh %x\n", rd32(GPUCC + GPUCC_GX_GDSCR), rd32(GMU_REG(GMU_RPMH_STATE)));
	if (!(rd32(GPUCC + GPUCC_GX_GDSCR) & (1u << 31))) {
		con_printf("GX not powered: stop\n");
		return;
	}
	con_printf("rbbm_status %x\n", rd32(GREG(0x210)));
	wr32(GREG(0x883), 0x41445245);
	con_printf("cp_scratch0 %x (want 41445245)\n", rd32(GREG(0x883)));

	/* hw_init subset (a6xx_gpu.c), A615: GBIF, GMEM 512K */
	wr32(GREG(0x3c45), 0);			/* GBIF_HALT */
	(void)rd32(GREG(0x3c45));
	wr32(GREG(0x16), 0);			/* RBBM_GBIF_HALT */
	(void)rd32(GREG(0x16));
	wr32(GREG(0xf803), 0);			/* RBBM_SECVID_TSB_CNTL */
	wr32(GREG(0xf800), 0);
	wr32(GREG(0xf801), 0);
	wr32(GREG(0xf802), 0);
	static const uint16_t addr_mode[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05,
					      0xa601, 0x9601, 0x0e00, 0xae01, 0xb601, 0xf810 };
	for (unsigned i = 0; i < sizeof(addr_mode) / sizeof(addr_mode[0]); i++)
		wr32(GREG(addr_mode[i]), 1);
	wr32(GREG(0x11), 3);			/* RBBM_GBIF_CLIENT_QOS_CNTL */
	wr32(GREG(0x50b), 0xffffffff);
	uint64_t trap = 0x1fffffffff000ull;
	wr32(GREG(0xe05), (uint32_t)(trap + 0xfc0));
	wr32(GREG(0xe06), (uint32_t)((trap + 0xfc0) >> 32));
	wr32(GREG(0xe09), (uint32_t)trap);
	wr32(GREG(0xe0a), (uint32_t)(trap >> 32));
	wr32(GREG(0xe07), (uint32_t)trap);
	wr32(GREG(0xe08), (uint32_t)(trap >> 32));
	wr32(GREG(0xe0b), 0x100000);		/* UCHE GMEM range: 512K at 1M */
	wr32(GREG(0xe0c), 0);
	wr32(GREG(0xe0d), 0x100000 + 0x80000 - 1);
	wr32(GREG(0xe0e), 0);
	wr32(GREG(0xe18), 0x804);
	wr32(GREG(0xe17), 0x4);
	wr32(GREG(0x8c2), 0x010000c0);		/* CP_ROQ_THRESHOLDS_2 (not a640) */
	wr32(GREG(0x8c1), 0x8040362c);
	wr32(GREG(0x8c3), 128);
	wr32(GREG(0x98d), 1);			/* CP_AHB_CNTL */
	wr32(GREG(0x500), 1);
	wr32(GREG(0x1f), (1u << 30) | 0x1fffff);
	wr32(GREG(0xe19), (1u << 7) | 1);

	/* SQE microcode: the file minus its first dword */
	uint32_t sqe_len = (uint32_t)(sqe_fw_end - sqe_fw) - 4;
	copy_in(G_SQE, sqe_fw + 4, sqe_len);
	wr32(GREG(0x830), GVA_SQE);		/* CP_SQE_INSTR_BASE */
	wr32(GREG(0x831), 0);
	wr32(GREG(0x800), GVA_RING);
	wr32(GREG(0x801), 0);
	wr32(GREG(0x802), 0x0800020c);		/* 32K ring, 32-byte blocks, no rptr update */
	wptr = 0;
	wr32(GREG(0x808), 1);			/* CP_SQE_CNTL: start */
	con_printf("cp started (sqe %u bytes) status %x\n", sqe_len, rd32(GREG(0x210)));

	pkt7(0x48, 8);				/* CP_ME_INIT */
	ring(0x2f);
	ring(3);
	ring(0x20000000);
	for (int i = 0; i < 5; i++)
		ring(0);
	if (flush_wait("me_init"))
		goto out;

	if (zap_load()) {
		wr32(GREG(0xf400), 0);		/* RBBM_SECVID_TRUST_CNTL, as Linux without zap */
		con_printf("no zap: trust cntl %x\n", rd32(GREG(0xf400)));
	} else {
		pkt7(0x66, 1);			/* CP_SET_SECURE_MODE 0 */
		ring(0);
		if (flush_wait("secure off"))
			goto out;
	}

	pkt7(0x3d, 3);				/* CP_MEM_WRITE */
	ring(GVA_TARGET);
	ring(0);
	ring(0xcafef00d);
	pkt7(0x26, 0);				/* CP_WAIT_FOR_IDLE */
	flush_wait("mem write");
	for (int i = 0; i < 100 && rd32(G_TARGET) != 0xcafef00d; i++)
		delay_ms(1);
	con_printf("target %x %s\n", rd32(G_TARGET),
		   rd32(G_TARGET) == 0xcafef00d ? "*** GPU RAN OUR COMMANDS ***" : "(unchanged)");
	if (rd32(G_TARGET) == 0xcafef00d) {
		gpu_paint(cb);
		mdp_dump();
		gpu_blit_test(cb, 0);
		/* the panel pipe (VIG0, CTL0) reads 4352-byte rows from now on; ABL's autorefresh picks it up */
		wr32(MDP_TOP + 0x4000 + 0x24, DST_PITCH);
		wr32(MDP_TOP + 0x1000 + 0x18, 1);
		delay_ms(50);
		gpu_blit_test(cb, 1);
		con_printf("stride %u (want %u), ctl0 flush %x\n", rd32(MDP_TOP + 0x4000 + 0x24), DST_PITCH,
			   rd32(MDP_TOP + 0x1000 + 0x18));
	}
out:
	cb_fault("gpu", cb);
}

/* ---- entry points ---- */

void test_gpu(void)
{
	con_scale(2);
	con_printf("GPU bring-up: Adreno 615 (bonito)\n");
	if (cx_up())
		return;
	int gmu_cb = -1;
	if (gmu_boot(&gmu_cb))
		return;
	if (hfi_start())
		return;
	gpu_run(gmu_cb);
	con_printf("GPU: end\n");
}
