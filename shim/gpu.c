/*
 * GPU phase 1a: read-only look at the Adreno 615 power/clock state that ABL
 * leaves behind on bonito (SDM670). Nothing is written.
 *
 * Offsets from mainline Linux: drivers/clk/qcom/gcc-sdm845.c (GPU branch
 * clocks in GCC) and gpucc-sdm845.c (GPU clock controller at 0x5090000,
 * which bonito's DT lists as "qcom,sdm845-gpucc").
 *
 * GPUCC registers are only touched when GCC's gpu_cfg_ahb clock (the GPUCC
 * register interface clock) reports running; GPU core / GMU registers are
 * not read at all here (they need the CX/GX power domains, which is what
 * this probe is meant to find out).
 */
#include <stdint.h>
#include "console.h"
#include "gpu.h"

#define GCC_BASE		0x00100000UL
#define GCC_GPU_CFG_AHB_CBCR	0x71004
#define GCC_GPU_MEMNOC_GFX_CBCR	0x7100c
#define GCC_GPU_SNOC_DVM_CBCR	0x71018

#define GPUCC_BASE		0x05090000UL
#define GPUCC_PLL1_MODE		0x100
#define GPUCC_GX_GDSCR		0x100c
#define GPUCC_CX_GDSCR		0x106c
#define GPUCC_CX_GMU_CBCR	0x1098
#define GPUCC_CXO_CBCR		0x109c
#define GPUCC_GMU_CMD_RCGR	0x1120	/* CFG at +4 */

#define CBCR_CLK_OFF		(1u << 31)
#define GDSC_PWR_ON		(1u << 31)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

void test_gpu(void)
{
	con_str("GPU", "phase 1a: Adreno 615 handoff state (read-only)");

	uint32_t ahb = rd(GCC_BASE + GCC_GPU_CFG_AHB_CBCR);
	con_hex2("GCC gpu", "cfg_ahb=", ahb, "memnoc=", rd(GCC_BASE + GCC_GPU_MEMNOC_GFX_CBCR));
	con_hex("GCC snoc_dvm", rd(GCC_BASE + GCC_GPU_SNOC_DVM_CBCR));

	if (ahb & CBCR_CLK_OFF) {
		con_str("GPUCC", "cfg_ahb clock off: GPUCC not read");
		return;
	}
	uint32_t cx = rd(GPUCC_BASE + GPUCC_CX_GDSCR);
	uint32_t gx = rd(GPUCC_BASE + GPUCC_GX_GDSCR);
	con_hex2("GDSC", "cx=", cx, "gx=", gx);
	con_hex2("GPUCC", "cx_gmu=", rd(GPUCC_BASE + GPUCC_CX_GMU_CBCR), "cxo=", rd(GPUCC_BASE + GPUCC_CXO_CBCR));
	con_hex2("GMU RCG", "cmd=", rd(GPUCC_BASE + GPUCC_GMU_CMD_RCGR), "cfg=", rd(GPUCC_BASE + GPUCC_GMU_CMD_RCGR + 4));
	con_hex("PLL1 mode", rd(GPUCC_BASE + GPUCC_PLL1_MODE));
	con_str("GPU", (cx & GDSC_PWR_ON) ? "CX power domain ON at handoff" : "CX power domain OFF at handoff");
	con_str("GPU", (gx & GDSC_PWR_ON) ? "GX power domain ON at handoff" : "GX power domain OFF at handoff");
}
