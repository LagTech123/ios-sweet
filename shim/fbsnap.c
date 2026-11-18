/*
 * Decode the SWEET/XNU early framebuffer console into plain text in the
 * ramoops log before the kagura splash overwrites it.  The panel is dark at
 * bare-metal handoff, but RAM retains the pixels across a reset, so this is
 * the only practical way to see XNU's first console messages.
 */
#include <stdint.h>
#include "font.h"
#include "plog.h"

#define FB_BASE		0x83401000UL
#define FB_STRIDE_PX	1088u		/* 4352 bytes / 4 */
#define CELL_W		16u		/* 8 px glyph * EARLY_SCALE 2 */
#define CELL_H		32u		/* 16 px glyph * EARLY_SCALE 2 */
#define ROWS		59u		/* (1920 - 32) / CELL_H */
#define COLS		67u		/* 1080 / CELL_W */

static int pixel_on(unsigned int y, unsigned int x)
{
	volatile const uint32_t *fb = (volatile const uint32_t *)FB_BASE;
	uint32_t p = fb[y * FB_STRIDE_PX + x];
	return (p & 0x00ffffffu) > 0x00777777u;
}

static void read_glyph(unsigned int row, unsigned int col, uint8_t glyph[16])
{
	for (unsigned int gy = 0; gy < 16; gy++) {
		uint8_t bits = 0;
		for (unsigned int gx = 0; gx < 8; gx++)
			if (pixel_on(row * CELL_H + gy * 2, col * CELL_W + gx * 2))
				bits |= (uint8_t)(1u << gx);
		glyph[gy] = bits;
	}
}

static char decode_glyph(const uint8_t glyph[16])
{
	for (unsigned int i = 0; i < sizeof(font8x16) / sizeof(font8x16[0]); i++) {
		unsigned int j;
		for (j = 0; j < 16; j++)
			if (glyph[j] != font8x16[i][j])
				break;
		if (j == 16)
			return (char)(0x20 + i);
	}
	return '?';
}

void fbsnap_decode(void)
{
	char line[COLS + 2];

	plog_str("=== XNU EARLY FB CONSOLE ===\n");
	for (unsigned int row = 0; row < ROWS; row++) {
		int any = 0;
		for (unsigned int col = 0; col < COLS; col++) {
			uint8_t glyph[16];
			read_glyph(row, col, glyph);
			line[col] = decode_glyph(glyph);
			if (line[col] != ' ')
				any = 1;
		}
		if (!any)
			continue;
		int last = (int)COLS - 1;
		while (last >= 0 && line[last] == ' ')
			last--;
		line[last + 1] = '\n';
		line[last + 2] = 0;
		plog_str(line);
	}
	plog_str("=== END XNU EARLY FB CONSOLE ===\n");
	plog_commit();
}
