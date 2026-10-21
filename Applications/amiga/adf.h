/* From CFile (core/adf.h) by Tobias Karlsson, contributed to FUZIX under
 * the GNU GPL version 2. Unchanged apart from this note. */
/* adf - AmigaDOS volumes inside image files, read through core/arc.h.
 *
 * Floppy images (880K DD, 1760K HD, or any size that holds a DOS volume
 * from block 0) and hard disk images with a Rigid Disk Block (the first
 * DOS\0..DOS\7 partition is opened). The OFS and FFS file systems with
 * every variant: DOS\0/1 plain, DOS\2/3 international, DOS\4/5 directory
 * cache, DOS\6/7 long names (up to 107 characters, OS 3.2) - all of them
 * are read by walking the hash tables and chains, so the cache blocks are
 * never needed. The image is never loaded: a volume holds three block
 * buffers and a data buffer (8 blocks on FFS, so that blocks that follow
 * each other are read in one go - 5.5 KB in all; 2 KB on OFS) and reads
 * blocks through s->read.
 *
 * Every block goes through one reader (blk() in adf.c) that refuses
 * numbers outside the volume - the place a writer will add blk_write.
 * Chains are bounded (a block seen twice or out of range is
 * ARC_ECORRUPT, never a loop); checksums are not required.
 */
#ifndef CF_ADF_H
#define CF_ADF_H

#include "arc.h"

/* what adf_list puts in arcent.method */
enum {
    ADF_FILE = 0,
    ADF_DIR = 1,
    ADF_HLINK = 2,      /* a hard link: size, isdir, data are the target's;
                           date, prot, comment and name are the link's own */
    ADF_SLINK = 3       /* a soft link: a file whose bytes are the target path */
};

struct adfvol {
    struct arcsrc *src;
    unsigned long base;         /* byte offset of block 0 in the image */
    unsigned long nblocks;      /* blocks in the volume */
    unsigned long bsize;        /* bytes a block (512 on floppies) */
    unsigned long lo;           /* reserved (boot) blocks, never read as data */
    unsigned long root;         /* the root block's number */
    unsigned long tsize;        /* longs in a hash table / block list (72) */
    int dostype;                /* the n of DOS\n */
    int ffs, intl, lnfs;        /* the variant, from dostype */
    int hd;                     /* 1 = a 1760K floppy */
    char name[32];              /* the volume name (Latin-1) */
    char part[32];              /* the RDB partition opened ("" = none) */
    unsigned char *b[4];        /* block buffers: dir, entry, link/ext, data run */
    unsigned long run;          /* blocks b[3] holds (data runs on FFS) */
    /* the writer (core/adfw.c): its state, and how adf_close frees it */
    struct adfw *w;
    void (*wfree)(struct adfvol *v);
};

/* 0 or an ARC_ error. NDOS (games, trackloaders, PFS/SFS partitions):
   ARC_EUNSUP - "not a DOS disk"; DOS but no root block: ARC_ECORRUPT */
int adf_open(struct arcsrc *s, struct adfvol *v);
void adf_close(struct adfvol *v);
const char *adf_volname(struct adfvol *v);

/* one add() per entry of the directory at dirpath ("" = the root, "a/b"
   deeper; matched without case as AmigaDOS does). name = the entry's own
   name ('/' and ':' shown as '_', control characters as '?'), dataoff =
   its header block, aux1 = a link's target block, method = ADF_ */
int adf_list(struct adfvol *v, const char *dirpath, arc_addfn add, void *ctx);

/* a file's bytes into out; a hard link gives its target's, a soft link
   the path it points to; a directory is ARC_EUNSUP. In both calls a path
   that names nothing is ARC_ECORRUPT (the program only asks for names
   adf_list gave it) */
int adf_extract(struct adfvol *v, const char *path, struct arcsink *out);

/* for the writer (core/adfw.c) and the checker: block n into buf (one
   block; numbers outside the volume or in its boot blocks are
   ARC_ECORRUPT), the name hash (an index into a hash table), AmigaDOS's
   case folding of one character, and the name compare adf_list's names
   are found by ('/' ':' as '_', control characters as '?', no case) */
int adf_blk(struct adfvol *v, unsigned long n, unsigned char *buf);
unsigned long adf_hash(struct adfvol *v, const unsigned char *s, int n);
int adf_up(int c, int intl);
int adf_sameas(struct adfvol *v, const unsigned char *b, const char *s, int n);

#endif
