/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 *
 * A KAG2TWRP payload is handled differently: it carries a Linux kernel,
 * initramfs and DTB, and is launched through the safe-memory trampoline in
 * chain.S because the kernel destination overlaps this shim.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "plog.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* ---- KAG2TWRP chainload payload ---- */

struct chain_payload {
	char magic[8];
	uint32_t version, flags;
	uint64_t kernel_len, ramdisk_len, dtb_len;
	uint64_t kernel_load, ramdisk_load, dtb_load, kernel_entry;
};

struct chain_cfg {
	uint64_t ksrc, kdst, ksize;
	uint64_t rsrc, rdstd, rsize;
	uint64_t dsrc, ddst, dsize;
	uint64_t entry, dtb_arg;
};

_Static_assert(sizeof(struct chain_payload) == 72, "chain payload layout");
_Static_assert(sizeof(struct chain_cfg) == 88, "chain cfg layout");

/* Outside the kernel (ends ~0x81c6a00), dtb (0x81e00000) and ramdisk
 * (0x82200000) destination ranges. */
#define CHAIN_TRAMP_PA	0x81d00000UL
#define MSM_WDT		0x09830000UL

extern void chain_launch(uintptr_t dst, const void *cfg);
static int cmp8(const char *a, const char *b);
static int stored_gzip_hdr(const uint8_t *p);

/* Called before the watchdog is armed: a chainload must not be reset 40 s
 * later by the secondary CPU that guards the XNU first-boot attempt. */
int loader_chain_payload_present(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return 0;
	static const char chain_magic[] = "KAG2TWRP";
	if (cmp8((const char *)rd_start, chain_magic))
		return 1;
	return stored_gzip_hdr((const uint8_t *)rd_start);
}

static int cmp8(const char *a, const char *b)
{
	for (int i = 0; i < 8; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static uint64_t round8(uint64_t n)
{
	return (n + 7) & ~7ULL;
}

/* Our KAG2TWRP ramdisk is gzip with stored (uncompressed) deflate blocks,
 * so it can be unpacked in place with a tiny parser.  ABL may either copy it
 * verbatim or gunzip it before we see it; both cases are handled. */
static int stored_gzip_hdr(const uint8_t *p)
{
	return p[0] == 0x1f && p[1] == 0x8b && p[2] == 8 &&
	       (p[3] & 0xe0) == 0 && p[9] == 3;	/* OS = Unix: our encoder */
}

static uint64_t unpack_stored_gzip(uintptr_t buf, uint64_t cap)
{
	const uint8_t *s = (const uint8_t *)buf;
	uint8_t *dst = (uint8_t *)buf;
	const uint8_t *end = (const uint8_t *)(buf + cap);

	if (cap < 18 || !stored_gzip_hdr(s))
		return 0;
	s += 10;
	for (;;) {
		unsigned final, len, nlen;
		if (s >= end)
			return 0;
		final = *s++;
		if (s + 4 > end)
			return 0;
		len = (unsigned)s[0] | ((unsigned)s[1] << 8);
		nlen = (unsigned)s[2] | ((unsigned)s[3] << 8);
		s += 4;
		if ((len ^ 0xffffU) != nlen || s + len > end || dst + len > end)
			return 0;
		for (unsigned i = 0; i < len; i++)
			*dst++ = *s++;
		if (final)
			break;
	}
	return (uint64_t)(dst - buf);
}

static const char *chain_boot(uint64_t rd_start, uint64_t rd_end)
{
	const struct chain_payload *p = (const struct chain_payload *)rd_start;
	uint64_t need = sizeof(*p) + round8(p->kernel_len) +
			round8(p->ramdisk_len) + round8(p->dtb_len);

	plog_str("LOADER KAG2TWRP\n");
	plog_hex("rd.lo", rd_start);
	plog_hex("rd.hi", rd_end);
	plog_hex("version", p->version);
	plog_hex("kern.len", p->kernel_len);
	plog_hex("rd.len", p->ramdisk_len);
	plog_hex("dtb.len", p->dtb_len);
	plog_hex("need", need);
	plog_commit();

	if (p->version != 1 || rd_end <= rd_start || rd_end - rd_start < need) {
		plog_str("LOADER bad chain header\n");
		plog_commit();
		return "bad KAG2TWRP header";
	}

	/* Never let a stale firmware/earlier watchdog reset the kernel we
	 * are about to start. */
	*(volatile uint32_t *)(MSM_WDT + 0x08) = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	struct chain_cfg cfg;
	cfg.ksrc = rd_start + sizeof(*p);
	cfg.kdst = p->kernel_load;
	cfg.ksize = round8(p->kernel_len);
	cfg.rsrc = cfg.ksrc + cfg.ksize;
	cfg.rdstd = p->ramdisk_load;
	cfg.rsize = round8(p->ramdisk_len);
	cfg.dsrc = cfg.rsrc + cfg.rsize;
	cfg.ddst = p->dtb_load;
	cfg.dsize = round8(p->dtb_len);
	cfg.entry = p->kernel_entry;
	cfg.dtb_arg = p->dtb_load;

	plog_hex("k.src", cfg.ksrc);
	plog_hex("k.dst", cfg.kdst);
	plog_hex("k.size", cfg.ksize);
	plog_hex("r.src", cfg.rsrc);
	plog_hex("r.dst", cfg.rdstd);
	plog_hex("r.size", cfg.rsize);
	plog_hex("d.src", cfg.dsrc);
	plog_hex("d.dst", cfg.ddst);
	plog_hex("d.size", cfg.dsize);
	plog_hex("entry", cfg.entry);
	plog_hex("tramp", CHAIN_TRAMP_PA);
	plog_str("LOADER chainlaunch\n");
	plog_commit();

	chain_launch(CHAIN_TRAMP_PA, &cfg);
	return "chain returned";
}

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	plog_str("LOADER entry\n");
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end)) {
		plog_str("LOADER no initrd\n");
		plog_commit();
		return "no initrd in /chosen";
	}
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);
	plog_hex("initrd.lo", rd_start);
	plog_hex("initrd.hi", rd_end);
	plog_hex("rd.first", *(volatile uint64_t *)rd_start);
	plog_hex("rd.next", *(volatile uint64_t *)(rd_start + 8));
	plog_commit();

	static const char chain_magic[] = "KAG2TWRP";
	if (!cmp8((const char *)rd_start, chain_magic) &&
	    stored_gzip_hdr((const uint8_t *)rd_start)) {
		uint64_t n = unpack_stored_gzip(rd_start,
						rd_end - rd_start + 16384);
		plog_hex("gunzip", n);
		plog_commit();
		if (n)
			rd_end = rd_start + n;
	}
	if (cmp8((const char *)rd_start, chain_magic))
		return chain_boot(rd_start, rd_end);

	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic)) {
		plog_str("LOADER no SWEETXNU\n");
		plog_commit();
		return "no SWEETXNU payload";
	}
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7)) {
		plog_hex("LOADER bad header version", p->version);
		plog_commit();
		return "bad payload header";
	}
	plog_str("LOADER payload ok\n");

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size) {
		plog_str("LOADER ramdisk overlap\n");
		plog_commit();
		return "ramdisk overlaps kernel memory";
	}

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		plog_hex("seg.dst", dst);
		plog_hex("seg.size", s->file_size);
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	plog_commit();
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	plog_hex("XNU entry", entry_pa);
	plog_str("LOADER jumping\n");
	plog_commit();
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
