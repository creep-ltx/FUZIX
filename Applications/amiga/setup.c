/*
 *	setup - install Fuzix on an Amiga's hard disk, beside AmigaOS
 *
 *	setup		install from the install floppy onto the IDE disk
 *	setup extras	add the programs on the Extras floppies
 *	setup -boot	run by /etc/rc at every boot: offers the install when
 *			Fuzix runs from the install floppy and there is a
 *			hard disk, and offers the Extras disks once after
 *			the first boot from the hard disk
 *
 *	Fuzix lives in one partition of the disk's Amiga partition table (the
 *	Rigid Disk Block, which Kickstart and HDToolBox use), beside AmigaOS
 *	partitions. setup offers free space on the disk, an existing
 *	partition to replace, or the whole disk, and changes only the blocks
 *	of the table it needs:
 *
 *	- a FUZIX partition (PART block) with DosType FUZ\0: the Fuzix file
 *	  system, then in its last 256 blocks the kernel (a header block
 *	  "FUZK" + size, then the kernel). The whole partition stays within
 *	  32 MB: Fuzix reaches only that much of a device, and writes the
 *	  kernel through the partition (/dev/hdbN)
 *	- the "file system" for FUZ\0 (FSHD + LSEG blocks): the small loader
 *	  /usr/lib/fuzix/hdboot.hunk. Kickstart loads it at boot and starts
 *	  it when the FUZIX partition is booted; it reads the kernel through
 *	  AmigaOS's own disk driver and starts it
 *
 *	Which system starts by default is the partitions' boot priority; the
 *	other is picked in Kickstart's boot menu (both mouse buttons held at
 *	power on).
 *
 *	While installing, Fuzix runs from the floppy: the floppy is hda and
 *	the hard disk hdb; partition N of the table is hdbN.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <syscalls.h>

#define FLOPPY		"/dev/hda"
#define FLOPPY_ROOT	0x0001		/* hda1 */
#define DISK		"/dev/hdb"
#define LOADER		"/usr/lib/fuzix/hdboot.hunk"
#define KERNEL		"/usr/lib/fuzix/kernel"
#define EXTRAS_TODO	"/etc/extras.todo"

#define BLOCK		512
#define HOSTID		7
#define RESERVED	256		/* kernel blocks at the partition end */
#define MAXPART		65535		/* Fuzix block numbers are 16 bits */
#define MINFS		4096		/* 2 MB at least */
#define MINDISK		(64 + RESERVED + MINFS)
#define MAXFS		(MAXPART - RESERVED)
#define DOSTYPE		0x46555A00UL	/* 'FUZ\0' */
#define NONE		0xFFFFFFFFUL
#define ID_RDSK		0x5244534BUL
#define ID_PART		0x50415254UL
#define ID_FSHD		0x46534844UL
#define ID_LSEG		0x4C534547UL
#define ID_BADB		0x42414442UL
#define MAXPARTS	15
#define MAXTAB		512		/* blocks of the table we look at */

static uint32_t blk[BLOCK / 4];
static int disk = -1;
static int loader_fd = -1;		/* read 492 bytes at a time */
static uint32_t loader_len;

static void say(const char *s)
{
	fputs(s, stdout);
	fflush(stdout);
}

static int ask(const char *q, char *buf, int len)
{
	say(q);
	if (fgets(buf, len, stdin) == NULL)
		return -1;
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int yes(const char *q)
{
	char buf[16];

	if (ask(q, buf, sizeof(buf)))
		return 0;
	return buf[0] == 'y' || buf[0] == 'Y';
}

static int run(const char *cmd)
{
	int r = system(cmd);

	if (r)
		printf("setup: \"%s\" failed.\n", cmd);
	return r;
}

/* Block sizes from the kernel */
static uint32_t dev_blocks(const char *dev)
{
	uint32_t n = 0;
	int fd = open(dev, O_RDONLY);

	if (fd == -1)
		return 0;
	if (ioctl(fd, BLKGETSIZE, &n) == -1)
		n = 0;
	close(fd);
	return n;
}

static int get(uint32_t n, void *buf)
{
	if (lseek(disk, (off_t)n * BLOCK, SEEK_SET) == -1 ||
	    read(disk, buf, BLOCK) != BLOCK) {
		perror("setup: reading the hard disk");
		return -1;
	}
	return 0;
}

static int put(uint32_t n, const void *buf)
{
	if (lseek(disk, (off_t)n * BLOCK, SEEK_SET) == -1 ||
	    write(disk, buf, BLOCK) != BLOCK) {
		perror("setup: writing the hard disk");
		return -1;
	}
	return 0;
}

/*
 *	The Amiga partition table (RKM Devices, "SCSI device"): long 1 of
 *	every block is how many longs are checksummed, long 2 the checksum
 *	that makes them sum to zero.
 */
static void finish(uint32_t *b, uint16_t summed)
{
	uint32_t sum = 0;
	uint16_t i;

	b[1] = summed;
	b[2] = 0;
	for (i = 0; i < summed; i++)
		sum += b[i];
	b[2] = -sum;
}

static int valid(const uint32_t *b, uint32_t id)
{
	uint32_t sum = 0, i;

	if (b[0] != id || b[1] < 2 || b[1] > 128)
		return 0;
	for (i = 0; i < b[1]; i++)
		sum += b[i];
	return sum == 0;
}

/* What we know of the disk's table */
struct part {
	uint32_t at;			/* its PART block */
	uint32_t first, count;		/* 512-byte blocks */
	uint32_t dostype;
	int32_t bootpri;
	uint32_t flags;
	char name[32];
};

static uint32_t rdsk[BLOCK / 4];
static uint32_t rdsk_at;
static uint32_t cylblocks, heads, secs, locyl, hicyl, tablo, tabhi;
static struct part parts[MAXPARTS];
static int nparts;
static uint8_t used[MAXTAB / 8];	/* table blocks in use */

static void mark(uint32_t n)
{
	if (n < MAXTAB)
		used[n >> 3] |= 1 << (n & 7);
}

static int in_use(uint32_t n)
{
	return n >= MAXTAB || (used[n >> 3] & (1 << (n & 7)));
}

/* Follow a chain of LSEG blocks, marking them */
static void mark_lsegs(uint32_t n)
{
	int steps = 0;

	while (n != NONE && n < MAXTAB && steps++ < MAXTAB) {
		if (get(n, blk) || !valid(blk, ID_LSEG))
			return;
		mark(n);
		n = blk[4];
	}
}

/* 1 = a table was read, 0 = the disk has none, -1 = error */
static int read_table(void)
{
	uint32_t n, next, unit, *e;
	int steps;

	nparts = 0;
	memset(used, 0, sizeof(used));
	for (n = 0; n < 16; n++) {
		if (get(n, rdsk))
			return -1;
		if (valid(rdsk, ID_RDSK))
			break;
	}
	if (n == 16)
		return 0;
	if (rdsk[4] != BLOCK) {
		say("setup: the disk's partition table uses blocks other than 512 bytes.\n");
		return -1;
	}
	rdsk_at = n;
	mark(n);
	heads = rdsk[18];
	secs = rdsk[17];
	tablo = rdsk[32];
	tabhi = rdsk[33];
	locyl = rdsk[34];
	hicyl = rdsk[35];
	cylblocks = rdsk[36];
	if (!cylblocks)
		cylblocks = heads * secs;
	if (!cylblocks || hicyl < locyl) {
		say("setup: the disk's partition table makes no sense.\n");
		return -1;
	}

	/* Partitions, in table order: partition N is hdbN */
	next = rdsk[7];
	steps = 0;
	while (next != NONE && nparts < MAXPARTS && steps++ < 64) {
		struct part *p = parts + nparts;
		if (get(next, blk) || !valid(blk, ID_PART))
			break;
		mark(next);
		p->at = next;
		e = blk + 32;			/* DosEnvec */
		unit = e[1] / 128;
		p->first = e[9] * e[3] * e[5] * unit;
		p->count = (e[10] - e[9] + 1) * e[3] * e[5] * unit;
		p->dostype = e[0] >= 16 ? e[16] : 0;
		p->bootpri = e[0] >= 15 ? (int32_t)e[15] : 0;
		p->flags = blk[5];
		n = ((uint8_t *)blk)[36];
		if (n > 30)
			n = 30;
		memcpy(p->name, (uint8_t *)blk + 37, n);
		p->name[n] = 0;
		nparts++;
		next = blk[4];
	}
	/* File systems and their code, bad block lists, drive init code */
	next = rdsk[8];
	steps = 0;
	while (next != NONE && next < MAXTAB && steps++ < 64) {
		if (get(next, blk) || !valid(blk, ID_FSHD))
			break;
		mark(next);
		n = blk[4];
		mark_lsegs(blk[18]);
		next = n;
	}
	next = rdsk[6];
	steps = 0;
	while (next != NONE && next < MAXTAB && steps++ < 64) {
		if (get(next, blk) || !valid(blk, ID_BADB))
			break;
		mark(next);
		next = blk[4];
	}
	if (rdsk[9] != NONE)
		mark_lsegs(rdsk[9]);
	return 1;
}

static const char *fsname(uint32_t t)
{
	static char s[8];

	if (t == DOSTYPE)
		return "Fuzix";
	if ((t & 0xFFFFFF00UL) == 0x444F5300UL) {	/* DOS\n */
		s[0] = 'D'; s[1] = 'O'; s[2] = 'S'; s[3] = '0' + (t & 0xFF);
		s[4] = 0;
		return (t & 1) ? "FFS" : "OFS";
	}
	if (t == 0x50465303UL)
		return "PFS3";
	if (t == 0x53465300UL || t == 0x53465302UL)
		return "SFS";
	s[0] = t >> 24; s[1] = t >> 16; s[2] = t >> 8;
	s[3] = (t & 0xFF) < 10 ? '0' + (t & 0xFF) : '?';
	s[4] = 0;
	return s;
}

/* The biggest free stretch of the partitionable area, in whole
   cylinders: first block and count, 0 = none */
static uint32_t free_space(uint32_t *first)
{
	uint32_t lo = locyl * cylblocks, hi = (hicyl + 1) * cylblocks;
	uint32_t at = lo, best = 0, end, next;
	int i;

	*first = 0;
	while (at < hi) {
		/* the next partition at or after 'at' */
		next = hi;
		end = at;
		for (i = 0; i < nparts; i++) {
			if (parts[i].first <= at && parts[i].first + parts[i].count > at)
				end = parts[i].first + parts[i].count;
			else if (parts[i].first > at && parts[i].first < next)
				next = parts[i].first;
		}
		if (end > at) {			/* inside a partition */
			at = end;
			continue;
		}
		/* free from at to next, whole cylinders */
		{
			uint32_t a = (at + cylblocks - 1) / cylblocks * cylblocks;
			uint32_t b = next / cylblocks * cylblocks;
			if (b > a && b - a > best) {
				best = b - a;
				*first = a;
			}
		}
		at = next;
	}
	return best;
}

/* A fresh table for a disk without one (or one being wiped): 1 head,
   32 blocks a track, cylinders 0-1 for the table */
static void new_table(uint32_t blocks)
{
	memset(rdsk, 0, sizeof(rdsk));
	heads = 1;
	secs = 32;
	cylblocks = 32;
	locyl = 2;
	hicyl = blocks / cylblocks - 1;
	tablo = 0;
	tabhi = locyl * cylblocks - 1;
	rdsk[0] = ID_RDSK;
	rdsk[3] = HOSTID;
	rdsk[4] = BLOCK;
	rdsk[6] = NONE;			/* bad blocks */
	rdsk[7] = NONE;			/* partitions */
	rdsk[8] = NONE;			/* file systems */
	rdsk[9] = NONE;			/* drive init */
	memset(rdsk + 10, 0xFF, 6 * 4);
	rdsk[16] = hicyl + 1;
	rdsk[17] = secs;
	rdsk[18] = heads;
	rdsk[19] = 1;
	rdsk[20] = rdsk[24] = rdsk[25] = hicyl + 1;
	rdsk[26] = 3;
	rdsk[32] = tablo;
	rdsk[33] = tabhi;
	rdsk[34] = locyl;
	rdsk[35] = hicyl;
	rdsk[36] = cylblocks;
	memcpy(rdsk + 40, "FUZIX   ", 8);
	memcpy(rdsk + 42, "FUZIX DISK      ", 16);
	memcpy(rdsk + 46, "0.1 ", 4);
	rdsk_at = 0;
	nparts = 0;
	memset(used, 0, sizeof(used));
	mark(0);
}

/* A free block of the table area */
static uint32_t take_block(void)
{
	uint32_t n;

	for (n = tablo; n <= tabhi && n < MAXTAB; n++)
		if (n != rdsk_at && !in_use(n)) {
			mark(n);
			return n;
		}
	return NONE;
}

static void make_part(uint32_t *b, uint32_t next, uint32_t first,
		      uint32_t count, int32_t bootpri)
{
	uint32_t *e = b + 32;

	memset(b, 0, BLOCK);
	b[0] = ID_PART;
	b[3] = HOSTID;
	b[4] = next;
	b[5] = 1;			/* bootable */
	memcpy((uint8_t *)b + 36, "\005FUZIX", 6);
	e[0] = 16;
	e[1] = BLOCK / 4;
	e[3] = 1;			/* one surface of cylblocks blocks: */
	e[4] = 1;			/* the same cylinders as the table */
	e[5] = cylblocks;
	e[6] = 0;			/* nothing reserved at the start */
	e[9] = first / cylblocks;
	e[10] = (first + count) / cylblocks - 1;
	e[11] = 30;			/* buffers */
	e[13] = 0x1FE00;		/* max transfer */
	e[14] = 0x7FFFFFFE;		/* mask */
	e[15] = (uint32_t)bootpri;
	e[16] = DOSTYPE;
	finish(b, 64);
}

static void make_fshd(uint32_t *b, uint32_t next, uint32_t first_lseg)
{
	memset(b, 0, BLOCK);
	b[0] = ID_FSHD;
	b[3] = HOSTID;
	b[4] = next;
	b[8] = DOSTYPE;
	b[9] = 1;			/* version 0.1 */
	b[10] = 0x190;			/* patch StackSize, SegList, GlobalVec */
	b[15] = 4096;			/* stack */
	b[18] = first_lseg;
	b[19] = NONE;			/* GlobalVec -1: not BCPL */
	finish(b, 64);
}

static int open_loader(void)
{
	struct stat st;

	loader_fd = open(LOADER, O_RDONLY);
	if (loader_fd == -1 || fstat(loader_fd, &st) == -1) {
		perror(LOADER);
		return -1;
	}
	loader_len = st.st_size;
	return 0;
}

/* LSEG blocks hold 123 longs of the loader each; SummedLongs says how
   many the last one holds (Kickstart's LoadSeg must not see padding) */
static uint16_t lseg_count(void)
{
	return (loader_len + 491) / 492;
}

static int make_lseg(uint32_t *b, uint16_t i, uint32_t next)
{
	uint32_t off = (uint32_t)i * 492;
	uint32_t n = loader_len - off;

	if (n > 492)
		n = 492;
	memset(b, 0, BLOCK);
	b[0] = ID_LSEG;
	b[3] = HOSTID;
	b[4] = next;
	if (lseek(loader_fd, off, SEEK_SET) == -1 ||
	    read(loader_fd, b + 5, n) != (int)n) {
		printf("setup: cannot read %s\n", LOADER);
		return -1;
	}
	finish(b, 5 + n / 4);
	return 0;
}

/* The kernel into the partition's reserved blocks */
static int write_kernel(const char *dev, uint32_t at)
{
	/* longs: the disk transfer wants an even address */
	static uint32_t lbuf[BLOCK / 4];
	uint8_t *buf = (uint8_t *)lbuf;
	struct stat st;
	uint32_t n, i, size;
	int fd, r = -1, save = disk;

	fd = open(KERNEL, O_RDONLY);
	if (fd == -1 || fstat(fd, &st) == -1) {
		perror(KERNEL);
		return -1;
	}
	size = st.st_size;
	n = (size + BLOCK - 1) / BLOCK;
	if (n + 1 > RESERVED) {
		say("setup: the kernel does not fit its blocks.\n");
		return -1;
	}
	disk = open(dev, O_RDWR);
	if (disk == -1) {
		perror(dev);
		disk = save;
		return -1;
	}
	memset(buf, 0, BLOCK);
	memcpy(buf, "FUZK", 4);
	lbuf[1] = size;
	if (put(at, buf))
		goto out;
	for (i = 0; i < n; i++) {
		memset(buf, 0, BLOCK);
		if (read(fd, buf, BLOCK) < 0 || put(at + 1 + i, buf))
			goto out;
	}
	r = 0;
out:
	close(fd);
	close(disk);
	disk = save;
	return r;
}

/*
 *	Write the table changes: the loader as the FUZ\0 file system (a new
 *	one, or in place of an old one), and the FUZIX partition (new at the
 *	end of the list, or in place of partition 'replace'). New blocks are
 *	written first, the one block linking them in last.
 */
static int write_table(uint32_t first, uint32_t count, int replace,
		       int32_t bootpri, int fresh)
{
	uint32_t fshd = NONE, lseg[16], next, pblk, old_lsegs = NONE;
	uint16_t i, nl;
	int steps;

	if (open_loader())
		return -1;
	nl = lseg_count();
	if (nl > 16) {
		say("setup: the loader is too big.\n");
		return -1;
	}

	/* An old FUZ\0 file system: its blocks are reused or freed */
	next = fresh ? NONE : rdsk[8];
	steps = 0;
	while (next != NONE && steps++ < 64) {
		if (get(next, blk) || !valid(blk, ID_FSHD))
			break;
		if (blk[8] == DOSTYPE) {
			fshd = next;
			old_lsegs = blk[18];
			break;
		}
		next = blk[4];
	}
	/* Free the old loader's blocks for reuse */
	next = old_lsegs;
	steps = 0;
	while (next != NONE && next < MAXTAB && steps++ < MAXTAB) {
		if (get(next, blk) || !valid(blk, ID_LSEG))
			break;
		used[next >> 3] &= ~(1 << (next & 7));
		next = blk[4];
	}
	for (i = 0; i < nl; i++)
		if ((lseg[i] = take_block()) == NONE)
			goto full;
	for (i = 0; i < nl; i++) {
		if (make_lseg(blk, i, i + 1 < nl ? lseg[i + 1] : NONE) ||
		    put(lseg[i], blk))
			return -1;
	}
	if (fshd != NONE) {
		/* rewrite the old header in place, same chain position */
		if (get(fshd, blk))
			return -1;
		next = blk[4];
		make_fshd(blk, next, lseg[0]);
		if (put(fshd, blk))
			return -1;
	} else {
		if ((fshd = take_block()) == NONE)
			goto full;
		make_fshd(blk, rdsk[8], lseg[0]);
		if (put(fshd, blk))
			return -1;
		rdsk[8] = fshd;
	}

	/* The partition */
	if (replace >= 0) {
		pblk = parts[replace].at;
		if (get(pblk, blk))
			return -1;
		make_part(blk, blk[4], first, count, bootpri);
		if (put(pblk, blk))
			return -1;
	} else {
		if ((pblk = take_block()) == NONE)
			goto full;
		make_part(blk, NONE, first, count, bootpri);
		if (put(pblk, blk))
			return -1;
		if (nparts == 0)
			rdsk[7] = pblk;
		else {
			/* link it after the last partition */
			if (get(parts[nparts - 1].at, blk))
				return -1;
			blk[4] = pblk;
			finish(blk, blk[1]);
			if (put(parts[nparts - 1].at, blk))
				return -1;
		}
	}
	finish(rdsk, 64);
	if (put(rdsk_at, rdsk))
		return -1;
	return 0;
full:
	say("setup: the partition table has no free blocks left for Fuzix.\n");
	return -1;
}

/* Boot priorities of the other bootable partitions */
static int32_t other_pri(int max, int skip)
{
	int32_t r = max ? -128 : 127;
	int i, any = 0;

	for (i = 0; i < nparts; i++) {
		if (i == skip || !(parts[i].flags & 1) || parts[i].dostype == DOSTYPE)
			continue;
		any = 1;
		if (max ? parts[i].bootpri > r : parts[i].bootpri < r)
			r = parts[i].bootpri;
	}
	return any ? r : 0;
}

static void show_table(void)
{
	int i;

	for (i = 0; i < nparts; i++)
		printf("  %2d  %-12s %-6s %5lu MB%s\n", i + 1, parts[i].name,
		       fsname(parts[i].dostype),
		       (unsigned long)(parts[i].count / 2048),
		       (parts[i].flags & 1) ? "  boots" : "");
}

/*
 *	Copy the floppy's system to the hard disk. Done here rather than with
 *	"tar -c | tar -x": a Fuzix pipe keeps its data in blocks of the root
 *	file system, which during the install is the floppy, so every chunk
 *	through the pipe would be written to the floppy.
 */
static char src[256], dst[256];
static char cbuf[4096];

/* Files with more than one name: copy once, link the other names */
#define MAXLINKS 64
static struct {
	ino_t ino;
	char *path;
} links[MAXLINKS];
static int nlinks;

static int copy_file(struct stat *st)
{
	int in, out, n, i;

	if (st->st_nlink > 1) {
		for (i = 0; i < nlinks; i++)
			if (links[i].ino == st->st_ino)
				return link(links[i].path, dst);
		if (nlinks < MAXLINKS) {
			links[nlinks].ino = st->st_ino;
			links[nlinks].path = strdup(dst);
			if (links[nlinks].path)
				nlinks++;
		}
	}
	in = open(src, O_RDONLY);
	if (in == -1)
		return -1;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out == -1) {
		close(in);
		return -1;
	}
	while ((n = read(in, cbuf, sizeof(cbuf))) > 0)
		if (write(out, cbuf, n) != n) {
			n = -1;
			break;
		}
	close(in);
	close(out);
	return n;
}

static int copy_tree(int top)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	size_t sl = strlen(src), dl = strlen(dst);
	int r = 0;

	d = opendir(src);
	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (top && !strcmp(e->d_name, "mnt"))
			continue;
		if (sl + strlen(e->d_name) + 2 > sizeof(src) ||
		    dl + strlen(e->d_name) + 2 > sizeof(dst))
			continue;
		if (sl > 1)
			strcat(src, "/");
		strcat(src, e->d_name);
		strcat(dst, "/");
		strcat(dst, e->d_name);
		if (stat(src, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				if (mkdir(dst, 0700) && errno != EEXIST)
					r = -1;
				else if (copy_tree(0))
					r = -1;
			} else if (S_ISREG(st.st_mode)) {
				if (copy_file(&st) < 0)
					r = -1;
			} else if (mknod(dst, st.st_mode, st.st_rdev))
				r = -1;
			if (r == 0) {
				chown(dst, st.st_uid, st.st_gid);
				chmod(dst, st.st_mode & 07777);
			}
		}
		if (r)
			printf("setup: cannot copy %s\n", src);
		src[sl] = 0;
		dst[dl] = 0;
		if (r)
			break;
	}
	closedir(d);
	return r;
}

static int copy_system(void)
{
	strcpy(src, "/");
	strcpy(dst, "/mnt");
	nlinks = 0;
	return copy_tree(1);
}

static int install(void)
{
	uint32_t blocks, first = 0, count = 0, gap, gapat, fsblocks;
	int replace = -1, fresh = 0, i, part, r;
	int32_t bootpri;
	char buf[64];

	blocks = dev_blocks(DISK);
	if (blocks < MINDISK) {
		say("setup: no hard disk found (or it is smaller than 3 MB).\n");
		return 1;
	}
	disk = open(DISK, O_RDWR);
	if (disk == -1) {
		perror(DISK);
		return 1;
	}

	printf("\nInstalling Fuzix on the hard disk (%lu MB).\n\n",
	       (unsigned long)(blocks / 2048));
	r = read_table();
	if (r < 0)
		return 1;
	if (r == 0) {
		say("The disk has no Amiga partitions yet. Fuzix takes up to 32 MB\n"
		    "at its start; the rest stays free for AmigaOS (HDToolBox).\n");
		fresh = 1;
	} else {
		say("The disk's partitions:\n\n");
		show_table();
		gap = free_space(&gapat);
		say("\n");
		for (i = 0; i < nparts; i++)
			if (parts[i].dostype == DOSTYPE)
				printf("  r   install again in partition %d (FUZIX)\n", i + 1);
		if (gap >= RESERVED + MINFS)
			printf("  f   use free space on the disk (%lu MB free)\n",
			       (unsigned long)(gap / 2048));
		say("  1-n replace a partition with Fuzix (it is erased)\n"
		    "  e   erase the WHOLE disk for Fuzix, AmigaOS too\n"
		    "  q   stop, change nothing\n");
		if (ask("\nWhich? ", buf, sizeof(buf)) || buf[0] == 'q' || !buf[0]) {
			say("Nothing was changed.\n");
			return 1;
		}
		if (buf[0] == 'r') {
			for (i = 0; i < nparts; i++)
				if (parts[i].dostype == DOSTYPE)
					break;
			if (i == nparts) {
				say("There is no FUZIX partition. Nothing was changed.\n");
				return 1;
			}
			replace = i;
		} else if (buf[0] == 'f') {
			if (gap < RESERVED + MINFS) {
				say("There is not enough free space. Nothing was changed.\n");
				return 1;
			}
			first = gapat;
			count = gap;
		} else if (buf[0] == 'e') {
			if (ask("Type ERASE to erase the whole disk: ", buf, sizeof(buf)) ||
			    strcmp(buf, "ERASE")) {
				say("Nothing was changed.\n");
				return 1;
			}
			fresh = 1;
		} else {
			i = atoi(buf) - 1;
			if (i < 0 || i >= nparts) {
				say("No such partition. Nothing was changed.\n");
				return 1;
			}
			printf("Partition %d, %s, will be ERASED. Type its name to go on: ",
			       i + 1, parts[i].name);
			fflush(stdout);
			if (ask("", buf, sizeof(buf)) || strcmp(buf, parts[i].name)) {
				say("Nothing was changed.\n");
				return 1;
			}
			replace = i;
		}
	}

	if (fresh) {
		new_table(blocks);
		first = locyl * cylblocks;
		count = blocks / cylblocks * cylblocks - first;
	} else if (replace >= 0) {
		first = parts[replace].first;
		count = parts[replace].count;
		/* whole cylinders of the table */
		first = (first + cylblocks - 1) / cylblocks * cylblocks;
		count = (parts[replace].first + count - first) / cylblocks * cylblocks;
		if (count < RESERVED + MINFS) {
			say("That partition is too small for Fuzix (3 MB at least).\n");
			return 1;
		}
	}
	/* No bigger than Fuzix reaches (whole cylinders) */
	if (count > MAXPART)
		count = MAXPART / cylblocks * cylblocks;
	fsblocks = count - RESERVED;

	/* Which system starts by default */
	bootpri = 0;
	if (other_pri(1, replace) != 0 || nparts > (replace >= 0)) {
		if (yes("\nStart Fuzix when the Amiga is switched on? (y = Fuzix,\n"
			"n = AmigaOS stays first) ")) {
			bootpri = other_pri(1, replace) + 1;
			if (bootpri > 127)
				bootpri = 127;
		} else {
			bootpri = other_pri(0, replace) - 1;
			if (bootpri < -127)
				bootpri = -127;
		}
	}
	printf("\nFuzix gets %lu MB.%s\n", (unsigned long)(count / 2048),
	       fresh ? " EVERYTHING on the disk will be erased." : "");
	if (!yes("Go on? (y/n) ")) {
		say("Nothing was changed.\n");
		return 1;
	}

	say("\nWriting the partition table and the boot code...\n");
	if (write_table(first, count, replace, bootpri, fresh))
		return 1;
	part = replace >= 0 ? replace + 1 : nparts + 1;
	close(disk);
	sync();

	/* Let the kernel see the new partition */
	disk = open(DISK, O_RDONLY);
	if (disk == -1 || ioctl(disk, BLKRRPART, 0) == -1) {
		perror("setup: reading the new partitions");
		return 1;
	}
	close(disk);
	snprintf(buf, sizeof(buf), "/dev/hdb%d", part);
	if (dev_blocks(buf) != count) {
		printf("setup: %s did not appear as expected.\n", buf);
		return 1;
	}
	say("Writing the kernel...\n");
	if (write_kernel(buf, count - RESERVED))
		return 1;

	say("Making the file system...\n");
	{
		char cmd[96];
		snprintf(cmd, sizeof(cmd), "echo y | mkfs -f %s %u %lu >/dev/null",
			 buf, fsblocks >= 32768 ? 256 : (unsigned)(fsblocks / 128),
			 (unsigned long)fsblocks);
		if (run(cmd))
			return 1;
		snprintf(cmd, sizeof(cmd), "mount %s /mnt", buf);
		if (run(cmd))
			return 1;
	}

	say("Copying Fuzix from the floppy (this takes a few minutes)...\n");
	if (copy_system()) {
		run("umount /mnt");
		return 1;
	}
	mkdir("/mnt/mnt", 0755);
	/* The first boot from the hard disk offers the Extras disks */
	i = open("/mnt" EXTRAS_TODO, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (i != -1)
		close(i);
	sync();
	if (run("umount /mnt"))
		return 1;
	sync();

	say("\nFuzix is installed.\n\n");
	if (bootpri > other_pri(1, replace) || nparts == 0)
		say("The Amiga now starts Fuzix. For AmigaOS: hold both mouse\n"
		    "buttons while switching on, Boot Options, and pick its partition.\n");
	else
		say("AmigaOS still starts first. For Fuzix: hold both mouse buttons\n"
		    "while switching on, Boot Options, and pick FUZIX.\n");
	ask("\nTake the floppy out of the drive and press Return to restart. ",
	    buf, sizeof(buf));
	/* Directly: /bin/reboot asks init, which is busy running /etc/rc
	   (which is running us) and never gets to it */
	sync();
	uadmin(A_REBOOT, 0, 0);
	perror("setup: restarting");
	return 1;
}

/*
 *	Extras disks: a tar archive written straight onto the floppy, whose
 *	first member is etc/extras.N-of-T (it stays in /etc and records that
 *	disk N is in).
 */
static int extras_disk(int *n, int *total)
{
	char hdr[BLOCK];
	int fd = open(FLOPPY, O_RDONLY);
	int r;

	if (fd == -1)
		return -1;
	r = read(fd, hdr, BLOCK);
	close(fd);
	if (r != BLOCK || strncmp(hdr, "etc/extras.", 11))
		return -1;
	if (sscanf(hdr + 11, "%d-of-%d", n, total) != 2)
		return -1;
	return 0;
}

static int installed(int n, int total)
{
	char name[40];

	snprintf(name, sizeof(name), "/etc/extras.%d-of-%d", n, total);
	return access(name, 0) == 0;
}

static int extras(void)
{
	char buf[16];
	int n, total = 0, i, left;

	say("\nThe Extras disks add the rest of Fuzix: more commands, games, "
	    "manual pages.\n");
	for (;;) {
		if (total) {
			left = 0;
			for (i = 1; i <= total; i++)
				if (!installed(i, total))
					left++;
			if (left == 0) {
				unlink(EXTRAS_TODO);
				say("All the Extras disks are in.\n");
				return 0;
			}
		}
		if (ask("\nPut an Extras disk in the drive and press Return "
			"(or type q and Return to stop): ", buf, sizeof(buf)) ||
		    buf[0] == 'q' || buf[0] == 'Q')
			return 0;
		if (extras_disk(&n, &total)) {
			say("That is not a Fuzix Extras disk.\n");
			continue;
		}
		if (installed(n, total)) {
			printf("Extras disk %d of %d is in already.\n", n, total);
			continue;
		}
		printf("Copying Extras disk %d of %d...\n", n, total);
		if (run("cd / && tar -x -f " FLOPPY))
			continue;
		sync();
		printf("Extras disk %d of %d is in.\n", n, total);
	}
}

/* At boot (from /etc/rc) */
static int boot(void)
{
	struct stat st;

	if (stat("/", &st) == -1)
		return 0;
	if (st.st_dev == FLOPPY_ROOT) {
		/* Running from the install floppy */
		if (dev_blocks(DISK) < MINDISK)
			return 0;
		if (yes("\nThere is a hard disk. Install Fuzix on it now? "
			"(y/n) "))
			return install();
		say("You can install later: log in as root and type setup\n");
		return 0;
	}
	if (access(EXTRAS_TODO, 0) == 0) {
		if (yes("\nAdd the programs on the Extras disks now? (y/n) "))
			return extras();
		unlink(EXTRAS_TODO);
		say("You can add them later: log in as root and type "
		    "setup extras\n");
	}
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc == 2 && strcmp(argv[1], "-boot") == 0)
		return boot();
	if (getuid() != 0) {
		say("setup: only root can do that.\n");
		return 1;
	}
	if (argc == 2 && strcmp(argv[1], "extras") == 0)
		return extras();
	if (argc == 1)
		return install();
	say("usage: setup [extras]\n");
	return 1;
}
