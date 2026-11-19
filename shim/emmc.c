/*
 * Kagura eMMC (SDCC1 / SDHCI at 0x07464900) read-only probe for the bare-metal
 * shim. Sony ABL has already initialized the controller and selected the eMMC
 * user area; this code only does polled PIO (CMD17), never resets or writes.
 * Results go to pstore so they can be recovered with TWRP.
 */
#include <stdint.h>
#include "plog.h"
#include "console.h"
#include "emmc.h"

#define HC_BASE         0x07464900UL

#define BLK_SIZE        0x04u
#define BLK_COUNT       0x06u
#define ARGUMENT        0x08u
#define XFER_MODE       0x0Cu
#define COMMAND         0x0Eu
#define BUFFER          0x20u
#define PRES_STATE      0x24u
#define HOST_CTRL       0x28u
#define CLK_CTRL        0x2Cu
#define INT_STATUS      0x30u
#define INT_ENABLE      0x34u
#define SIGNAL_ENABLE   0x38u
#define CAPABILITIES    0x40u
#define HOST_VERSION    0xFEu

#define PRES_CMD_INH    0x00000001u
#define PRES_DATA_INH   0x00000002u
#define XFER_READ       0x10u
#define XFER_BLKCNT     0x02u
#define CMD_RSP_SHORT   0x02u
#define CMD_CRC         0x08u
#define CMD_IDX         0x10u
#define CMD_DATA        0x20u
#define INT_DATA_END    0x00000002u
#define INT_DATA_AVAIL  0x00000020u
#define INT_ERRORS      0xffff8000u
#define TIMEOUT         40000000u

static volatile uint8_t *const hc = (volatile uint8_t *)HC_BASE;
static uint8_t sector[512] __attribute__((aligned(8)));
static uint64_t first_lba, nsectors;

static inline uint8_t rd8(uint32_t o)  { return hc[o]; }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(hc + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(hc + o); }
static inline void wr8(uint32_t o, uint8_t v)   { hc[o] = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(hc + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(hc + o) = v; }

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static int read_sector(uint32_t lba)
{
	uint32_t enable = rd32(INT_ENABLE), signal = rd32(SIGNAL_ENABLE);
	uint32_t st;
	unsigned words = 0;

	for (unsigned i = 0; i < TIMEOUT; i++) {
		if (!(rd32(PRES_STATE) & (PRES_CMD_INH | PRES_DATA_INH))) {
			break;
		}
		if (i == TIMEOUT - 1) {
			return 7; /* EBUSY */
		}
	}
	wr32(INT_ENABLE, 0);
	wr32(SIGNAL_ENABLE, 0);
	wr32(INT_STATUS, rd32(INT_STATUS));

	wr16(BLK_SIZE, 512);
	wr16(BLK_COUNT, 1);
	wr32(ARGUMENT, lba);
	wr8(XFER_MODE, (uint8_t)(XFER_READ | XFER_BLKCNT));
	wr16(COMMAND, (uint16_t)((17u << 8) | CMD_RSP_SHORT | CMD_CRC |
	    CMD_IDX | CMD_DATA));

	for (unsigned i = 0; i < TIMEOUT; i++) {
		st = rd32(INT_STATUS);
		if (st & INT_ERRORS) {
			plog_hex("EMMC err int", st);
			wr32(INT_STATUS, rd32(INT_STATUS));
			wr32(INT_ENABLE, enable);
			wr32(SIGNAL_ENABLE, signal);
			return 5; /* EIO */
		}
		if ((st & INT_DATA_AVAIL) && !words) {
			uint32_t *out = (uint32_t *)sector;
			for (unsigned j = 0; j < 128; j++) {
				out[j] = rd32(BUFFER);
			}
			words = 128;
			wr32(INT_STATUS, INT_DATA_AVAIL);
		}
		if (words == 128 && (st & INT_DATA_END)) {
			break;
		}
		if (i == TIMEOUT - 1) {
			plog_hex("EMMC timeout int", st);
			wr32(INT_STATUS, rd32(INT_STATUS));
			wr32(INT_ENABLE, enable);
			wr32(SIGNAL_ENABLE, signal);
			return 6; /* ETIMEDOUT */
		}
	}
	wr32(INT_STATUS, rd32(INT_STATUS));
	wr32(INT_ENABLE, enable);
	wr32(SIGNAL_ENABLE, signal);
	return 0;
}

static int name_eq(const uint8_t *p, const char *want)
{
	unsigned k;
	for (k = 0; want[k]; k++) {
		if (p[56 + k * 2] != (uint8_t)want[k] || p[57 + k * 2] != 0) {
			return 0;
		}
	}
	return 1;
}

static int find_userdata(void)
{
	int error = read_sector(1);
	if (error) {
		return error;
	}
	plog_hex("EMMC gpt sig0", le64(sector));
	plog_hex("EMMC gpt sig1", le64(sector + 8));
	if (sector[0] != 'E' || sector[1] != 'F' || sector[2] != 'I' ||
	    sector[3] != ' ' || sector[4] != 'P' || sector[5] != 'A' ||
	    sector[6] != 'R' || sector[7] != 'T') {
		return 6; /* ENXIO */
	}
	uint64_t entries = le64(sector + 72);
	uint32_t nent = le32(sector + 80), esz = le32(sector + 84);
	plog_hex("EMMC gpt entries", (entries << 32) | nent);
	if (entries > 32 || nent == 0 || nent > 128 || esz != 128) {
		return 6;
	}
	for (uint32_t s = 0; s < (nent + 3) / 4; s++) {
		error = read_sector((uint32_t)entries + s);
		if (error) {
			return error;
		}
		for (uint32_t j = 0; j < 4; j++) {
			const uint8_t *e = sector + j * 128;
			if (name_eq(e, "userdata")) {
				first_lba = le64(e + 32);
				nsectors = le64(e + 40) - first_lba + 1;
				return 0;
			}
		}
	}
	return 6;
}

void emmc_probe(void)
{
	plog_str("EMMC probe\n");
	plog_hex("EMMC ver", rd16(HOST_VERSION));
	plog_hex("EMMC caps", rd32(CAPABILITIES));
	plog_hex("EMMC state", rd32(PRES_STATE));
	plog_hex("EMMC ctrl", rd8(HOST_CTRL));
	plog_hex("EMMC clk", rd16(CLK_CTRL));
	plog_commit();

	int error = find_userdata();
	if (!error) {
		plog_str("EMMC userdata OK\n");
		plog_hex("EMMC first", first_lba);
		plog_hex("EMMC sectors", nsectors);
		con_hex2("EMMC", "first=", (uint32_t)first_lba,
		    "sectors=", (uint32_t)nsectors);
	} else {
		plog_str("EMMC userdata FAIL\n");
		plog_hex("EMMC error", (uint64_t)(unsigned)error);
		con_hex("EMMC", (uint64_t)(unsigned)error);
	}
	plog_commit();
}
