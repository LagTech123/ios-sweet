/*
 * Persistent log for the kagura shim.
 *
 * The MSM8996 ramoops console zone lives at 0xa7fbe000.  The previous
 * xztest image used the same format: a 12-byte header followed by text,
 * with the DBGC magic committed last.  TWRP exposes the result as
 * /sys/fs/pstore/console-ramoops after the shim resets.
 */
#include <stdint.h>
#include "plog.h"

#define PLOG_ZONE	0xa7fbe000UL
#define PLOG_SIZE	0x40000UL
#define PLOG_MAGIC	0x43474244U	/* "DBGC" */

static uint32_t plog_len;

static void pc(char c)
{
	if (plog_len < PLOG_SIZE - 12)
		*(volatile uint8_t *)(PLOG_ZONE + 12 + plog_len++) = (uint8_t)c;
}

void plog_str(const char *s)
{
	while (*s)
		pc(*s++);
}

void plog_hex(const char *key, uint64_t value)
{
	static const char digits[] = "0123456789abcdef";

	plog_str(key);
	pc('=');
	for (int shift = 60; shift >= 0; shift -= 4)
		pc(digits[(value >> shift) & 0xf]);
	pc('\n');
}

void plog_commit(void)
{
	*(volatile uint32_t *)(PLOG_ZONE + 0) = 0;
	*(volatile uint32_t *)(PLOG_ZONE + 4) = 0;
	*(volatile uint32_t *)(PLOG_ZONE + 8) = plog_len;
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(PLOG_ZONE + 0) = PLOG_MAGIC;
	__asm__ volatile("dsb sy" ::: "memory");
}

void plog_init(void)
{
	plog_len = 0;
	plog_commit();
}
