/* From CFile (core/arc.h) by Tobias Karlsson, contributed to FUZIX under
 * the GNU GPL version 2. Unchanged apart from this note. */
/* arc - the shape every built-in archive and image reader shares.
 *
 * Plain C, no Amiga calls, so each reader builds and is proven on the
 * host first (against the real tools) and then under vamos. The program
 * supplies the bytes (struct arcsrc), takes the output (struct arcsink)
 * and lends the memory (arc_alloc/arc_free) - on the Amiga that is
 * AllocMem, on the host malloc. A reader never reads outside its
 * buffers and never loops without end: corrupt or crafted input gives
 * an error code, never a crash or a hang.
 */
#ifndef CF_ARC_H
#define CF_ARC_H

/* bytes at off: returns how many were read (< n only at the end), < 0 on error */
typedef long (*arc_readfn)(void *ctx, unsigned long off, void *buf, long n);
/* output: returns 1 to go on, 0 to stop (a write error, or Esc) */
typedef int (*arc_writefn)(void *ctx, const void *buf, long n);

struct arcsrc {
    arc_readfn read;
    void *ctx;
    unsigned long size;                 /* the archive's size in bytes */
};

struct arcsink {
    arc_writefn write;
    void *ctx;
};

/* lent by the program: cleared memory, NULL when there is none */
void *arc_alloc(unsigned long n);
void arc_free(void *p);

/* one member as the readers report it */
struct arcent {
    char name[256];             /* the stored path, '/' between parts, no trailing '/' */
    unsigned long size;         /* unpacked bytes */
    unsigned long packed;       /* packed bytes (0 when it means nothing) */
    long days, mins, ticks;     /* an AmigaDOS datestamp; all 0 when unknown */
    unsigned long prot;         /* AmigaDOS protection bits (0 = ----rwed) */
    char comment[80];           /* a filenote, "" when none */
    unsigned char isdir;
    /* the format's own, for its extract call - opaque to the program */
    unsigned long dataoff;
    unsigned long aux1, aux2, aux3;
    int method;
};

/* results: 0 is success, the rest say why not */
enum {
    ARC_OK = 0,
    ARC_ECORRUPT = -1,          /* the structure cannot be valid */
    ARC_ECRC = -2,              /* unpacked, but the check value is wrong */
    ARC_EUNSUP = -3,            /* a method or variant this reader does not do */
    ARC_ENOMEM = -4,
    ARC_EREAD = -5,             /* the source could not be read */
    ARC_ESTOP = -6              /* the sink said stop */
};

/* the add callback of every list function: return 0 to stop listing */
typedef int (*arc_addfn)(void *ctx, const struct arcent *e);

/* plain words for an error code ("checksum error", ...) */
const char *arc_errstr(int e);

/* CRCs shared by the formats (tables built on first use, not stored) */
unsigned long arc_crc32(unsigned long crc, const void *buf, long n);    /* zip, LZX */
int arc_crcready(void);                 /* 1 = the CRC-32 table is there (0: no memory, sums are 0) */
unsigned int arc_crc16(unsigned int crc, const void *buf, long n);      /* lha, DMS (CRC-16/ARC) */
void arc_freetables(void);              /* at the end: the CRC tables go back */

/* AmigaDOS datestamp from a calendar date (1978-...); day/min/tick */
void arc_datestamp(int y, int mo, int d, int h, int mi, int s, long *days, long *mins, long *ticks);

#endif
