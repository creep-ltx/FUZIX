/*
 *	Amiga Rigid Disk Block partitions (RKM Devices, "SCSI device").
 *
 *	The drive's partition table lives in one of its first 16 blocks
 *	(RDSK), with a chain of PART blocks. Each partition's geometry is in
 *	its DosEnvec: start = LowCyl x Surfaces x BlocksPerTrack blocks of
 *	SizeBlock longs. Partitions are numbered in the order of the chain,
 *	so hdb1 is the first PART block: the same order HDToolBox shows.
 *
 *	Fuzix's own partitions have DosType FUZ\0: the Fuzix file system,
 *	then in the last 256 blocks the kernel for the Amiga hard disk
 *	loader (hdboot). Every partition is the whole partition, for Fuzix
 *	and for tools such as amigaread on AmigaOS partitions.
 *
 *	Checked after the MBR: a disk with a PC partition table keeps it.
 */
#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <blkdev.h>
#include "rdb.h"

#ifdef CONFIG_RDB

#define ID_RDSK		0x5244534BUL
#define ID_PART		0x50415254UL
#define NONE		0xFFFFFFFFUL

/* DosType of each partition found, per drive (for picking the root) */
uint32_t rdb_dostype[MAX_BLKDEV][MAX_PARTITIONS];

static uint32_t g32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint8_t read_block(uint32_t n, uint8_t *buf)
{
	blk_op.is_read = true;
	blk_op.is_user = false;
	blk_op.addr = buf;
	blk_op.lba = n;
	blk_op.nblock = 1;
	return blk_op.blkdev->transfer();
}

/* A block with this id whose SummedLongs longs add up to 0 */
static uint8_t valid(const uint8_t *b, uint32_t id)
{
	uint32_t n, sum = 0, i;

	if (g32(b) != id)
		return 0;
	n = g32(b + 4);
	if (n < 2 || n > 128)
		return 0;
	for (i = 0; i < n; i++)
		sum += g32(b + 4 * i);
	return sum == 0;
}

/* Returns 1 if the drive has an RDB (its partitions are then set up) */
uint_fast8_t rdb_parse(uint_fast8_t letter)
{
	blkdev_t *blk = blk_op.blkdev;
	uint8_t drive = letter - 'a';
	uint8_t *b = tmpbuf();
	uint32_t n, next, start, count, unit, *env;
	uint8_t found = 0, part = 0, steps = 0, i;
	char name[32];

	for (n = 0; n < 16; n++) {
		if (!read_block(n, b))
			break;
		if (valid(b, ID_RDSK)) {
			found = 1;
			break;
		}
	}
	if (!found || g32(b + 16) != 512) {	/* 512-byte blocks only */
		tmpfree(b);
		return 0;
	}
	memset(rdb_dostype[drive], 0, sizeof(rdb_dostype[drive]));
	next = g32(b + 28);			/* rdb_PartitionList */
	while (next != NONE && part < MAX_PARTITIONS && steps++ < 64) {
		if (!read_block(next, b) || !valid(b, ID_PART))
			break;
		next = g32(b + 16);
		env = (uint32_t *)(b + 128);	/* DosEnvec, aligned */
		/* blocks of SizeBlock longs, in 512-byte blocks. Every
		   PART block takes a number, so hdbN is always the Nth; one
		   that cannot be used gets no blocks */
		unit = env[1] / 128;
		start = count = 0;
		if (unit && env[4] == 1 && env[10] >= env[9]) {
			start = env[9] * env[3] * env[5] * unit;
			count = (env[10] - env[9] + 1) * env[3] * env[5] * unit;
		}
		rdb_dostype[drive][part] = env[0] >= 16 ? env[16] : 0;
		blk->lba_first[part] = start;
		blk->lba_count[part] = count;
		part++;
		/* BCPL name: length, then the characters */
		i = b[36];
		if (i > 30)
			i = 30;
		memcpy(name, b + 37, i);
		name[i] = 0;
		kprintf("hd%c%d (%s) ", letter, part, name);
	}
	tmpfree(b);
	return 1;
}

/* The first Fuzix partition of a drive, as a minor number, or 0 */
uint16_t rdb_fuzix_minor(uint_fast8_t drive)
{
	uint8_t i;

	for (i = 0; i < MAX_PARTITIONS; i++)
		if (rdb_dostype[drive][i] == RDB_FUZIX)
			return (drive << 4) | (i + 1);
	return 0;
}

#endif
