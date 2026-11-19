/*
 * MSM8996 multimedia clock/power and display-state probe.
 *
 * MMCC (0x008c0000) is reachable at shim entry and the mdss GDSCR reports the
 * domain powered, so the MDSS register blocks may be read.  Each block dump is
 * committed separately: if one access ever stalls the interconnect the pstore
 * log still shows how far we got.
 *
 * Offsets:
 *   MMCC/GDSCR  mainline drivers/clk/qcom/mmcc-msm8996.c
 *   MDP5        mainline drivers/gpu/drm/msm/disp/mdp5/mdp5.xml.h (v5.10)
 *   DSI 6G      mainline drivers/gpu/drm/msm/dsi/dsi.xml.h
 * Block bases come from the kagura DT (ctl 0x2000, dma 0x25000, LM 0x45000,
 * intf 0x6b000) and are relative to the MDP base.
 */
#include <stdint.h>
#include "cc.h"
#include "plog.h"

#define MMCC_BASE	0x008c0000UL
#define MDP_BASE	0x00900000UL
#define DSI0_BASE	0x00994000UL
#define DSI0_PHY	0x00994400UL

#define GDSC_COLLAPSE	0x1U
#define GDSC_PWR_ON	0x80000000U

#define MDP_HW_VERSION	0x0000UL

#define CTL0_BASE	(MDP_BASE + 0x2000UL)
#define DMA0_BASE	(MDP_BASE + 0x25000UL)
#define LM0_BASE	(MDP_BASE + 0x45000UL)
#define INTF0_BASE	(MDP_BASE + 0x6b000UL)

static inline uint32_t rd(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

/* Commit before and after every read: if one of these accesses stalls the
 * interconnect, the pstore log still names it. */
static void show(const char *name, uintptr_t a)
{
	plog_str("rd ");
	plog_str(name);
	plog_str("\n");
	plog_commit();
	plog_hex(name, rd(a));
	plog_commit();
}

/* Log "<tag>+<offset>" = value without needing a string table. */
static void peek(const char *tag, uintptr_t a)
{
	static const char hex[] = "0123456789abcdef";
	char name[24];
	int n = 0;

	while (tag[n] && n < 16) {
		name[n] = tag[n];
		n++;
	}
	name[n++] = '+';
	name[n++] = hex[(a >> 12) & 0xf];
	name[n++] = hex[(a >> 8) & 0xf];
	name[n++] = hex[(a >> 4) & 0xf];
	name[n++] = hex[a & 0xf];
	name[n] = 0;
	plog_hex(name, rd(a));
}

static void dump_block(const char *tag, uintptr_t base, int from, int to)
{
	for (int o = from; o <= to; o += 4)
		peek(tag, base + (uintptr_t)o);
	plog_commit();
}

void cc_probe(void)
{
	plog_str("--- mmcc state ---\n");

	/* Parent domain first: mdss_gdsc cannot be on while this one is off. */
	show("MMCC mmagic gdscr", MMCC_BASE + 0x247c);
	show("MMCC mmagic axi", MMCC_BASE + 0x2474);

	/* MDSS power domain and its clock branches. */
	show("MMCC mdss gdscr", MMCC_BASE + 0x2304);
	show("MMCC mdss ahb", MMCC_BASE + 0x2308);
	show("MMCC mdss axi", MMCC_BASE + 0x2310);
	show("MMCC mdss mdp", MMCC_BASE + 0x231c);
	show("MMCC mdss byte0", MMCC_BASE + 0x233c);
	show("MMCC mdss esc0", MMCC_BASE + 0x2344);

	if (!(rd(MMCC_BASE + 0x2304) & GDSC_PWR_ON)) {
		plog_str("mdss domain off: MDP/DSI skipped\n");
		plog_commit();
		return;
	}

	plog_str("mdss domain on: reading MDP/DSI\n");
	plog_commit();
	show("MDP hw version", MDP_BASE + MDP_HW_VERSION);

	/* CTL0: layer0 config, op mode, flush, start. */
	dump_block("CTL0", CTL0_BASE, 0x00, 0x1c);
	/* DMA0: source address/stride/format and the live source address. */
	dump_block("DMA0", DMA0_BASE, 0x10, 0x24);
	dump_block("DMAc", DMA0_BASE, 0xa0, 0xac);
	/* Layer mixer 0: output size and operating mode. */
	dump_block("LM0", LM0_BASE, 0x00, 0x1c);
	/* Interface 0: timing engine enable, config, type. */
	dump_block("INTF0", INTF0_BASE, 0x00, 0x1c);
	/* DSI0 controller: CTRL, STATUS0, FIFO status, clock control. */
	dump_block("DSI0", DSI0_BASE, 0x00, 0x1c);
	/* DSI0 PHY/PLL block. */
	dump_block("PHY0", DSI0_PHY, 0x00, 0x1c);

	plog_str("--- end mmcc state ---\n");
	plog_commit();
	(void)GDSC_COLLAPSE;
}
