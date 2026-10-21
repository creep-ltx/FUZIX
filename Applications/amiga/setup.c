/*
 *	setup - install Fuzix on an Amiga's hard disk
 *
 *	setup		install from the install floppy onto the IDE disk
 *	setup extras	add the programs on the Extras floppies
 *	setup -boot	run by /etc/rc at every boot: offers the install when
 *			Fuzix runs from the install floppy and there is a
 *			hard disk, and offers the Extras disks once after
 *			the first boot from the hard disk
 *
 *	The hard disk gets two partition tables. Block 0 holds a PC style
 *	(MBR) table, which Fuzix reads: partition 1 is the Fuzix file system.
 *	Blocks 1 onwards hold an Amiga Rigid Disk Block, which Kickstart
 *	reads: one bootable partition, FUZIX, over the same blocks, with
 *	DosType FUZ\0, and as the "file system" for FUZ\0 the Fuzix loader
 *	(/usr/lib/fuzix/fuzix.hunk: a small program with the kernel inside).
 *	At power on Kickstart loads it from the disk and runs it, and it
 *	starts Fuzix: no floppy needed. The same layout as mkrdb.py.
 *
 *	While installing, Fuzix runs from the floppy: the floppy is hda and
 *	the hard disk hdb. After the install the hard disk is still hdb (the
 *	floppy drive is always hda), and the root file system is hdb1.
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
#define DISK_ROOT	"/dev/hdb1"
#define LOADER		"/usr/lib/fuzix/fuzix.hunk"
#define EXTRAS_TODO	"/etc/extras.todo"

#define BLOCK		512
#define HOSTID		7
#define HEADS		1
#define SECS		32
#define CYLBLOCKS	(HEADS * SECS)
#define RDB_CYLS	64		/* blocks 0-2047: the partition tables */
#define FIRST		(RDB_CYLS * CYLBLOCKS)
#define MAXFS		65504		/* Fuzix file systems: 16-bit block numbers */
#define DOSTYPE		0x46555A00UL	/* 'FUZ\0' */
#define NONE		0xFFFFFFFFUL

static uint32_t blk[BLOCK / 4];
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

/*
 *	Amiga partition table blocks (see the RKM Devices manual, "SCSI
 *	device": RigidDiskBlock, PartitionBlock, FileSysHeaderBlock,
 *	LoadSegBlock). Long 1 is the number of longs checksummed, long 2 the
 *	checksum that makes them sum to zero.
 */
static void finish(uint16_t summed)
{
	uint32_t sum = 0;
	uint16_t i;

	blk[1] = summed;
	blk[2] = 0;
	for (i = 0; i < summed; i++)
		sum += blk[i];
	blk[2] = -sum;
}

static void text(uint32_t *l, const char *s, int n)
{
	memset(l, 0, n);
	memcpy(l, s, strlen(s) < (size_t)n ? strlen(s) : (size_t)n);
}

static void make_rdsk(uint32_t cyls)
{
	uint32_t *l = blk;

	memset(blk, 0, sizeof(blk));
	l[0] = 0x5244534BUL;		/* RDSK */
	l[3] = HOSTID;
	l[4] = BLOCK;
	l[6] = NONE;			/* bad blocks */
	l[7] = 2;			/* partitions */
	l[8] = 3;			/* file systems */
	l[9] = NONE;			/* drive init */
	memset(l + 10, 0xFF, 6 * 4);
	l[16] = cyls;
	l[17] = SECS;
	l[18] = HEADS;
	l[19] = 1;			/* interleave */
	l[20] = cyls;			/* park */
	l[24] = cyls;			/* write precomp */
	l[25] = cyls;			/* reduced write */
	l[26] = 3;			/* step rate */
	l[32] = 1;			/* RDB blocks low */
	l[33] = FIRST - 1;		/* RDB blocks high */
	l[34] = RDB_CYLS;		/* first cylinder for partitions */
	l[35] = cyls - 1;
	l[36] = CYLBLOCKS;
	text(l + 40, "FUZIX", 8);
	text(l + 42, "FUZIX DISK", 16);
	text(l + 46, "0.1", 4);
	finish(64);
}

static void make_part(uint32_t lowcyl, uint32_t highcyl)
{
	uint32_t *l = blk;
	uint32_t *e;

	memset(blk, 0, sizeof(blk));
	l[0] = 0x50415254UL;		/* PART */
	l[3] = HOSTID;
	l[4] = NONE;			/* next */
	l[5] = 1;			/* bootable */
	memcpy(l + 9, "\005FUZIX", 6);
	e = l + 32;			/* DosEnvec */
	e[0] = 16;
	e[1] = BLOCK / 4;
	e[3] = HEADS;
	e[4] = 1;
	e[5] = SECS;
	e[6] = 2;			/* reserved */
	e[9] = lowcyl;
	e[10] = highcyl;
	e[11] = 30;			/* buffers */
	e[13] = 0x1FE00;		/* max transfer */
	e[14] = 0x7FFFFFFE;		/* mask */
	e[15] = 0;			/* boot priority */
	e[16] = DOSTYPE;
	finish(64);
}

static void make_fshd(uint32_t first_lseg)
{
	uint32_t *l = blk;

	memset(blk, 0, sizeof(blk));
	l[0] = 0x46534844UL;		/* FSHD */
	l[3] = HOSTID;
	l[4] = NONE;
	l[8] = DOSTYPE;
	l[9] = 1;			/* version 0.1 */
	l[10] = 0x190;			/* patch StackSize, SegList, GlobalVec */
	l[15] = 4096;			/* stack */
	l[18] = first_lseg;
	l[19] = NONE;			/* GlobalVec -1: not BCPL */
	finish(64);
}

/* LSEG blocks hold 123 longs of the loader each; SummedLongs says how
   many the last one holds (Kickstart's LoadSeg must not see padding) */
static uint16_t lseg_count(void)
{
	return (loader_len + 491) / 492;
}

static int make_lseg(uint16_t i, uint32_t first)
{
	uint32_t off = (uint32_t)i * 492;
	uint32_t n = loader_len - off;

	if (n > 492)
		n = 492;
	memset(blk, 0, sizeof(blk));
	blk[0] = 0x4C534547UL;		/* LSEG */
	blk[3] = HOSTID;
	blk[4] = (i + 1 < lseg_count()) ? first + i + 1 : NONE;
	if (lseek(loader_fd, off, SEEK_SET) == -1 ||
	    read(loader_fd, blk + 5, n) != (int)n) {
		printf("setup: cannot read %s\n", LOADER);
		return -1;
	}
	finish(5 + n / 4);
	return 0;
}

/* PC partition table: one Fuzix partition (type 0x7E) */
static void make_mbr(uint32_t first, uint32_t count)
{
	uint8_t *b = (uint8_t *)blk;
	uint8_t *e = b + 446;

	memset(blk, 0, sizeof(blk));
	e[4] = 0x7E;
	e[8] = first;
	e[9] = first >> 8;
	e[10] = first >> 16;
	e[11] = first >> 24;
	e[12] = count;
	e[13] = count >> 8;
	e[14] = count >> 16;
	e[15] = count >> 24;
	b[510] = 0x55;
	b[511] = 0xAA;
}

static int put(int fd, uint32_t n)
{
	if (lseek(fd, (off_t)n * BLOCK, SEEK_SET) == -1 ||
	    write(fd, blk, BLOCK) != BLOCK) {
		perror("setup: writing the hard disk");
		return -1;
	}
	return 0;
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

static int write_tables(uint32_t disk, uint32_t fsblocks)
{
	uint32_t cyls = disk / CYLBLOCKS;
	uint16_t i;
	int fd = open(DISK, O_RDWR);

	if (fd == -1) {
		perror(DISK);
		return -1;
	}
	/* Clear the old tables (an AmigaOS RDB may be anywhere in 0-15) */
	memset(blk, 0, sizeof(blk));
	for (i = 0; i < 128; i++)
		if (put(fd, i))
			goto bad;
	make_mbr(FIRST, fsblocks);
	if (put(fd, 0))
		goto bad;
	make_rdsk(cyls);
	if (put(fd, 1))
		goto bad;
	make_part(FIRST / CYLBLOCKS, (FIRST + fsblocks) / CYLBLOCKS - 1);
	if (put(fd, 2))
		goto bad;
	make_fshd(4);
	if (put(fd, 3))
		goto bad;
	for (i = 0; i < lseg_count(); i++) {
		if (make_lseg(i, 4) || put(fd, 4 + i))
			goto bad;
	}
	close(fd);
	sync();
	/* Let the kernel see the new partitions */
	fd = open(DISK, O_RDONLY);
	if (fd == -1 || ioctl(fd, BLKRRPART, 0) == -1) {
		perror("setup: reading the new partitions");
		return -1;
	}
	close(fd);
	return 0;
bad:
	close(fd);
	return -1;
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
	uint32_t disk, fsblocks;
	char buf[64];
	int fd;

	disk = dev_blocks(DISK);
	if (disk < FIRST + 4096) {
		say("setup: no hard disk found (or it is smaller than 3 MB).\n");
		return 1;
	}
	if (open_loader())
		return 1;
	fsblocks = disk - FIRST;
	if (fsblocks > MAXFS)
		fsblocks = MAXFS;
	fsblocks -= fsblocks % CYLBLOCKS;

	printf("\nInstalling Fuzix on the hard disk.\n\n"
	       "The hard disk holds %lu MB. Fuzix will use %lu MB of it",
	       (unsigned long)(disk / 2048), (unsigned long)(fsblocks / 2048));
	if (FIRST + fsblocks < disk)
		say(" (a Fuzix file system holds at most 32 MB)");
	say(".\n\nEVERYTHING on the hard disk will be erased, AmigaOS "
	    "partitions too.\n");
	if (ask("Type ERASE and press Return to go on: ", buf, sizeof(buf)) ||
	    strcmp(buf, "ERASE")) {
		say("Nothing was changed.\n");
		return 1;
	}

	say("\nWriting the partition tables and the boot code...\n");
	if (write_tables(disk, fsblocks))
		return 1;
	if (dev_blocks(DISK_ROOT) != fsblocks) {
		say("setup: the new partition did not appear.\n");
		return 1;
	}

	say("Making the file system...\n");
	snprintf(buf, sizeof(buf), "echo y | mkfs -f %s %u %lu >/dev/null",
		 DISK_ROOT, fsblocks >= 32768 ? 256 : (unsigned)(fsblocks / 128),
		 (unsigned long)fsblocks);
	if (run(buf))
		return 1;
	if (run("mount " DISK_ROOT " /mnt"))
		return 1;

	say("Copying Fuzix from the floppy (this takes a few minutes)...\n");
	if (copy_system()) {
		run("umount /mnt");
		return 1;
	}
	mkdir("/mnt/mnt", 0755);
	/* The first boot from the hard disk offers the Extras disks */
	fd = open("/mnt" EXTRAS_TODO, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd != -1)
		close(fd);
	sync();
	if (run("umount /mnt"))
		return 1;
	sync();

	say("\nFuzix is installed.\n\n"
	    "Take the floppy out of the drive and press Return: the Amiga "
	    "restarts\nand boots Fuzix from the hard disk.\n");
	ask("", buf, sizeof(buf));
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
		if (dev_blocks(DISK) < FIRST + 4096)
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
