/*
 * Minimal bare-metal DSI bring-up for kagura (MSM8996 / DSI 6G + 14nm PHY).
 *
 * The bootloader leaves the panel powered but the DSI link disabled; the Sony
 * logo is only the panel's retained GRAM image.  This file starts the 14nm
 * PLL/PHY, sends the DT panel-on command sequence, then pushes a full frame
 * straight through the DSI command DMA (no MDP pipeline needed).
 */
#include <stdint.h>
#include "dsi.h"
#include "plog.h"

#define MMCC_BASE	0x008c0000UL
#define DSI_BASE	0x00994000UL
#define PHY_CMN		0x00994400UL
#define PHY_LN		0x00994500UL
#define PHY_PLL		0x00994800UL

/* MMCC byte0 RCG: source 1 = dsi0pllbyte, /2 via pre_div=1 */
#define MMCC_BYTE0_CFG	(MMCC_BASE + 0x2120)
#define MMCC_BYTE0_CMD	(MMCC_BASE + 0x2124)
#define BYTE0_SRC_DSI0	(1U << 8)
#define BYTE0_DIV_2	1U

/* DSI controller registers (dsi.xml.h) */
#define DSI_CTRL		0x000
#define DSI_STATUS0		0x004
#define DSI_FIFO_STATUS		0x008
#define DSI_CMD_CFG0		0x03c
#define DSI_CMD_CFG1		0x040
#define DSI_CMD_DMA_CTRL	0x038
#define DSI_DMA_BASE		0x044
#define DSI_DMA_LEN		0x048
#define DSI_TRIG_CTRL		0x080
#define DSI_LANE_CTRL		0x0a8
#define DSI_RESET		0x114
#define DSI_CLK_CTRL		0x118
#define DSI_CLK_STATUS		0x11c
#define DSI_PHY_RESET		0x128
#define DSI_TRIG_DMA		0x08c

#define CTRL_ENABLE		0x00000001U
#define CTRL_CMD_MODE_EN	0x00000004U
#define CTRL_LANES		0x000000f0U
#define CTRL_CLK_EN		0x00000100U
#define CTRL_ECC_CHECK		0x00100000U
#define CTRL_CRC_CHECK		0x01000000U
#define CLK_CTRL_ALL		0x0000023fU	/* + FORCE_ON_DYN_AHBM_HCLK */
#define DMA_BUSY		0x00000002U
#define CMD_DMA_LOW_POWER	0x04000000U
#define CMD_DMA_FROM_FB		0x10000000U

/* 14nm PHY common registers */
#define CMN_CLK_CFG1		0x14
#define CMN_GLBL_TEST_CTRL	0x18
#define CMN_CTRL_0		0x1c
#define CMN_CTRL_1		0x20
#define CMN_PLL_CNTRL		0x48
#define CMN_LDO_CNTRL		0x4c

/* 14nm PLL registers (PHY_PLL base) */
#define PLL_IE_TRIM		0x000
#define PLL_IP_TRIM		0x004
#define PLL_IPTAT_TRIM		0x010
#define PLL_CLKBUFLR_EN		0x01c
#define PLL_SYSCLK_EN_RESET	0x028
#define PLL_RESETSM_CNTRL	0x02c
#define PLL_RESETSM_CNTRL2	0x030
#define PLL_RESETSM_CNTRL5	0x03c
#define PLL_KVCO_DIV_REF1	0x040
#define PLL_KVCO_DIV_REF2	0x044
#define PLL_VREF_CFG1		0x05c
#define PLL_VCO_DIV_REF1	0x06c
#define PLL_VCO_DIV_REF2	0x070
#define PLL_VCO_COUNT1		0x074
#define PLL_VCO_COUNT2		0x078
#define PLL_KVCO_COUNT1		0x048
#define PLL_KVCO_COUNT2		0x04c
#define PLL_PLLLOCK_CMP1	0x07c
#define PLL_PLLLOCK_CMP2	0x080
#define PLL_PLLLOCK_CMP3	0x084
#define PLL_PLLLOCK_CMP_EN	0x088
#define PLL_DEC_START		0x090
#define PLL_DIV_FRAC_START1	0x0b4
#define PLL_DIV_FRAC_START2	0x0b8
#define PLL_DIV_FRAC_START3	0x0bc
#define PLL_TXCLK_EN		0x0c0
#define PLL_CRCTRL		0x0c4
#define PLL_RESET_SM_READY_STATUS 0x0cc
#define PLL_MISC1		0x0e8
#define PLL_CP_SET_CUR		0x0f0
#define PLL_ICPMSET		0x0f4
#define PLL_ICPCSET		0x0f8
#define PLL_ICP_SET		0x0fc
#define PLL_LPF1		0x100
#define PLL_LPF2_POSTDIV	0x104

/* Link / PLL parameters.  1080x1920@60, 24bpp, 4 lanes, blanking from DT. */
#define H_TOTAL	1152U
#define V_TOTAL	2163U
#define FPS	60U
#define BPP	24U
#define LANES	4U
#define BITRATE (((uint64_t)H_TOTAL * V_TOTAL * FPS * BPP) / LANES)
#define VCO_RATE (BITRATE * 2U)	/* CMN_CLK_CFG1 selects the /2 path */
#define REF_RATE 19200000ULL

/* D-PHY timings, computed at runtime by msm_dsi_dphy_timing_calc_v2(). */
struct dphy_timing {
	uint32_t clk_zero, clk_trail, clk_prepare, hs_exit;
	uint32_t hs_zero, hs_prepare, hs_trail, hs_rqst;
	uint32_t ta_go, ta_sure, ta_get;
	uint32_t clk_pre, clk_post, clk_pre_inc_by_2;
	uint32_t hs_rqst_ckln, hs_prep_dly, hs_prep_dly_ckln;
	uint32_t hs_halfbyte_en, hs_halfbyte_en_ckln;
};

static struct dphy_timing tm;

/* DMA packet buffer.  One long packet at a time keeps this small. */
#define PKT_MAX 65536U
static uint8_t pktbuf[PKT_MAX + 8] __attribute__((aligned(8)));

/* ---- small helpers ---- */

static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint8_t rd8(uintptr_t a) { return *(volatile uint8_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static uint64_t tick_hz(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static void udelay(unsigned us)
{
	uint64_t end = ticks() + tick_hz() / 1000000U * us;
	while (ticks() < end)
		;
}

static void mdelay(unsigned ms) { udelay(ms * 1000); }

static void stage(const char *s)
{
	plog_str("DSI ");
	plog_str(s);
	plog_str("\n");
	plog_commit();
}

/* ---- D-PHY timing calculation (mainline msm_dsi_dphy_timing_calc_v2) ---- */

static int32_t sdiv_up(int32_t n, int32_t d)
{
	return n >= 0 ? (n + d - 1) / d : (n - d + 1) / d;
}

static int32_t div_up_u(uint32_t n, uint32_t d) { return (int32_t)((n + d - 1) / d); }

static int32_t linear_inter(int32_t tmax, int32_t tmin, int32_t pct,
			    int32_t min_result, int even)
{
	int32_t v = sdiv_up((tmax - tmin) * pct, 100) + tmin;
	if (even && (v & 1))
		return v > min_result + 1 ? v - 1 : min_result;
	return v > min_result ? v : min_result;
}

static int calc_timing(uint32_t bit_rate, uint32_t esc_rate)
{
	const int32_t coeff = 1000;
	int32_t ui, ui_x8, tmin, tmax, temp;
	int32_t val, val_ckln, pd, pd_ckln;

	if (!bit_rate || !esc_rate)
		return -1;
	tm.hs_halfbyte_en = 0;
	tm.hs_halfbyte_en_ckln = 0;
	tm.hs_prep_dly_ckln = (bit_rate > 100000000U) ? 0 : 3;
	tm.hs_prep_dly = (bit_rate > 120000000U) ? 0 : 1;
	pd_ckln = tm.hs_prep_dly_ckln;
	pd = tm.hs_prep_dly;
	val = (pd << 1);
	val_ckln = (pd_ckln << 1);

	/* ui in 1/1000 ns, multiply by 1e6 ns/ms and divide by kbps */
	ui = (int32_t)((1000000ULL * coeff * 1000000ULL) / bit_rate);
	ui_x8 = ui << 3;

	tmin = sdiv_up(38 * coeff - val_ckln * ui, ui_x8);
	if (tmin < 0) tmin = 0;
	tmax = (95 * coeff - val_ckln * ui) / ui_x8;
	if (tmax < 0) tmax = 0;
	tm.clk_prepare = (uint32_t)linear_inter(tmax, tmin, 50, 0, 0);

	temp = 300 * coeff - ((int32_t)tm.clk_prepare * 8 + val_ckln) * ui;
	tmin = sdiv_up(temp - 11 * ui, ui_x8) - 3;
	tmax = tmin > 255 ? 511 : 255;
	tm.clk_zero = (uint32_t)linear_inter(tmax, tmin, 2, 0, 0);

	tmin = div_up_u(60U * coeff + 3U * (uint32_t)ui, (uint32_t)ui_x8);
	temp = 105 * coeff + 12 * ui - 20 * coeff;
	tmax = (temp + 3 * ui) / ui_x8;
	tm.clk_trail = (uint32_t)linear_inter(tmax, tmin, 30, 0, 0);

	tmin = sdiv_up(40 * coeff + 4 * ui - val * ui, ui_x8);
	if (tmin < 0) tmin = 0;
	tmax = (85 * coeff + 6 * ui - val * ui) / ui_x8;
	if (tmax < 0) tmax = 0;
	tm.hs_prepare = (uint32_t)linear_inter(tmax, tmin, 50, 0, 0);

	temp = 145 * coeff + 10 * ui -
	       ((int32_t)tm.hs_prepare * 8 + val) * ui;
	tmin = sdiv_up(temp - 11 * ui, ui_x8) - 3;
	tmax = 255;
	tm.hs_zero = (uint32_t)linear_inter(tmax, tmin, 10, 0, 0);

	tmin = div_up_u(60U * coeff + 7U * (uint32_t)ui, (uint32_t)ui_x8);
	temp = 105 * coeff + 12 * ui - 20 * coeff;
	tmax = (temp + 3 * ui) / ui_x8;
	tm.hs_trail = (uint32_t)linear_inter(tmax, tmin, 30, 0, 0);

	tm.hs_rqst = (uint32_t)sdiv_up(50 * coeff - 8 * ui, ui_x8);
	tmin = div_up_u(100U * coeff, (uint32_t)ui_x8) - 1;
	tm.hs_exit = (uint32_t)linear_inter(255, tmin, 10, 0, 0);
	tm.hs_rqst_ckln = tm.hs_rqst;

	tmin = div_up_u(60U * coeff + 52U * (uint32_t)ui - 43U * (uint32_t)ui,
			(uint32_t)ui_x8) - 1;
	tm.clk_post = (uint32_t)linear_inter(63, tmin, 10, 0, 0);

	temp = 8 * ui + ((int32_t)tm.clk_prepare * 8 + val_ckln) * ui;
	temp += (((int32_t)tm.clk_zero + 3) * 8 + 11 - (pd_ckln << 1)) * ui;
	tmin = div_up_u((uint32_t)temp, (uint32_t)(8 * ui)) - 1;
	if (tmin > 63) {
		tm.clk_pre = (uint32_t)linear_inter(126, tmin, 10, 0, 0) >> 1;
		tm.clk_pre_inc_by_2 = 1;
	} else {
		tm.clk_pre = (uint32_t)linear_inter(63, tmin, 10, 0, 0);
		tm.clk_pre_inc_by_2 = 0;
	}
	tm.ta_go = 3;
	tm.ta_sure = 0;
	tm.ta_get = 4;
	return 0;
}

/* ---- PLL ---- */

struct pll_conf {
	uint32_t dec_start, div_frac_start, plllock_cmp, plllock_cnt;
	uint32_t vco_div_ref, vco_count, kvco_div_ref, kvco_count;
};

static void pll_calc(uint64_t vco, struct pll_conf *p)
{
	uint64_t mult = 1ULL << 20;
	uint64_t dm = vco * mult / REF_RATE;
	uint64_t rem = dm % mult;
	uint64_t cmp;

	p->dec_start = (uint32_t)(dm / mult);
	p->div_frac_start = (uint32_t)rem;
	p->plllock_cnt = 1;
	cmp = 256 * dm / mult;
	p->plllock_cmp = (uint32_t)(cmp / 10);

	p->vco_div_ref = (uint32_t)((REF_RATE * 5 / 1000000) & 0x3ff) - 2;
	p->vco_count = (uint32_t)(vco / 1000000 * 5 / 10);
	p->kvco_div_ref = (uint32_t)((REF_RATE * 5 / 1000000) & 0x3ff) - 1;
	{
		uint32_t slop;
		if (vco > 1300000000ULL && vco <= 1800000000ULL) slop = 600;
		else if (vco > 1800000000ULL && vco < 2300000000ULL) slop = 400;
		else slop = 280;
		p->kvco_count = slop * 2 / 100 * 5;
	}
}

static void pll_program(const struct pll_conf *p)
{
	uintptr_t b = PHY_PLL, c = PHY_CMN;

	wr8(c + CMN_LDO_CNTRL, 0x3c);
	wr8(b + PLL_SYSCLK_EN_RESET, 0);
	wr8(b + PLL_TXCLK_EN, 1);
	wr8(b + PLL_RESETSM_CNTRL, 48);
	wr8(b + PLL_RESETSM_CNTRL2, 4 << 3);
	wr8(b + PLL_RESETSM_CNTRL5, 5);
	wr8(b + PLL_VCO_DIV_REF1, p->vco_div_ref & 0xff);
	wr8(b + PLL_VCO_DIV_REF2, (p->vco_div_ref >> 8) & 3);
	wr8(b + PLL_KVCO_DIV_REF1, p->kvco_div_ref & 0xff);
	wr8(b + PLL_KVCO_DIV_REF2, (p->kvco_div_ref >> 8) & 3);
	wr8(b + PLL_MISC1, 16);
	wr8(b + PLL_IE_TRIM, 4);
	wr8(b + PLL_IP_TRIM, 4);
	wr8(b + PLL_CP_SET_CUR, (1 << 3) | 1);
	wr8(b + PLL_ICPCSET, 0);
	wr8(b + PLL_ICPMSET, 0);
	wr8(b + PLL_ICP_SET, (4 << 3) | 4);
	wr8(b + PLL_LPF1, (1 << 4) | 11);
	wr8(b + PLL_IPTAT_TRIM, 7);
	wr8(b + PLL_CRCTRL, (1 << 4) | 2);

	/* PLL software reset */
	wr8(c + CMN_PLL_CNTRL, 0);
	wr8(c + CMN_CTRL_1, 0x20);
	dsb();
	udelay(10);
	wr8(c + CMN_CTRL_1, 0);
	dsb();

	wr8(c + CMN_CLK_CFG1, 1);	/* use the /2 path */
	wr8(c + CMN_CTRL_0, 0xff);

	wr8(b + PLL_DEC_START, p->dec_start);
	wr8(b + PLL_DIV_FRAC_START1, p->div_frac_start & 0xff);
	wr8(b + PLL_DIV_FRAC_START2, (p->div_frac_start >> 8) & 0xff);
	wr8(b + PLL_DIV_FRAC_START3, (p->div_frac_start >> 16) & 0xf);
	wr8(b + PLL_PLLLOCK_CMP1, p->plllock_cmp & 0xff);
	wr8(b + PLL_PLLLOCK_CMP2, (p->plllock_cmp >> 8) & 0xff);
	wr8(b + PLL_PLLLOCK_CMP3, (p->plllock_cmp >> 16) & 3);
	wr8(b + PLL_PLLLOCK_CMP_EN, (p->plllock_cnt << 1));
	wr8(b + PLL_VCO_COUNT1, p->vco_count & 0xff);
	wr8(b + PLL_VCO_COUNT2, (p->vco_count >> 8) & 0xff);
	wr8(b + PLL_KVCO_COUNT1, p->kvco_count & 0xff);
	wr8(b + PLL_KVCO_COUNT2, (p->kvco_count >> 8) & 3);
	wr8(b + PLL_LPF2_POSTDIV, 3);	/* postdiv /1 */
	dsb();
}

static int pll_start(void)
{
	uintptr_t b = PHY_PLL, c = PHY_CMN;
	int i;
	uint8_t v;

	wr8(b + PLL_VREF_CFG1, 0x10);
	wr8(c + CMN_PLL_CNTRL, 1);
	dsb();
	for (i = 0; i < 15; i++) {
		v = rd8(b + PLL_RESET_SM_READY_STATUS);
		if (v & 0x20) break;
		udelay(1000);
	}
	if (!(v & 0x20)) { plog_hex("PLL lock timeout raw", v); return -1; }
	for (i = 0; i < 15; i++) {
		v = rd8(b + PLL_RESET_SM_READY_STATUS);
		if (v & 0x01) break;
		udelay(1000);
	}
	plog_hex("PLL status", v);
	return (v & 0x01) ? 0 : -1;
}

/* ---- PHY ---- */

static void lane_timing(unsigned lane)
{
	uintptr_t l = PHY_LN + lane * 0x80;
	unsigned ck = (lane == 4);

	wr8(l + 0x18, tm.hs_exit);
	wr8(l + 0x1c, ck ? tm.clk_zero : tm.hs_zero);
	wr8(l + 0x20, ck ? tm.clk_prepare : tm.hs_prepare);
	wr8(l + 0x24, ck ? tm.clk_trail : tm.hs_trail);
	wr8(l + 0x28, ck ? tm.hs_rqst_ckln : tm.hs_rqst);
	wr8(l + 0x00, (ck ? tm.hs_prep_dly_ckln : tm.hs_prep_dly) << 1);
	wr8(l + 0x04, 0);
	wr8(l + 0x2c, (tm.ta_go << 4) | tm.ta_sure);
	wr8(l + 0x30, tm.ta_get);
	wr8(l + 0x34, 0xa0);
}

static void phy_enable(void)
{
	uintptr_t c = PHY_CMN, l = PHY_LN;
	unsigned i;

	wr8(c + CMN_LDO_CNTRL, 0x1c);	/* standalone PHY */
	wr8(c + CMN_GLBL_TEST_CTRL, 1);
	for (i = 0; i < 5; i++) {
		uintptr_t ln = l + i * 0x80;
		wr8(ln + 0x64, 0x1d);
		wr8(ln + 0x38, 0xff);
		wr8(ln + 0x3c, i == 4 ? 0x00 : 0x06);
		wr8(ln + 0x0c, i == 4 ? 0x8f : 0x0f);
		wr8(ln + 0x08, 0x10);
		wr8(ln + 0x10, 0);
		wr8(ln + 0x14, 0x88);
		lane_timing(i);
	}
	wr8(c + CMN_PLL_CNTRL, 0);
	dsb();
	wr8(c + CMN_CTRL_1, 0x80);
	dsb();
	udelay(100);
	wr8(c + CMN_CTRL_1, 0);
	wr8(c + CMN_GLBL_TEST_CTRL, rd8(c + CMN_GLBL_TEST_CTRL) & ~4U);
	wr8(PHY_PLL + PLL_CLKBUFLR_EN, 1);
	wr8(c + CMN_CTRL_0, 0xff);
	dsb();
}

/* ---- DSI packet helpers ----
 *
 * MSM's CMD_DMA engine does not consume raw MIPI DSI wire packets.  It takes
 * a 4-byte MSM descriptor followed by the payload, and the DSI controller
 * generates ECC/CRC itself (see mainline dsi_cmd_dma_add()):
 *
 *   [0] word count low
 *   [1] word count high
 *   [2] data type (with virtual channel)
 *   [3] BIT(7) = last packet, BIT(6) = long packet, BIT(5) = read
 *   [4..] payload (DCS long writes include the command byte here)
 *   padding to a 4-byte boundary with 0xff
 */
static unsigned make_packet(uint8_t dt, const uint8_t *payload, unsigned len)
{
	unsigned total, pad;
	if (len > PKT_MAX - 4) len = PKT_MAX - 4;
	pktbuf[0] = len & 0xffU;
	pktbuf[1] = (len >> 8) & 0xffU;
	pktbuf[2] = dt;
	pktbuf[3] = 0x80U | 0x40U;	/* last, long packet */
	if (payload != pktbuf + 4)
		for (unsigned i = 0; i < len; i++)
			pktbuf[4 + i] = payload[i];
	total = 4 + len;
	pad = (-total) & 3U;
	for (unsigned i = 0; i < pad; i++)
		pktbuf[total + i] = 0xffU;
	return total + pad;
}

static void dma_send(unsigned len, int lp)
{
	uintptr_t d = DSI_BASE;
	wr32(d + DSI_CMD_DMA_CTRL, CMD_DMA_FROM_FB | (lp ? CMD_DMA_LOW_POWER : 0));
	wr32(d + DSI_DMA_BASE, (uint32_t)(uintptr_t)pktbuf);
	wr32(d + DSI_DMA_LEN, len);
	dsb();
	wr32(d + DSI_TRIG_DMA, 1);
	dsb();
}

static int dma_wait(void)
{
	uintptr_t d = DSI_BASE;
	uint64_t end = ticks() + tick_hz();
	while (ticks() < end) {
		if (!(rd32(d + DSI_STATUS0) & DMA_BUSY)) return 0;
	}
	plog_hex("DMA timeout status", rd32(d + DSI_STATUS0));
	return -1;
}

static int send_cmd(uint8_t dtype, const uint8_t *payload, unsigned len,
		    unsigned wait_ms, int lp)
{
	unsigned n = make_packet(dtype, payload, len);
	dma_send(n, lp);
	if (dma_wait()) return -1;
	if (wait_ms) mdelay(wait_ms);
	return 0;
}

/* Panel on-command bytes from the stock DTB (7-byte header + payload). */
static const uint8_t panel_on[] = {
	0x29,0x01,0,0,0,0,0x02, 0xb0,0x00,
	0x29,0x01,0,0,0,0,0x02, 0xd6,0x01,
	0x29,0x01,0,0,0,0,0x03, 0xc4,0x70,0x22,
	0x29,0x01,0,0,0,0,0x15, 0xc6,0x53,0x2e,0x2e,0x05,0x45,0x00,0x00,0x00,0x00,0x00,0x00,0x42,0x0f,0x00,0x00,0x00,0x00,0x04,0x10,0x06,
	0x29,0x01,0,0,0,0,0x0e, 0xec,0x64,0xdc,0xec,0x3b,0x52,0x00,0x0b,0x0b,0x13,0x15,0x68,0x0b,0xb5,
	0x29,0x01,0,0,0,0,0x02, 0xb0,0x03,
	0x39,0x01,0,0,0,0,0x02, 0x35,0x00,
	0x39,0x01,0,0,0,0,0x02, 0x36,0x00,
	0x39,0x01,0,0,0,0,0x02, 0x3a,0x77,
	0x39,0x01,0,0,0,0,0x05, 0x2a,0x00,0x00,0x04,0x37,
	0x39,0x01,0,0,0,0,0x05, 0x2b,0x00,0x00,0x07,0x7f,
	0x39,0x01,0,0,0,0,0x03, 0x44,0x00,0x00,
	0x39,0x01,0,0,0x78,0x00,0x01, 0x11,
};

/* Panel reset line: TLMM GPIO 8, active-low.  REG_BASE=0, REG_SIZE=0x1000,
 * ctl_reg=base+pin*0x1000 (oe_bit 9), io_reg=+4 (out_bit 1).
 * Sequence from the panel DT: somc,pw-on-rst-seq. */
#define TLMM_BASE	0x01010000UL
#define GPIO8_CTL	(TLMM_BASE + 8U * 0x1000U)
#define GPIO8_IO	(GPIO8_CTL + 4U)

static void panel_reset(void)
{
	uint32_t v = rd32(GPIO8_CTL);
	stage("panel reset");
	v |= 1U << 9;			/* output enable */
	v &= ~(7U << 2);		/* function 0 = gpio */
	wr32(GPIO8_CTL, v); dsb();
	/* Exact Sony DT sequence somc,pw-on-rst-seq =
	 * <0 10, 1 10, 0 1, 1 10> (value, delay ms). */
	wr32(GPIO8_IO, 0); dsb(); mdelay(10);
	wr32(GPIO8_IO, 2); dsb(); mdelay(10);
	wr32(GPIO8_IO, 0); dsb(); mdelay(1);
	wr32(GPIO8_IO, 2); dsb(); mdelay(10);
	plog_hex("GPIO8 ctl", rd32(GPIO8_CTL));
	plog_hex("GPIO8 io", rd32(GPIO8_IO));
	plog_commit();
}

static int panel_init(void)
{
	const uint8_t *p = panel_on;
	unsigned left = sizeof(panel_on);
	unsigned ncmd = 0;
	while (left >= 7) {
		/* qcom DTB DCS command header (7 bytes):
		 *   [0] dtype, [1] last, [2] vc, [3] ack,
		 *   [4] wait-after in ms, [5..6] big-endian payload length.
		 * The wait is applied AFTER transmission, as in the kernel. */
		unsigned dtype = p[0];
		unsigned wait = p[4];
		unsigned dlen = ((unsigned)p[5] << 8) | p[6];
		if (dlen == 0 || dlen > left - 7) break;
		plog_hex("cmd meta", ((uint64_t)dtype << 48) |
			 ((uint64_t)wait << 32) | dlen);
		if (send_cmd((uint8_t)dtype, p + 7, dlen, wait, 1)) return -1;
		ncmd++;
		p += 7 + dlen;
		left -= 7 + dlen;
	}
	plog_commit();
	plog_hex("panel cmds sent", ncmd);
	plog_hex("panel bytes left", left);
	{
		/* qcom,mdss-dsi-post-panel-on-command = <0x39010000 0x129>:
		 * generic long write, payload = DCS display-on (0x29). */
		static const uint8_t display_on[] = { 0x29 };
		if (send_cmd(0x39, display_on, 1, 0, 1)) return -1;
	}
	stage("panel init done");
	return 0;
}

/* Push the splash framebuffer through DSI DMA as DCS memory writes. */
#define FB_BASE 0x83401000UL
#define FB_STRIDE 4352U
#define FB_W 1080U
#define FB_H 1920U

static int push_frame(void)
{
	static const uint8_t col[] = { 0x2a,0,0,0x04,0x37 };
	static const uint8_t page[] = { 0x2b,0,0,0x07,0x7f };
	unsigned row = 0, chunk_cap;
	uint32_t px;
	int first = 1;

	/* DCS long writes (DT 0x29), not generic long writes. */
	if (send_cmd(0x29, col, sizeof(col), 0, 0)) return -1;
	if (send_cmd(0x29, page, sizeof(page), 0, 0)) return -1;
	/* MSM DMA descriptor: 4-byte header + payload[0] = 0x2c/0x3c. */
	chunk_cap = PKT_MAX - 4 - 1;
	while (row < FB_H) {
		uint8_t *out = pktbuf + 5;	/* after 4-byte header + DCS byte */
		unsigned n = 0;
		unsigned max_pixels = (chunk_cap - 1) / 3;
		/* qcom,mdss-dsi-wr-mem-start / -continue. */
		pktbuf[4] = first ? 0x2c : 0x3c;
		first = 0;
		while (n < max_pixels && row < FB_H) {
			unsigned colx = n % FB_W;
			px = rd32(FB_BASE + (uintptr_t)(row * FB_STRIDE + colx * 4));
			out[n*3+0] = (uint8_t)(px >> 16);
			out[n*3+1] = (uint8_t)(px >> 8);
			out[n*3+2] = (uint8_t)px;
			n++;
			if (colx == FB_W - 1) row++;
		}
		{
			unsigned len = make_packet(0x29, pktbuf + 4, 1 + n * 3);
			dma_send(len, 0);
			if (dma_wait()) return -1;
		}
	}
	stage("frame pushed");
	return 0;
}

/* One-shot diagnostic: which MMIO writes actually take effect? */
static void dsi_write_test(void)
{
	uintptr_t d = DSI_BASE;
	uint32_t v;

	stage("write test");
	plog_hex("pre CTRL", rd32(d + DSI_CTRL));
	plog_hex("pre CLKCTRL", rd32(d + DSI_CLK_CTRL));
	plog_hex("pre TRIGCTRL", rd32(d + DSI_TRIG_CTRL));
	plog_hex("pre RESET", rd32(d + DSI_RESET));
	plog_hex("pre PHYRESET", rd32(d + DSI_PHY_RESET));
	plog_hex("pre PHYC0", rd8(PHY_CMN + CMN_CTRL_0));
	plog_hex("pre PLLIE", rd8(PHY_PLL + PLL_IE_TRIM));
	plog_hex("pre MDP LM0+8", rd32(0x900000UL + 0x45008));
	plog_commit();

	wr32(d + DSI_CTRL, 0); dsb();
	plog_hex("post CTRL0", rd32(d + DSI_CTRL));
	wr32(d + DSI_CTRL, 0xdeadbeefU); dsb();
	plog_hex("post CTRL32", rd32(d + DSI_CTRL));
	wr8(d + DSI_CTRL, 0xf5); dsb();
	plog_hex("post CTRL8", rd32(d + DSI_CTRL));
	wr32(d + DSI_CLK_CTRL, 0x23f); dsb();
	plog_hex("post CLKCTRL", rd32(d + DSI_CLK_CTRL));
	wr32(d + DSI_TRIG_CTRL, 0x55); dsb();
	plog_hex("post TRIGCTRL", rd32(d + DSI_TRIG_CTRL));
	wr8(PHY_CMN + CMN_CTRL_0, 0xa5); dsb();
	plog_hex("post PHYC0", rd8(PHY_CMN + CMN_CTRL_0));
	wr8(PHY_PLL + PLL_IE_TRIM, 0x5a); dsb();
	plog_hex("post PLLIE", rd8(PHY_PLL + PLL_IE_TRIM));
	wr32(0x900000UL + 0x45008, 0x55); dsb();
	plog_hex("post MDP", rd32(0x900000UL + 0x45008));
	plog_commit();

	/* Where is the bootloader's live framebuffer? */
	plog_hex("VIG0 SRC0", rd32(0x900000UL + 0x5000 + 0x14));
	plog_hex("RGB0 SRC0", rd32(0x900000UL + 0x15000 + 0x14));
	plog_hex("DMA0 SRC0", rd32(0x900000UL + 0x25000 + 0x14));
	plog_hex("VIG0 CURSRC", rd32(0x900000UL + 0x5000 + 0xa4));
	plog_hex("RGB0 CURSRC", rd32(0x900000UL + 0x15000 + 0xa4));
	plog_hex("DMA0 CURSRC", rd32(0x900000UL + 0x25000 + 0xa4));
	plog_commit();

	/* Clock-state probe: DSI CLK_STATUS tells us which domain clocks are
	 * actually running.  mmcc pclk0 was never enabled in earlier builds. */
	{
		uintptr_t m = MMCC_BASE;
		stage("clock probe");
		plog_hex("CLKSTATUS pre", rd32(d + 0x11c));
		plog_hex("MMCC pclk0 pre", rd32(m + 0x2314));
		plog_hex("MMCC pclk1 pre", rd32(m + 0x2318));
		plog_hex("MMCC vsync pre", rd32(m + 0x2324));
		plog_commit();

		/* Enable every MDSS branch clock (CBCR bit 0). */
		wr32(m + 0x2308, 1);	/* ahb */
		wr32(m + 0x230c, 1);	/* hdmi ahb */
		wr32(m + 0x2310, 1);	/* axi */
		wr32(m + 0x2314, 1);	/* pclk0 */
		wr32(m + 0x2318, 1);	/* pclk1 */
		wr32(m + 0x231c, 1);	/* mdp */
		wr32(m + 0x2324, 1);	/* vsync */
		wr32(m + 0x233c, 1);	/* byte0 */
		wr32(m + 0x2340, 1);	/* byte1 */
		wr32(m + 0x2344, 1);	/* esc0 */
		wr32(m + 0x2348, 1);	/* esc1 */
		dsb();
		/* pclk0 RCG: source 1 = dsi0pll. */
		wr32(m + 0x2004, 1U << 8); dsb();
		wr32(m + 0x2000, 1); dsb();
		plog_hex("pclk0 cfg", rd32(m + 0x2004));
		plog_hex("MMCC pclk0 post", rd32(m + 0x2314));
		plog_commit();

		wr32(d + DSI_CLK_CTRL, 0x23f); dsb();
		mdelay(1);
		plog_hex("CLKCTRL now", rd32(d + DSI_CLK_CTRL));
		plog_hex("CLKSTATUS now", rd32(d + 0x11c));
		plog_commit();

		/* With clocks enabled, retry the write-only CTRL register and
		 * send a DCS display-off to see if the link is alive now. */
		wr32(d + DSI_CTRL, 0x11001f5U); dsb();
		{
			static const uint8_t dcs_off[] = { 0x28 };
			stage("display off retry");
			send_cmd(0x29, dcs_off, sizeof(dcs_off), 100, 1);
			plog_hex("status post retry", rd32(d + DSI_STATUS0));
			plog_commit();
		}
	}

	/* restore the two config registers we care about */
	wr32(d + DSI_CTRL, 0x10040001U);
	wr32(d + DSI_TRIG_CTRL, 4);
	wr8(PHY_CMN + CMN_CTRL_0, 0xff);
	wr8(PHY_PLL + PLL_IE_TRIM, 4);
	dsb();
}

int dsi_bringup(void)
{
	uintptr_t d = DSI_BASE;
	struct pll_conf pc;
	uint32_t bitrate = (uint32_t)BITRATE;

	/* Live state before we reprogram anything. DSI/PHY reads have been
	 * proven safe in this harness; commit each so a hang is locatable. */
	stage("live state");
	plog_hex("HW version", rd32(d + DSI_CTRL));	/* alias: 0x10040001 */
	plog_hex("STATUS0", rd32(d + DSI_STATUS0));
	plog_hex("FIFO_STATUS", rd32(d + DSI_FIFO_STATUS));
	plog_hex("CLKCTRL", rd32(d + DSI_CLK_CTRL));
	plog_hex("CLKSTATUS", rd32(d + DSI_CLK_STATUS));
	plog_hex("CMD_DMA_CTRL", rd32(d + DSI_CMD_DMA_CTRL));
	plog_commit();
	plog_hex("PHY CTRL0", rd8(PHY_CMN + CMN_CTRL_0));
	plog_hex("PHY CLKCFG1", rd8(PHY_CMN + CMN_CLK_CFG1));
	plog_hex("PLL status", rd8(PHY_PLL + PLL_RESET_SM_READY_STATUS));
	plog_commit();

	/* The kernel feeds DSI DMA an IOMMU-mapped IOVA.  We are bare metal
	 * with a physical pktbuf; put the MDSS SMMU in bypass so the DSI DMA
	 * reads the actual address instead of translating it. */
	stage("smmu bypass");
	plog_hex("SMMU sCR0 pre", rd32(0x00d00000UL));
	wr32(0x00d00000UL, rd32(0x00d00000UL) | 1U); dsb();
	plog_hex("SMMU sCR0 post", rd32(0x00d00000UL));
	plog_commit();

	/* Run the real bring-up: PHY, PLL, controller, panel on-commands,
	 * then push the splash through DSI DMA.  The diagnostic path is
	 * retained in dsi_write_test() but is no longer called here. */
	stage("start");
	plog_hex("bitrate", BITRATE);
	plog_hex("vco", VCO_RATE);
	plog_commit();

	if (calc_timing(bitrate, 19200000U)) { stage("timing failed"); return -1; }
	plog_hex("clk_prepare", tm.clk_prepare);
	plog_hex("hs_zero", tm.hs_zero);
	plog_commit();

	stage("phy enable");
	phy_enable();
	pll_calc(VCO_RATE, &pc);
	plog_hex("dec_start", pc.dec_start);
	plog_hex("div_frac", pc.div_frac_start);
	plog_commit();
	stage("pll program");
	pll_program(&pc);
	stage("pll start");
	if (pll_start()) { stage("pll failed"); return -1; }
	stage("pll locked");

	/* Point MMCC byte0 at the DSI PLL and divide by two.
	 * QCOM RCG: 0x0 = CMD (write 1 to apply), 0x4 = CFG. */
	wr32(MMCC_BYTE0_CMD, BYTE0_SRC_DSI0 | BYTE0_DIV_2);
	dsb();
	wr32(MMCC_BYTE0_CFG, 1);
	dsb();
	plog_hex("byte0 cfg", rd32(MMCC_BYTE0_CMD));
	plog_hex("byte0 cmd", rd32(MMCC_BYTE0_CFG));
	plog_commit();

	/* Controller: clocks on, soft reset, command mode, 4 lanes. */
	stage("controller reset");
	wr32(d + DSI_CTRL, 0);
	wr32(d + DSI_CLK_CTRL, CLK_CTRL_ALL);
	dsb();
	wr32(d + DSI_RESET, 1);
	dsb();
	mdelay(20);
	wr32(d + DSI_RESET, 0);
	dsb();
	mdelay(1);
	/* The soft reset gates the controller clocks again; re-enable them
	 * before touching any other DSI register. */
	wr32(d + DSI_CLK_CTRL, CLK_CTRL_ALL);
	dsb();
	mdelay(1);
	plog_hex("DSI CLKCTRL post-reset", rd32(d + DSI_CLK_CTRL));
	plog_commit();
	/* Software DMA trigger only; BLOCK_DMA_WITHIN_FRAME hangs without TE. */
	wr32(d + DSI_TRIG_CTRL, 4);
	/* Command-mode destination format and DCS memory-write opcodes,
	 * as mainline dsi_ctrl_setup() programs them. */
	wr32(d + DSI_CMD_CFG0, 8);		/* RGB888 */
	wr32(d + DSI_CMD_CFG1, 0x2cU | (0x3cU << 8) | (1U << 16));
	wr32(d + 0x00c0, 0x1b2bU);	/* t_clk_pre=0x2b, t_clk_post=0x1b (DT) */
	wr32(d + DSI_LANE_CTRL, 0);
	wr32(d + DSI_CTRL, CTRL_ENABLE | CTRL_CMD_MODE_EN | CTRL_LANES |
			   CTRL_CLK_EN | CTRL_ECC_CHECK | CTRL_CRC_CHECK);
	dsb();
	mdelay(1);
	plog_hex("DSI CTRL", rd32(d + DSI_CTRL));
	plog_hex("DSI STATUS", rd32(d + DSI_STATUS0));
	plog_hex("DSI CLKCTRL", rd32(d + DSI_CLK_CTRL));
	plog_hex("DSI TRIGCTRL", rd32(d + DSI_TRIG_CTRL));
	plog_hex("DSI RESET", rd32(d + DSI_RESET));
	plog_hex("PHY CTRL0", rd8(PHY_CMN + CMN_CTRL_0));
	plog_hex("PHY CLKCFG1", rd8(PHY_CMN + CMN_CLK_CFG1));
	plog_hex("PLL status", rd8(PHY_PLL + PLL_RESET_SM_READY_STATUS));
	plog_commit();

	panel_reset();
	if (panel_init()) return -1;
	if (push_frame()) return -1;
	stage("done");
	return 0;
}
