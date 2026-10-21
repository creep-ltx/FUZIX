/*
 *	amigadir, amigaread - AmigaDOS disks on Fuzix
 *
 *	amigadir [-l] [-r] [-d disk] [dir]	list a directory
 *	amigaread [-d disk] file [to]		copy a file out (to stdout
 *						when there is no "to")
 *	amigaread -r [-d disk] dir [to]		copy a whole directory out
 *
 *	The disk is the floppy, /dev/hda, unless -d names another device or
 *	an image file (.adf, or a hard disk image with an Amiga partition
 *	table). Paths are AmigaDOS paths without the volume: "s/startup-
 *	sequence", "c"; case does not matter. Every AmigaDOS variant reads:
 *	OFS and FFS, international, directory cache, long names (OS 3.2).
 *
 *	The AmigaDOS reader is CFile's (adf.c), by the same author.
 *	GNU GPL version 2, like FUZIX.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <syscalls.h>
#include "adf.h"

static const char *prog;
static int fd = -1;
static int longlist, recursive;

/* What adf.c asks of the program */
void *arc_alloc(unsigned long n)
{
	return calloc(1, n);
}

void arc_free(void *p)
{
	free(p);
}

const char *arc_errstr(int e)
{
	switch (e) {
	case ARC_ECORRUPT: return "the disk is damaged, or the name is not on it";
	case ARC_EUNSUP: return "not an AmigaDOS disk";
	case ARC_ENOMEM: return "not enough memory";
	case ARC_EREAD: return "the disk could not be read";
	case ARC_ESTOP: return "stopped";
	default: return "error";
	}
}

static long src_read(void *ctx, unsigned long off, void *buf, long n)
{
	if (lseek(fd, (off_t)off, SEEK_SET) == -1)
		return -1;
	return read(fd, buf, n);
}

static void fail(const char *what, int e)
{
	fprintf(stderr, "%s: %s: %s\n", prog, what, arc_errstr(e));
	exit(1);
}

/* AmigaDOS dates count from 1978-01-01, Unix from 1970-01-01 */
static time_t unixtime(const struct arcent *e)
{
	return ((time_t)e->days + 2922) * 86400L + e->mins * 60L + e->ticks / 50;
}

static void protbits(unsigned long p, char *s)
{
	/* AmigaDOS: hsparwed; r w e d are set when NOT allowed */
	s[0] = p & 0x80 ? 'h' : '-';
	s[1] = p & 0x40 ? 's' : '-';
	s[2] = p & 0x20 ? 'p' : '-';
	s[3] = p & 0x10 ? 'a' : '-';
	s[4] = p & 0x08 ? '-' : 'r';
	s[5] = p & 0x04 ? '-' : 'w';
	s[6] = p & 0x02 ? '-' : 'e';
	s[7] = p & 0x01 ? '-' : 'd';
	s[8] = 0;
}

/* Listing: entries are collected, then printed (a recursive listing
   lists subdirectories after their parent) */
#define MAXENT 128

struct ent {
	char name[108];
	unsigned long size;
	unsigned char isdir;
	int method;
	unsigned long prot;
	time_t when;
};

struct list {
	struct ent *e;
	int n, full;
};

static int collect(void *ctx, const struct arcent *a)
{
	struct list *l = ctx;
	struct ent *e;

	if (l->n == MAXENT) {
		l->full = 1;
		return 0;
	}
	e = l->e + l->n++;
	strncpy(e->name, a->name, sizeof(e->name) - 1);
	e->name[sizeof(e->name) - 1] = 0;
	e->size = a->size;
	e->isdir = a->isdir;
	e->method = a->method;
	e->prot = a->prot;
	e->when = unixtime(a);
	return 1;
}

static int byname(const void *a, const void *b)
{
	const struct ent *x = a, *y = b;
	if (x->isdir != y->isdir)
		return y->isdir - x->isdir;	/* directories first */
	return strcasecmp(x->name, y->name);
}

/* Insertion sort: a directory has few entries */
static void sort(struct ent *e, int n)
{
	struct ent t;
	int i, j;

	for (i = 1; i < n; i++) {
		t = e[i];
		for (j = i - 1; j >= 0 && byname(&e[j], &t) > 0; j--)
			e[j + 1] = e[j];
		e[j + 1] = t;
	}
}

static void join(char *out, size_t len, const char *dir, const char *name)
{
	if (*dir)
		snprintf(out, len, "%s/%s", dir, name);
	else
		snprintf(out, len, "%s", name);
}

static void list(struct adfvol *v, const char *dir)
{
	struct list l;
	char path[256], prot[9];
	struct tm *tm;
	int i, r;

	l.e = calloc(MAXENT, sizeof(struct ent));
	if (l.e == NULL)
		fail(dir, ARC_ENOMEM);
	l.n = l.full = 0;
	r = adf_list(v, dir, collect, &l);
	if (r < 0)
		fail(*dir ? dir : "the root", r);
	sort(l.e, l.n);
	if (recursive && *dir)
		printf("%s:\n", dir);
	for (i = 0; i < l.n; i++) {
		struct ent *e = l.e + i;
		if (longlist) {
			protbits(e->prot, prot);
			tm = localtime(&e->when);
			if (e->isdir)
				printf("%s      (dir) ", prot);
			else
				printf("%s %10lu ", prot, e->size);
			if (tm)
				printf("%04d-%02d-%02d %02d:%02d ", tm->tm_year + 1900,
				       tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
				       tm->tm_min);
		}
		printf("%s%s%s\n", e->name, e->isdir ? "/" : "",
		       e->method == ADF_SLINK ? " (soft link)" :
		       e->method == ADF_HLINK ? " (link)" : "");
	}
	if (l.full)
		printf("(only the first %d entries)\n", MAXENT);
	if (recursive)
		for (i = 0; i < l.n; i++)
			if (l.e[i].isdir && l.e[i].method != ADF_HLINK) {
				join(path, sizeof(path), dir, l.e[i].name);
				putchar('\n');
				list(v, path);
			}
	free(l.e);
}

/* Reading */
/* A pipe takes a big write in parts: loop */
static int sink_write(void *ctx, const void *buf, long n)
{
	const char *p = buf;
	int w;

	while (n > 0) {
		w = write(*(int *)ctx, p, n > 512 ? 512 : n);
		if (w <= 0)
			return 0;
		p += w;
		n -= w;
	}
	return 1;
}

static int find(struct adfvol *v, const char *path, struct arcent *out);

static int found;
static struct arcent want;
static const char *want_name;

static int match(void *ctx, const struct arcent *a)
{
	if (strcasecmp(a->name, want_name) == 0) {
		want = *a;
		found = 1;
		return 0;
	}
	return 1;
}

/* Find an entry: list its directory and pick it out */
static int find(struct adfvol *v, const char *path, struct arcent *out)
{
	char dir[256];
	const char *slash = strrchr(path, '/');
	int r;

	if (slash) {
		size_t n = slash - path;
		if (n >= sizeof(dir))
			n = sizeof(dir) - 1;
		memcpy(dir, path, n);
		dir[n] = 0;
		want_name = slash + 1;
	} else {
		dir[0] = 0;
		want_name = path;
	}
	found = 0;
	r = adf_list(v, dir, match, NULL);
	/* match() stops the listing when it has the entry: ARC_ESTOP */
	if (found) {
		*out = want;
		return 0;
	}
	if (r < 0)
		return r;
	return ARC_ECORRUPT;
}

static void readfile(struct adfvol *v, const char *path, const char *to,
		     const struct arcent *e)
{
	struct arcsink s;
	struct utimbuf ut;
	int out = 1, r;

	if (to) {
		out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out == -1) {
			perror(to);
			exit(1);
		}
	}
	s.write = sink_write;
	s.ctx = &out;
	r = adf_extract(v, path, &s);
	if (to) {
		close(out);
		ut.actime = ut.modtime = unixtime(e);
		utime(to, &ut);
		/* AmigaDOS e (executable) means nothing here: plain files */
	}
	/* Stopped while writing to stdout: the reader went away (head) */
	if (r == ARC_ESTOP && !to)
		exit(0);
	if (r < 0)
		fail(path, r);
}

/* A whole directory into a directory of the same name under "to" */
static int copy_one(void *ctx, const struct arcent *a)
{
	struct list *l = ctx;
	return collect(l, a);
}

static void readtree(struct adfvol *v, const char *dir, const char *to)
{
	struct list l;
	char from[256], dest[256];
	int i, r;

	if (mkdir(to, 0755) == -1 && errno != EEXIST) {
		perror(to);
		exit(1);
	}
	l.e = calloc(MAXENT, sizeof(struct ent));
	if (l.e == NULL)
		fail(dir, ARC_ENOMEM);
	l.n = l.full = 0;
	r = adf_list(v, dir, copy_one, &l);
	if (r < 0)
		fail(*dir ? dir : "the root", r);
	for (i = 0; i < l.n; i++) {
		struct ent *e = l.e + i;
		join(from, sizeof(from), dir, e->name);
		join(dest, sizeof(dest), to, e->name);
		if (e->isdir) {
			if (e->method != ADF_HLINK)
				readtree(v, from, dest);
		} else {
			struct arcent a;
			memset(&a, 0, sizeof(a));
			a.days = (e->when / 86400L) - 2922;
			a.mins = (e->when % 86400L) / 60;
			fprintf(stderr, "%s\n", from);
			readfile(v, from, dest, &a);
		}
	}
	if (l.full)
		fprintf(stderr, "%s: %s: only the first %d entries\n", prog, dir, MAXENT);
	free(l.e);
}

static void usage(void)
{
	if (strcmp(prog, "amigadir") == 0)
		fprintf(stderr, "usage: amigadir [-l] [-r] [-d disk] [dir]\n");
	else
		fprintf(stderr, "usage: amigaread [-r] [-d disk] path [to]\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	const char *disk = "/dev/hda";
	struct adfvol v;
	struct arcsrc s;
	struct stat st;
	unsigned long blocks = 0;
	int opt, r;

	prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	while ((opt = getopt(argc, argv, "lrd:")) != -1) {
		switch (opt) {
		case 'l':
			longlist = 1;
			break;
		case 'r':
			recursive = 1;
			break;
		case 'd':
			disk = optarg;
			break;
		default:
			usage();
		}
	}

	fd = open(disk, O_RDONLY);
	if (fd == -1 || fstat(fd, &st) == -1) {
		perror(disk);
		return 1;
	}
	s.read = src_read;
	s.ctx = NULL;
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE, &blocks) == -1 || blocks == 0) {
			fprintf(stderr, "%s: %s: no disk in the drive?\n", prog, disk);
			return 1;
		}
		s.size = blocks * 512;
	} else
		s.size = st.st_size;

	memset(&v, 0, sizeof(v));
	r = adf_open(&s, &v);
	if (r < 0)
		fail(disk, r);

	if (strcmp(prog, "amigadir") == 0) {
		if (argc - optind > 1)
			usage();
		printf("%s:%s\n", adf_volname(&v),
		       optind < argc ? argv[optind] : "");
		list(&v, optind < argc ? argv[optind] : "");
	} else {
		struct arcent e;
		const char *path, *to;

		if (argc - optind < 1 || argc - optind > 2)
			usage();
		path = argv[optind];
		to = argc - optind == 2 ? argv[optind + 1] : NULL;
		r = find(&v, path, &e);
		if (r < 0)
			fail(path, r);
		if (e.isdir) {
			if (!recursive) {
				fprintf(stderr, "%s: %s is a directory (use -r)\n",
					prog, path);
				return 1;
			}
			readtree(&v, path, to ? to : e.name);
		} else
			readfile(&v, path, to, &e);
	}
	adf_close(&v);
	return 0;
}
