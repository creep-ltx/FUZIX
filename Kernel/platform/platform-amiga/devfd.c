/*
 *	Amiga floppy: the internal drive (DF0), 880K double density disks in
 *	the AmigaDOS track format (11 sectors of 512 bytes, MFM), driven
 *	directly through Paula's disk DMA.
 *
 *	A whole track is read in one revolution and decoded into a track
 *	cache, so the other ten sectors of a track cost nothing. A write
 *	updates the cache and writes the whole track back at once (the
 *	Amiga format has no per-sector write).
 *
 *	The drive appears as an ordinary block device (hda, hdb...); MBR
 *	partitions on it are found the usual way. The boot floppy carries
 *	one in its bootblock when it holds a Fuzix file system.
 */
#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <timer.h>
#include <blkdev.h>
#include "amiga.h"
#include "devfd.h"

#define CIAA_PRA	(*(volatile uint8_t *)0xBFE001)
#define CIAB_PRB	(*(volatile uint8_t *)0xBFD100)
#define CIAB_DDRB	(*(volatile uint8_t *)0xBFD300)

/* CIA-A port A inputs, active low */
#define PRA_RDY		0x20
#define PRA_TK0		0x10
#define PRA_WPRO	0x08
#define PRA_CHNG	0x04

/* CIA-B port B outputs, active low */
#define PRB_MTR		0x80
#define PRB_SEL0	0x08
#define PRB_SIDE	0x04	/* low = upper head (side 1) */
#define PRB_DIR		0x02	/* high = out towards track 0 */
#define PRB_STEP	0x01

#define DSKPTH		CUSTOM(0x020)
#define DSKPTL		CUSTOM(0x022)
#define DSKLEN		CUSTOM(0x024)
#define DSKSYNC		CUSTOM(0x07E)
#define ADKCON		CUSTOM(0x09E)

#define INTF_DSKBLK	0x0002

#define SECTORS		11
#define CYLS		80
#define TRACKS		(CYLS * 2)
#define SYNC		0x4489
#define M		0x55555555UL

/* A sector on disk: 2 gap words, 2 sync words, then 270 longs */
#define SECTOR_WORDS	544
/* Read a little more than one revolution (as LDOS does) */
#define READ_WORDS	6944
/* Gap written before sector 0; the end of the write may run over its
   start, which is harmless */
#define GAP_WORDS	300
#define WRITE_WORDS	(GAP_WORDS + SECTORS * SECTOR_WORDS + 1)

/* Kernel is in chip RAM, so this is DMA-able */
static uint16_t mfm[READ_WORDS];

static uint8_t trackcache[SECTORS * 512];
static int16_t cache_track = -1;
static int16_t cur_cyl = -1;
static uint8_t motor_on;

/* Ticks left until the motor goes off; 0 = no timer running */
static volatile uint8_t motor_ticks;

/* Wait n times 64us: PAL/NTSC scan lines, two at 31kHz */
static void wait_lines(uint16_t n)
{
	volatile uint8_t *vpos = (volatile uint8_t *)0xDFF006;
	uint8_t c;

	n = (n * lines_per_ms + 15) / 16;
	while (n--) {
		c = *vpos;
		while (*vpos == c)
			;
	}
}

#define wait_ms(n)	wait_lines((n) * 16)

static void fd_motor_on(void)
{
	uint16_t i;

	motor_ticks = 0;
	if (motor_on)
		return;
	CIAB_PRB |= PRB_SEL0;		/* deselect */
	CIAB_PRB &= ~PRB_MTR;		/* motor on ... */
	CIAB_PRB &= ~PRB_SEL0;		/* ... latched by selecting */
	/* Ready within half a second */
	for (i = 0; i < 500 && (CIAA_PRA & PRA_RDY); i++)
		wait_ms(1);
	motor_on = 1;
}

static void fd_motor_off(void)
{
	CIAB_PRB |= PRB_SEL0;
	CIAB_PRB |= PRB_MTR;
	CIAB_PRB &= ~PRB_SEL0;
	CIAB_PRB |= PRB_SEL0;
	motor_on = 0;
}

/* Called every tick: the motor goes off two seconds after the last use */
void fd_timer(void)
{
	if (motor_ticks && --motor_ticks == 0)
		fd_motor_off();
}

static void fd_done(void)
{
	motor_ticks = TICKSPERSEC * 2;
}

static void step(void)
{
	CIAB_PRB &= ~PRB_STEP;
	wait_lines(1);
	CIAB_PRB |= PRB_STEP;
	wait_ms(3);
}

static int recalibrate(void)
{
	uint8_t n;

	CIAB_PRB |= PRB_DIR;		/* out */
	for (n = 0; n < CYLS + 5; n++) {
		if (!(CIAA_PRA & PRA_TK0)) {
			cur_cyl = 0;
			wait_ms(18);
			return 0;
		}
		step();
	}
	return -1;
}

static int seek(uint8_t track)
{
	uint8_t cyl = track >> 1;

	if (cur_cyl < 0 && recalibrate())
		return -1;
	if (cyl != cur_cyl) {
		if (cyl > cur_cyl)
			CIAB_PRB &= ~PRB_DIR;	/* in */
		else
			CIAB_PRB |= PRB_DIR;
		while (cur_cyl != cyl) {
			step();
			cur_cyl += (cyl > cur_cyl) ? 1 : -1;
		}
		wait_ms(18);			/* settle */
	}
	if (track & 1)
		CIAB_PRB &= ~PRB_SIDE;		/* upper head */
	else
		CIAB_PRB |= PRB_SIDE;
	wait_lines(2);
	return 0;
}

/* Run one disk DMA transfer and wait for it (up to a second) */
static int disk_dma(uint16_t len)
{
	uint16_t t = 16000;

	DSKLEN = 0x4000;		/* stop anything left running */
	DSKPTH = (uint32_t)mfm >> 16;
	DSKPTL = (uint32_t)mfm & 0xFFFF;
	INTREQ = INTF_DSKBLK;
	DMACON = 0x8210;		/* SET | DMAEN | DSKEN */
	DSKLEN = len;
	DSKLEN = len;			/* twice to start */
	while (!(INTREQR & INTF_DSKBLK)) {
		wait_lines(1);
		if (--t == 0)
			break;
	}
	DSKLEN = 0x4000;
	INTREQ = INTF_DSKBLK;
	return t ? 0 : -1;
}

static uint32_t get32(const uint16_t *p)
{
	return ((uint32_t)p[0] << 16) | p[1];
}

/* Decode every good sector of the track into the cache; returns a bit
   per sector found */
static uint16_t decode_track(uint8_t track)
{
	uint16_t *p = mfm;
	uint16_t *end = mfm + READ_WORDS - (SECTOR_WORDS - 4);
	uint16_t found = 0;
	uint32_t info, sum, stored;
	uint8_t sec, i;
	uint16_t j;

	while (p < end) {
		if (*p != SYNC) {
			p++;
			continue;
		}
		while (p < end && *p == SYNC)
			p++;
		/* Header: info, label, header sum (longs 0-11) */
		sum = 0;
		for (i = 0; i < 10; i++)
			sum ^= get32(p + 2 * i);
		sum &= M;
		stored = ((get32(p + 20) & M) << 1) | (get32(p + 22) & M);
		if (sum != stored)
			continue;
		info = ((get32(p) & M) << 1) | (get32(p + 2) & M);
		sec = info >> 8;
		if ((uint8_t)(info >> 16) != track || sec >= SECTORS)
			continue;
		/* Data: odd longs, then even longs */
		sum = 0;
		for (j = 0; j < 256; j++)
			sum ^= get32(p + 28 + 2 * j);
		sum &= M;
		stored = ((get32(p + 24) & M) << 1) | (get32(p + 26) & M);
		if (sum == stored) {
			uint32_t *d = (uint32_t *)(trackcache + sec * 512);
			for (j = 0; j < 128; j++)
				*d++ = ((get32(p + 28 + 2 * j) & M) << 1) |
					(get32(p + 28 + 256 + 2 * j) & M);
			found |= 1 << sec;
		}
		p += SECTOR_WORDS - 4;
	}
	return found;
}

static int read_track(uint8_t track)
{
	uint8_t tries;

	if (track == cache_track)
		return 0;
	cache_track = -1;
	for (tries = 0; tries < 4; tries++) {
		if (seek(track))
			return -1;
		ADKCON = 0x6600;	/* precomp, word and MSB sync off */
		ADKCON = 0x9500;	/* MFM, word sync, fast */
		DSKSYNC = SYNC;
		if (disk_dma(0x8000 | READ_WORDS) == 0 &&
		    decode_track(track) == (1 << SECTORS) - 1) {
			cache_track = track;
			return 0;
		}
		if (tries == 1)
			cur_cyl = -1;	/* recalibrate and try again */
	}
	kprintf("fd: cannot read track %d\n", track);
	return -1;
}

/*
 *	MFM encoding: data bits sit in the 0x55555555 positions, and a clock
 *	bit is set where both neighbouring data bits are 0.
 */
static uint16_t *wp;
static uint32_t lastbit;

static void put16(uint16_t w)
{
	*wp++ = w;
	lastbit = w & 1;
}

static void put_mfm(uint32_t d)
{
	uint32_t v = d | (~((d >> 1) | (lastbit << 31) | (d << 1)) & 0xAAAAAAAAUL);

	*wp++ = v >> 16;
	*wp++ = v;
	lastbit = v & 1;
}

static void encode_sector(uint8_t track, uint8_t sec)
{
	uint32_t *d = (uint32_t *)(trackcache + sec * 512);
	uint32_t info = 0xFF000000UL | ((uint32_t)track << 16) |
			((uint32_t)sec << 8) | (SECTORS - sec);
	uint32_t sum;
	uint8_t i;
	uint16_t j;

	put16(0xAAAA);
	put16(0xAAAA);
	put16(SYNC);
	put16(SYNC);
	put_mfm((info >> 1) & M);
	put_mfm(info & M);
	for (i = 0; i < 8; i++)		/* label: zeros */
		put_mfm(0);
	sum = ((info >> 1) & M) ^ (info & M);
	put_mfm((sum >> 1) & M);
	put_mfm(sum & M);
	sum = 0;
	for (j = 0; j < 128; j++)
		sum ^= ((d[j] >> 1) & M) ^ (d[j] & M);
	put_mfm((sum >> 1) & M);
	put_mfm(sum & M);
	for (j = 0; j < 128; j++)
		put_mfm((d[j] >> 1) & M);
	for (j = 0; j < 128; j++)
		put_mfm(d[j] & M);
}

static int write_track(uint8_t track)
{
	uint16_t i;
	uint8_t sec;

	if (!(CIAA_PRA & PRA_WPRO)) {
		kputs("fd: disk is write protected\n");
		return -1;
	}
	if (seek(track))
		return -1;
	wp = mfm;
	lastbit = 0;
	for (i = 0; i < GAP_WORDS; i++)
		put16(0xAAAA);
	for (sec = 0; sec < SECTORS; sec++)
		encode_sector(track, sec);
	put16(0xAAAA);

	ADKCON = 0x6600;
	/* MFM, fast; write precompensation on the inner half */
	ADKCON = 0x9100 | ((track >> 1) >= CYLS / 2 ? 0x2000 : 0);
	if (disk_dma(0xC000 | WRITE_WORDS)) {
		cache_track = -1;
		return -1;
	}
	wait_ms(2);
	return 0;
}

/* The change line stays low while there is no disk, even after a step
   (a step clears it once a disk is in). Step in and back out. */
static uint8_t disk_in(void)
{
	if (recalibrate())
		return 0;
	CIAB_PRB &= ~PRB_DIR;
	step();
	CIAB_PRB |= PRB_DIR;
	step();
	wait_ms(18);
	return (CIAA_PRA & PRA_CHNG) ? 1 : 0;
}

static uint_fast8_t fd_transfer(void)
{
	uint32_t lba = blk_op.lba;
	uint8_t track = lba / SECTORS;
	uint8_t sec = lba % SECTORS;
	int r;

	if (lba >= TRACKS * SECTORS)
		return 0;
	fd_motor_on();
	/* A changed disk: forget what we know about it */
	if (!(CIAA_PRA & PRA_CHNG)) {
		cache_track = -1;
		cur_cyl = -1;
		if (!disk_in()) {
			fd_done();
			udata.u_error = ENXIO;
			return 0;
		}
	}
	r = read_track(track);
	if (r == 0) {
		if (blk_op.is_read)
			memcpy(blk_op.addr, trackcache + sec * 512, 512);
		else {
			memcpy(trackcache + sec * 512, blk_op.addr, 512);
			r = write_track(track);
		}
	}
	fd_done();
	return r ? 0 : 1;
}

static int fd_flush(void)
{
	return 0;
}

/*
 *	Returns the block device, or NULL if there is no DF0. The drive is
 *	registered with or without a disk in it, so a disk can be put in
 *	later (the install disks, the Extras disks); its partitions are only
 *	read when a disk is in at boot.
 */
blkdev_t *fd_init(void)
{
	blkdev_t *blk;
	uint8_t in;

	CIAB_DDRB = 0xFF;
	CIAB_PRB = 0xFF;		/* nothing selected, motor off */
	/* Kickstart's trackdisk may have left a transfer set up */
	DSKLEN = 0x4000;
	ADKCON = 0x7F00;
	INTREQ = INTF_DSKBLK;
	kputs("Floppy DF0: ");
	fd_motor_on();
	if (recalibrate()) {
		kputs("no drive\n");
		fd_motor_off();
		return NULL;
	}
	in = disk_in();
	blk = blkdev_alloc();
	if (blk == NULL) {
		fd_motor_off();
		return NULL;
	}
	blk->transfer = fd_transfer;
	blk->flush = fd_flush;
	blk->driver_data = 0;
	blk->drive_lba_count = TRACKS * SECTORS;
	if (in)
		blkdev_scan(blk, SWAPSCAN);
	else
		kputs("hda: no disk\n");
	fd_done();
	return blk;
}
