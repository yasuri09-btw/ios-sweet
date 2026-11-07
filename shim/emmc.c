/*
 * bonito milestone: can we talk to the internal eMMC that ABL booted from?
 *
 * Read-only probe of sdhc_1 (Qualcomm sdhci-msm v5 at 0x7c4000). Like the UFS
 * probe it reuses what the bootloader left running: clocks, bus width, timing
 * and the selected card. Nothing is reset or reconfigured beyond the per-line
 * CMD/DAT software resets after an error, and the only commands sent are
 *   CMD13 SEND_STATUS, CMD8 SEND_EXT_CSD, CMD17/CMD18 READ (+ auto CMD12).
 * No write, erase or CMD6 SWITCH is ever issued from this file.
 *
 * Runs with the MMU off; data moves by PIO through the buffer data port, so
 * there is no DMA and no cache maintenance.
 *
 * Register layout: SD Host Controller Simplified Specification (SDHCI) and
 * JEDEC eMMC (JESD84). Qualcomm specifics (vendor register block inside the hc
 * window on v5, GCC branch clocks) were taken from the mainline Linux
 * sdhci-msm and gcc-sdm845 drivers as a hardware reference.
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"

#define HC_BASE		0x007c4000UL	/* sdhc_1 "hc" */
#define CQ_BASE		0x007c5000UL	/* sdhc_1 "cqhci" */
#define GCC_BASE	0x00100000UL
#define GCC_SDCC1_APPS_CBCR	0x26004
#define GCC_SDCC1_AHB_CBCR	0x26008
#define CBCR_CLK_OFF	(1u << 31)

/* SDHCI standard registers */
#define SD_BLKSIZE	0x04	/* 16 */
#define SD_BLKCNT	0x06	/* 16 */
#define SD_ARG		0x08
#define SD_XFER_CMD	0x0c	/* transfer mode (16) | command (16), written as one */
#define SD_RESP0	0x10
#define SD_DATA		0x20
#define SD_PRESENT	0x24
#define SD_HOSTCTL	0x28	/* host ctl 1 (8) | power (8) | gap (8) | wakeup (8) */
#define SD_CLKCTL	0x2c	/* clock (16) | timeout (8) | sw reset (8) */
#define SD_TIMEOUT	0x2e	/* 8 */
#define SD_RESET	0x2f	/* 8 */
#define SD_INTSTS	0x30	/* normal (16) | error (16) */
#define SD_INTSTS_EN	0x34
#define SD_INTSIG_EN	0x38
#define SD_HOSTCTL2	0x3c	/* auto cmd err (16) | host ctl 2 (16) */
#define SD_CAPS		0x40
#define SD_CAPS1	0x44
#define SD_VERSION	0xfc	/* slot int (16) | host version (16) */
/* Qualcomm v5 vendor registers inside the hc window */
#define MSM_MCI_VERSION	0x318

#define PS_CMD_INHIBIT	(1u << 0)
#define PS_DAT_INHIBIT	(1u << 1)
#define PS_BUF_RD_EN	(1u << 11)

#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_BUF_RD	(1u << 5)
#define INT_ERROR	(1u << 15)

#define RST_CMD		0x02
#define RST_DAT		0x04

/* command register flags (bits 31:16 of SD_XFER_CMD) */
#define C_RESP_NONE	0x00
#define C_RESP_136	0x01
#define C_RESP_48	0x02
#define C_RESP_48B	0x03
#define C_CRC		0x08
#define C_IDX		0x10
#define C_DATA		0x20
#define CMD(idx, f)	((uint32_t)((idx) << 8 | (f)) << 16)
/* transfer mode flags (bits 15:0) */
#define TM_BLKCNT	(1u << 1)
#define TM_ACMD12	(1u << 2)
#define TM_READ		(1u << 4)
#define TM_MULTI	(1u << 5)

#define R1		(C_RESP_48 | C_CRC | C_IDX)
#define SECTOR		512u

#define E_CMD_TIMEOUT	0x10000	/* our own codes; low 16 bits = SDHCI error status */
#define E_DATA_TIMEOUT	0x20000
#define E_INHIBIT	0x30000

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint8_t rd8(uintptr_t a) { return *(volatile uint8_t *)a; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline void wr16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
#define HC(off)		(HC_BASE + (off))

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static uint64_t ms_ticks(unsigned ms)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f / 1000 * ms;
}

static uint32_t rca;
/* raw state at the last failed command, for the screen */
static uint32_t last_sts, last_present, last_acmd;
static uint8_t buf[64 * SECTOR] __attribute__((aligned(64)));

static void line_reset(uint8_t mask)
{
	wr8(HC(SD_RESET), mask);
	uint64_t end = ticks() + ms_ticks(100);
	while ((rd8(HC(SD_RESET)) & mask) && ticks() < end)
		;
}

/* Wait for any of want (or the error bit) in the normal interrupt status. */
static uint32_t wait_int(uint32_t want, unsigned ms)
{
	uint64_t end = ticks() + ms_ticks(ms);
	for (;;) {
		uint32_t s = rd(HC(SD_INTSTS));
		if (s & (want | INT_ERROR))
			return s;
		if (ticks() > end)
			return 0;
	}
}

/*
 * Data-phase wait: done when the status has want (or an error), or the present
 * state has all of ps_set, or none of ps_clear. Returns the status (with want
 * or'ed in when the present state decided), 0 on timeout.
 */
static uint32_t wait_data(uint32_t want, uint32_t ps_set, uint32_t ps_clear, unsigned ms)
{
	uint64_t end = ticks() + ms_ticks(ms);
	for (;;) {
		uint32_t s = rd(HC(SD_INTSTS));
		if (s & (want | INT_ERROR))
			return s;
		uint32_t ps = rd(HC(SD_PRESENT));
		if ((ps_set && (ps & ps_set) == ps_set) || (ps_clear && !(ps & ps_clear)))
			return s | want;
		if (ticks() > end)
			return 0;
	}
}

/*
 * One command. For reads, nblk sectors are moved into out by PIO.
 * Returns 0, or E_* | SDHCI error status bits.
 */
static uint32_t cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t nblk, uint32_t tm,
		    uint32_t *resp, uint8_t *out)
{
	uint32_t inhibit = PS_CMD_INHIBIT | (nblk || (flags & 3) == C_RESP_48B ? PS_DAT_INHIBIT : 0);
	uint64_t end = ticks() + ms_ticks(500);
	while (rd(HC(SD_PRESENT)) & inhibit)
		if (ticks() > end)
			return E_INHIBIT;

	wr(HC(SD_INTSTS), 0xffffffff);			/* clear everything (W1C) */
	if (nblk) {
		wr16(HC(SD_BLKSIZE), SECTOR);
		wr16(HC(SD_BLKCNT), (uint16_t)nblk);
	}
	wr(HC(SD_ARG), arg);
	wr(HC(SD_XFER_CMD), CMD(idx, flags | (nblk ? C_DATA : 0)) | tm);

	uint32_t s = wait_int(INT_CMD_DONE, 100);
	if (!s || (s & INT_ERROR)) {
		last_sts = rd(HC(SD_INTSTS));
		last_present = rd(HC(SD_PRESENT));
		last_acmd = rd(HC(SD_HOSTCTL2));
		uint32_t err = s ? (s >> 16) : E_CMD_TIMEOUT;
		wr(HC(SD_INTSTS), 0xffffffff);
		line_reset(RST_CMD | (nblk ? RST_DAT : 0));
		return err ? err : E_CMD_TIMEOUT;
	}
	wr(HC(SD_INTSTS), INT_CMD_DONE);
	if (resp)
		*resp = rd(HC(SD_RESP0));

	/*
	 * Data phase. Besides the interrupt status bits, poll the present state
	 * (buffer read enable, DAT inhibit): on bonito the card's data reaches
	 * the buffer (PS bit 11) without Buffer Read Ready ever latching.
	 */
	for (uint32_t b = 0; b < nblk; b++) {
		s = wait_data(INT_BUF_RD, PS_BUF_RD_EN, 0, 1000);
		if (!s || (s & INT_ERROR))
			goto data_err;
		wr(HC(SD_INTSTS), INT_BUF_RD);
		uint32_t *w = (uint32_t *)(out + b * SECTOR);
		for (unsigned i = 0; i < SECTOR / 4; i++)
			w[i] = rd(HC(SD_DATA));
	}
	if (nblk || (flags & 3) == C_RESP_48B) {
		s = wait_data(INT_XFER_DONE, 0, PS_DAT_INHIBIT, 1000);
		if (!s || (s & INT_ERROR))
			goto data_err;
		wr(HC(SD_INTSTS), INT_XFER_DONE);
	}
	return 0;

data_err:
	last_sts = rd(HC(SD_INTSTS));
	last_present = rd(HC(SD_PRESENT));
	{
		uint32_t err = s ? (s >> 16) : E_DATA_TIMEOUT;
		wr(HC(SD_INTSTS), 0xffffffff);
		line_reset(RST_CMD | RST_DAT);
		return err ? err : E_DATA_TIMEOUT;
	}
}

/* ---- full re-initialization (ABL hands over a host at power-on defaults) ---- */

#define MSM_PWRCTL_STATUS	0x240
#define MSM_PWRCTL_MASK		0x244
#define MSM_PWRCTL_CLEAR	0x248
#define MSM_PWRCTL_CTL		0x24c
#define MSM_VENDOR_SPEC		0x20c
#define PWR_BUS_ON		(1u << 1)
#define PWR_IO_LOW		(1u << 2)
#define PWR_IO_HIGH		(1u << 3)
#define ACK_BUS_OK		(1u << 0)
#define ACK_IO_OK		(1u << 2)
#define VS_IO_PAD_PWR_SWITCH	(1u << 16)
#define VS_POR_VAL		0xa9c
#define VS_MCLK_SEL_MASK	(3u << 8)
#define VS_MCLK_SEL_DFLT	(2u << 8)
#define VS_SELECT_IN_EN		(1u << 18)
#define VS_SELECT_IN_MASK	(7u << 19)
#define MSM_VENDOR_SPEC3	0x250
#define VS3_PWRSAVE_DLL		(1u << 3)
#define MSM_DLL_CONFIG		0x200
#define MSM_DDR_200_CFG		0x224
#define GCC_SDCC1_APPS_RCGR	0x26028		/* CMD_RCGR; CFG +4, M +8, N +0xc */

/*
 * A power-control or signalling-voltage write on sdhci-msm raises a request
 * in PWRCTL_STATUS that software must clear and acknowledge. The regulators
 * are left on by ABL, so acknowledging success is all there is to do.
 */
static uint32_t pwr_handshake(void)
{
	uint64_t end = ticks() + ms_ticks(50);
	uint32_t st;
	while (!(st = rd(HC(MSM_PWRCTL_STATUS)) & 0xf))
		if (ticks() > end)
			return 0;
	for (int i = 0; i < 10 && (rd(HC(MSM_PWRCTL_STATUS)) & st); i++)
		wr(HC(MSM_PWRCTL_CLEAR), st);
	uint32_t ack = 0;
	if (st & 0x3)
		ack |= ACK_BUS_OK;
	if (st & (PWR_IO_LOW | PWR_IO_HIGH))
		ack |= ACK_IO_OK;
	wr(HC(MSM_PWRCTL_CTL), ack);
	return st;
}

/* Card clock = base / (2 * div), 10-bit divider; div 0 = base. */
static int set_clock(uint32_t div)
{
	uint16_t c = (uint16_t)rd(HC(SD_CLKCTL)) & ~0x4;	/* card clock off */
	wr16(HC(SD_CLKCTL), c);
	c = (uint16_t)((div & 0xff) << 8 | ((div >> 8) & 0x3) << 6 | 0x1);
	wr16(HC(SD_CLKCTL), c);
	uint64_t end = ticks() + ms_ticks(150);
	while (!(rd(HC(SD_CLKCTL)) & 0x2))
		if (ticks() > end)
			return -1;
	wr16(HC(SD_CLKCTL), c | 0x4);
	return 0;
}

static void delay_ms(unsigned ms)
{
	uint64_t end = ticks() + ms_ticks(ms);
	while (ticks() < end)
		;
}

/*
 * Bring the card from whatever ABL left to transfer state at default speed,
 * 1-bit bus, RCA 1. CMD0 only resets the card's state machine; it never
 * touches stored data.
 */
/* step (command index, or 0xc for clock) in the top byte, cmd() code below */
#define FAIL(step, rc)	((uint32_t)(step) << 24 | ((rc) & 0xffffff))

static uint32_t reinit_card(void)
{
	uint32_t rcgr = GCC_BASE + GCC_SDCC1_APPS_RCGR;
	con_hex2("RCG", "cfg=", rd(rcgr + 4), "m/n=", (rd(rcgr + 8) & 0xff) << 16 | (rd(rcgr + 0xc) & 0xff));

	/*
	 * ABL's controller reset does not cover the Qualcomm vendor registers,
	 * which may still select HS400's strobe-clocked data sampling. Do what
	 * mainline sdhci-msm does: vendor spec to its power-on value, then the
	 * default (non-HS400) input clock and no DLL power save.
	 */
	con_hex2("VENDOR", "spec=", rd(HC(MSM_VENDOR_SPEC)), "spec3=", rd(HC(MSM_VENDOR_SPEC3)));
	con_hex2("VENDOR", "dll=", rd(HC(MSM_DLL_CONFIG)), "ddr200=", rd(HC(MSM_DDR_200_CFG)));
	wr(HC(MSM_VENDOR_SPEC), VS_POR_VAL);
	wr(HC(MSM_VENDOR_SPEC3), rd(HC(MSM_VENDOR_SPEC3)) & ~VS3_PWRSAVE_DLL);
	uint32_t vs = rd(HC(MSM_VENDOR_SPEC));
	vs = (vs & ~VS_MCLK_SEL_MASK) | VS_MCLK_SEL_DFLT;
	vs &= ~(VS_SELECT_IN_EN | VS_SELECT_IN_MASK);
	wr(HC(MSM_VENDOR_SPEC), vs);

	wr(HC(MSM_PWRCTL_MASK), 0xf);
	wr(HC(SD_INTSIG_EN), 0);
	wr(HC(SD_INTSTS_EN), 0x03ff003f);
	wr8(HC(SD_TIMEOUT), 0x0e);

	/* Bus power on (VDD field 3.0 V for the 2.95 V vmmc), then 1.8 V signalling. */
	wr8(HC(SD_HOSTCTL + 1), 0x0c | 0x01);
	uint32_t p1 = pwr_handshake();
	uint32_t ctl2 = rd(HC(SD_HOSTCTL2));
	wr16(HC(SD_HOSTCTL2 + 2), (uint16_t)((ctl2 >> 16) | (1u << 3)));
	uint32_t p2 = pwr_handshake();
	wr(HC(MSM_VENDOR_SPEC), rd(HC(MSM_VENDOR_SPEC)) | VS_IO_PAD_PWR_SWITCH);
	con_hex2("POWER", "bus irq=", p1, "io irq=", p2);
	con_hex2("POWER", "hostctl=", rd(HC(SD_HOSTCTL)), "ctl2=", rd(HC(SD_HOSTCTL2)));

	if (set_clock(0x3ff))				/* <= 188 kHz at 384 MHz */
		return FAIL(0xc, 1);
	delay_ms(10);					/* >= 74 clocks before CMD0 */

	uint32_t rc, r = 0;
	rc = cmd(0, 0, C_RESP_NONE, 0, 0, 0, 0);	/* GO_IDLE_STATE */
	if (rc)
		return FAIL(0, rc);
	delay_ms(2);

	/* SEND_OP_COND until not busy: 1.7-1.95 V + 2.7-3.6 V, sector mode. */
	uint64_t end = ticks() + ms_ticks(1500);
	do {
		rc = cmd(1, 0x40ff8080, C_RESP_48, 0, 0, &r, 0);
		if (rc)
			return FAIL(1, rc);
		if (r & (1u << 31))
			break;
		delay_ms(10);
	} while (ticks() < end);
	con_hex2("CMD1", "ocr=", r, "sector=", (r >> 30) & 1);
	if (!(r & (1u << 31)))
		return FAIL(1, 0xbb);

	rc = cmd(2, 0, C_RESP_136 | C_CRC, 0, 0, &r, 0);	/* ALL_SEND_CID */
	if (rc)
		return FAIL(2, rc);
	con_hex2("CMD2", "cid0=", r, "sts=", rd(HC(SD_INTSTS)));
	rca = 1;
	for (int i = 0; i < 3; i++) {			/* SET_RELATIVE_ADDR */
		rc = cmd(3, rca << 16, R1, 0, 0, &r, 0);
		con_hex2("CMD3", "try=", i, "rc=", rc);
		if (!rc)
			break;
		con_hex2(" sts", "int=", last_sts, "present=", last_present);
		delay_ms(5);
	}
	if (rc)
		return FAIL(3, rc);
	con_hex("CMD3 r1", r);
	rc = cmd(7, rca << 16, C_RESP_48B | C_CRC | C_IDX, 0, 0, &r, 0);	/* SELECT */
	if (rc)
		return FAIL(7, rc);

	if (set_clock(8))				/* <= 24 MHz at 384 MHz */
		return FAIL(0xc, 8);
	return 0;
}

/* Read n (<= 64) sectors at lba: CMD17 for one, CMD18 + auto CMD12 for more. */
static uint32_t read_sectors(uint32_t lba, uint32_t n, uint8_t *out)
{
	if (n == 1)
		return cmd(17, lba, R1, 1, TM_READ, 0, out);
	return cmd(18, lba, R1, n, TM_READ | TM_MULTI | TM_BLKCNT | TM_ACMD12, 0, out);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Big GPT entries (>= 512 MiB), and userdata's filesystem magic. */
static void list_partitions(uint32_t elba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(64)));
	uint32_t secs = (nent * 128 + SECTOR - 1) / SECTOR, rc;
	for (uint32_t s = 0; s < secs; s += 32) {
		uint32_t n = secs - s < 32 ? secs - s : 32;
		if ((rc = read_sectors(elba + s, n, ents + s * SECTOR))) {
			con_hex("EMMC PART rc", rc);
			return;
		}
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		uint32_t mib = (uint32_t)((last - first + 1) >> 11);
		if (mib < 512)
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", mib);
		if (!__builtin_strcmp(name, "userdata") && !read_sectors((uint32_t)first + 2, 1, buf))
			/* ext4 magic at superblock+56 (offset 1024+56), f2fs at 1024 */
			con_hex2(" fs", "ext4=", buf[56] | buf[57] << 8, "f2fs=", le32(buf));
	}
}

void test_emmc(void)
{
	con_str("EMMC", "probe sdhc_1 (read-only, PIO)");

	/* Never touch an unclocked block: that can hang the bus. */
	uint32_t apps = rd(GCC_BASE + GCC_SDCC1_APPS_CBCR), ahb = rd(GCC_BASE + GCC_SDCC1_AHB_CBCR);
	con_hex2("GCC", "apps=", apps, "ahb=", ahb);
	if ((apps | ahb) & CBCR_CLK_OFF) {
		con_str("EMMC", "sdcc1 clocks off: stop");
		return;
	}

	uint32_t ver = rd(HC(SD_VERSION)) >> 16;
	con_hex2("SDHCI", "ver=", ver, "mci=", rd(HC(MSM_MCI_VERSION)));
	con_hex2("SDHCI", "caps=", rd(HC(SD_CAPS)), "caps1=", rd(HC(SD_CAPS1)));
	con_hex2("SDHCI", "present=", rd(HC(SD_PRESENT)), "hostctl=", rd(HC(SD_HOSTCTL)));
	con_hex2("SDHCI", "clk=", rd(HC(SD_CLKCTL)), "ctl2=", rd(HC(SD_HOSTCTL2)));
	con_hex2("SDHCI", "intsts=", rd(HC(SD_INTSTS)), "sig=", rd(HC(SD_INTSIG_EN)));
	uint32_t cqcfg = rd(CQ_BASE + 0x08);
	con_hex2("CQHCI", "ver=", rd(CQ_BASE + 0x00), "cfg=", cqcfg);

	if (cqcfg & 1) {
		con_str("EMMC", "host in command queue mode: stop");
		return;
	}
	wr(HC(SD_INTSTS), 0xffffffff);

	uint32_t st = 0, rc = 1;
	if (!(rd(HC(SD_HOSTCTL)) & (1u << 8))) {
		/* ABL left the host at power-on defaults: initialize the card again. */
		rc = reinit_card();
		con_hex("REINIT rc", rc);
		if (rc)
			return;
		rc = cmd(13, rca << 16, R1, 0, 0, &st, 0);
	} else {
		/* Host still configured: gated card clock back on, keeping ABL's divider. */
		uint16_t clk = (uint16_t)rd(HC(SD_CLKCTL));
		if ((clk & 0x7) != 0x7 && set_clock((clk >> 8) | ((clk >> 6) & 3) << 8)) {
			con_str("EMMC", "clock not stable: stop");
			return;
		}
		wr(HC(SD_INTSIG_EN), 0);
		wr(HC(SD_INTSTS_EN), 0x03ff003f);
		wr8(HC(SD_TIMEOUT), 0x0e);
		/* ABL's RCA isn't known: CMD13 answers only to the right one. */
		for (rca = 1; rca <= 4; rca++)
			if (!(rc = cmd(13, rca << 16, R1, 0, 0, &st, 0)))
				break;
	}
	con_hex2("CMD13", "rca=", rca, "rc=", rc);
	if (rc)
		return;
	con_hex2("CMD13", "status=", st, "state=", (st >> 9) & 0xf);	/* 4 = tran */
	if (((st >> 9) & 0xf) != 4) {
		con_str("EMMC", "card not in transfer state: stop");
		return;
	}

	rc = cmd(8, 0, R1, 1, TM_READ, 0, buf);
	con_hex("CMD8 rc", rc);
	if (rc) {
		con_hex2(" sts", "int=", last_sts, "present=", last_present);
		con_hex(" int_en", rd(HC(SD_INTSTS_EN)));
		return;
	}
	con_hex2("CMD8 ok", "int_en=", rd(HC(SD_INTSTS_EN)), "present=", rd(HC(SD_PRESENT)));
	uint32_t sectors = le32(buf + 212);
	con_hex2("EXTCSD", "rev=", buf[192], "sectors=", sectors);
	con_hex2("EXTCSD", "part_cfg=", buf[179], "cmdq_en=", buf[15]);
	con_hex2("EXTCSD", "hs_timing=", buf[185], "bus_width=", buf[183]);
	if (buf[15] & 1) {
		con_str("EMMC", "card in command queue mode: stop");
		return;
	}
	if (buf[179] & 7) {
		con_str("EMMC", "user area not selected: stop");
		return;
	}

	rc = read_sectors(0, 2, buf);			/* protective MBR + GPT header */
	con_hex("READ rc", rc);
	if (rc)
		return;
	const uint8_t *gpt = buf + SECTOR;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("GPT", sig);
	uint32_t nent = le32(gpt + 80), esz = le32(gpt + 84), elba = (uint32_t)le64(gpt + 72);
	con_hex2("GPT", "entries=", nent, "lba=", elba);
	if (__builtin_memcmp(gpt, "EFI PART", 8) || esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);

	/* PIO throughput: 4 MiB in 32 KiB CMD18 reads from the start of the disk. */
	uint64_t t0 = ticks();
	for (uint32_t lba = 0; lba < 8192 && !rc; lba += 64)
		rc = read_sectors(lba, 64, buf);
	uint64_t dt = ticks() - t0, f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	con_hex2("4MiB", "rc=", rc, "ms=", (uint32_t)(dt * 1000 / f));
}
