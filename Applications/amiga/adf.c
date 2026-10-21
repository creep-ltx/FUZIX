/* From CFile (core/adf.c) by Tobias Karlsson, contributed to FUZIX under
 * the GNU GPL version 2. Unchanged apart from this note. */
/* adf - see adf.h. No Amiga headers here, on purpose.
 *
 * The layout, for a block of B bytes (512 on a floppy; T = B/4 - 56
 * longs in a table, 72):
 *   header/root/ext   0 type (2 header, 16 extension), 4 own number,
 *                     8 table entries used, 24.. the table (hash table of
 *                     a directory; a file's data blocks, stored from the
 *                     END: the last long is the first block)
 *   B-192 protection, B-188 byte size, B-184 comment (BCPL, 79),
 *   B-92 date (days, mins, ticks), B-80 name (BCPL, 30), B-44 a hard
 *   link's target, B-16 next in the hash chain, B-12 parent,
 *   B-8 extension block, B-4 secondary type (1 root, 2 dir, -3 file,
 *   3 soft link, 4 dir link, -4 file link)
 *   long names (DOS\6/7): name then comment as two BCPL strings in the
 *   112 bytes at B-184; a comment that did not fit is in a comment
 *   block (type 64, text at 24) named at B-72; the date is at B-60
 *   OFS data block: 0 type 8, 4 header, 8 sequence (1..), 12 bytes used,
 *   16 next, 24.. the data. FFS data blocks are only data.
 * All of it is read byte by byte (big-endian, no unaligned access).
 */
#include "adf.h"

#define T_HEADER 2
#define T_DATA 8
#define T_LIST 16
#define T_COMMENT 64
#define ST_ROOT 1
#define ST_DIR 2
#define ST_SLINK 3
#define ST_DLINK 4
#define ST_FILE 0xfffffffdUL    /* -3 */
#define ST_FLINK 0xfffffffcUL   /* -4 */
#define RUN 8                   /* FFS: blocks read in one go */

static unsigned long g32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
           (unsigned long)p[2] << 8 | p[3];
}

static void mcopy(unsigned char *d, const unsigned char *s, unsigned long n)
{
    while (n--) *d++ = *s++;
}

/* ---- the one way to a block ---------------------------------------- */

/* cnt blocks from n into buf; numbers outside the volume (or in its boot
   blocks) are ARC_ECORRUPT. A writer adds its twin here. */
static int blkn(struct adfvol *v, unsigned long n, unsigned long cnt, unsigned char *buf)
{
    long len = (long)(cnt * v->bsize);
    if (n < v->lo || n >= v->nblocks || cnt > v->nblocks - n) return ARC_ECORRUPT;
    if (v->src->read(v->src->ctx, v->base + n * v->bsize, buf, len) != len) return ARC_EREAD;
    return ARC_OK;
}

static int blk(struct adfvol *v, unsigned long n, unsigned char *buf)
{
    return blkn(v, n, 1, buf);
}

/* a header block (file, dir, link) n of directory dir, read into buf */
static int hdr(struct adfvol *v, unsigned long n, unsigned long dir, unsigned char *buf)
{
    int r = blk(v, n, buf);
    if (r) return r;
    if (g32(buf) != T_HEADER || (g32(buf + 4) != n && n != v->root) ||
        (dir && g32(buf + v->bsize - 12) != dir)) return ARC_ECORRUPT;
    return ARC_OK;
}

/* ---- names ---------------------------------------------------------- */

/* AmigaDOS's case folding: ASCII, and Latin-1 on international volumes */
static int up(int c, int intl)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (intl && c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 32;
    return c;
}

static unsigned long hash(const unsigned char *s, int n, struct adfvol *v)
{
    unsigned long h = n;
    while (n--) h = ((h << 3) + (h << 2) + h + up(*s++, v->intl)) & 0x7ff;   /* *13 */
    return h % v->tsize;
}

/* where an entry's name is, and its length */
static const unsigned char *ename(struct adfvol *v, const unsigned char *b, int *len)
{
    const unsigned char *p = b + v->bsize - (v->lnfs ? 184 : 80);
    int n = *p, max = v->lnfs ? 107 : 30;
    *len = n > max ? max : n;
    return p + 1;
}

/* a BCPL string into a C one, made safe for a path */
static void cstr(char *d, const unsigned char *s, int n)
{
    while (n-- > 0) {
        int c = *s++;
        *d++ = c == '/' || c == ':' ? '_' : c < 0x20 ? '?' : c;
    }
    *d = 0;
}

/* the name as cstr shows it, against n bytes of a path */
static int sameas(struct adfvol *v, const unsigned char *b, const char *s, int n)
{
    int len, i;
    const unsigned char *p = ename(v, b, &len);
    if (len != n) return 0;
    for (i = 0; i < n; i++) {
        int c = p[i];
        c = c == '/' || c == ':' ? '_' : c < 0x20 ? '?' : c;
        if (up(c, v->intl) != up((unsigned char)s[i], v->intl)) return 0;
    }
    return 1;
}

/* ---- directories ----------------------------------------------------- */

/* the directory walk: every entry of the directory in b[0] (block dir)
   is read into b[1] and given to fn (1 = go on, 0 = stop). Chains are
   checked for loops (Brent: one saved number, no table) and the walk as
   a whole for steps; a bad entry ends its chain and the walk goes on.
   With every entry's parent checked, the only directory loop that can
   be reached from the root is a directory inside itself: refused too.
   Returns ARC_ESTOP when fn stopped, ARC_ECORRUPT when something was bad. */
typedef int (*entfn)(struct adfvol *v, void *ctx, unsigned long n);

static int walkdir(struct adfvol *v, unsigned long dir, entfn fn, void *ctx)
{
    unsigned long i, steps = 0;
    int bad = 0;
    for (i = 0; i < v->tsize; i++) {
        unsigned long n = g32(v->b[0] + 24 + 4 * i), tort = 0, pw = 1, lam = 0;
        while (n) {
            int r;
            if (n == tort || n == dir || ++steps > v->nblocks) {
                bad = 1;
                break;
            }
            if (lam == pw) {
                tort = n;
                pw <<= 1;
                lam = 0;
            }
            lam++;
            if ((r = hdr(v, n, dir, v->b[1]))) {
                if (r == ARC_EREAD) return r;
                bad = 1;
                break;
            }
            if (!fn(v, ctx, n)) return ARC_ESTOP;
            n = g32(v->b[1] + v->bsize - 16);
        }
    }
    return bad ? ARC_ECORRUPT : ARC_OK;
}

struct look { const char *s; int n; unsigned long found; };

static int lookfn(struct adfvol *v, void *ctx, unsigned long n)
{
    struct look *l = ctx;
    if (!sameas(v, v->b[1], l->s, l->n)) return 1;
    l->found = n;
    return 0;
}

/* a hard link in b[1] replaced by its target (which is never a link) */
static int follow(struct adfvol *v)
{
    unsigned long st = g32(v->b[1] + v->bsize - 4), t;
    int r;
    if (st != ST_FLINK && st != ST_DLINK) return ARC_OK;
    t = g32(v->b[1] + v->bsize - 44);
    if ((r = hdr(v, t, 0, v->b[2]))) return r;
    st = g32(v->b[2] + v->bsize - 4);
    if (st != ST_FILE && st != ST_DIR) return ARC_ECORRUPT;
    mcopy(v->b[1], v->b[2], v->bsize);
    return ARC_OK;
}

/* the entry at path into b[1] ("" = the root): its number, 0 on error *r.
   Each part is found by its hash chain first, then (a name made safe by
   cstr, or a volume hashed wrongly) by the whole directory. */
static unsigned long find(struct adfvol *v, const char *path, int *r)
{
    unsigned long dir = v->root, n;
    if ((*r = hdr(v, dir, 0, v->b[1]))) return 0;
    if (g32(v->b[1] + v->bsize - 4) != ST_ROOT) {
        *r = ARC_ECORRUPT;
        return 0;
    }
    for (;;) {
        struct look l;
        unsigned long st;
        while (*path == '/') path++;
        if (!*path) return dir;
        /* only a directory goes deeper */
        st = g32(v->b[1] + v->bsize - 4);
        if (st != ST_ROOT && st != ST_DIR) {
            *r = ARC_EUNSUP;
            return 0;
        }
        mcopy(v->b[0], v->b[1], v->bsize);
        l.s = path;
        for (l.n = 0; path[l.n] && path[l.n] != '/'; l.n++) ;
        l.found = 0;
        /* the hash chain (bounded like walkdir's) */
        {
            unsigned long tort = 0, pw = 1, lam = 0, steps = 0;
            n = g32(v->b[0] + 24 + 4 * hash((const unsigned char *)path, l.n, v));
            while (n && n != tort && ++steps <= v->nblocks) {
                if (lam == pw) {
                    tort = n;
                    pw <<= 1;
                    lam = 0;
                }
                lam++;
                if (hdr(v, n, dir, v->b[1])) break;
                if (sameas(v, v->b[1], path, l.n)) {
                    l.found = n;
                    break;
                }
                n = g32(v->b[1] + v->bsize - 16);
            }
        }
        if (!l.found) {
            *r = walkdir(v, dir, lookfn, &l);
            if (*r == ARC_EREAD) return 0;
            if (!l.found) {
                *r = ARC_ECORRUPT;      /* no such entry: the program listed it */
                return 0;
            }
        }
        /* (lookfn stopped with the entry in b[1]) */
        path += l.n;
        dir = l.found;
        while (*path == '/') path++;
        if (*path && (*r = follow(v))) return 0;
        if (*path) dir = g32(v->b[1] + 4);
    }
}

/* ---- for the writer --------------------------------------------------- */

int adf_blk(struct adfvol *v, unsigned long n, unsigned char *buf)
{
    return blk(v, n, buf);
}

unsigned long adf_hash(struct adfvol *v, const unsigned char *s, int n)
{
    return hash(s, n, v);
}

int adf_up(int c, int intl)
{
    return up(c, intl);
}

int adf_sameas(struct adfvol *v, const unsigned char *b, const char *s, int n)
{
    return sameas(v, b, s, n);
}

/* ---- listing --------------------------------------------------------- */

struct lst { arc_addfn add; void *ctx; struct arcent e; };

static int listfn(struct adfvol *v, void *ctx, unsigned long n)
{
    struct lst *l = ctx;
    struct arcent *e = &l->e;
    unsigned char *b = v->b[1];
    unsigned long B = v->bsize, st = g32(b + B - 4);
    const unsigned char *p;
    int len, c;

    p = ename(v, b, &len);
    cstr(e->name, p, len);
    e->comment[0] = 0;
    if (v->lnfs) {
        /* the comment follows the name, or is in its own block */
        c = p[len];
        if (c && len + 2 + c <= 112) cstr(e->comment, p + len + 1, c > 79 ? 79 : c);
        else if (!c && g32(b + B - 72)) {
            unsigned long cb = g32(b + B - 72);
            if (!blk(v, cb, v->b[3]) && g32(v->b[3]) == T_COMMENT &&
                g32(v->b[3] + 4) == cb && g32(v->b[3] + 8) == n) {
                c = v->b[3][24];
                cstr(e->comment, v->b[3] + 25, c > 79 ? 79 : c);
            }
        }
        p = b + B - 60;
    } else {
        c = b[B - 184];
        cstr(e->comment, b + B - 183, c > 79 ? 79 : c);
        p = b + B - 92;
    }
    e->days = (long)g32(p);
    e->mins = (long)g32(p + 4);
    e->ticks = (long)g32(p + 8);
    e->prot = g32(b + B - 192);
    e->dataoff = n;
    e->aux1 = e->aux2 = e->aux3 = 0;
    e->packed = 0;
    e->size = 0;
    e->isdir = 0;
    if (st == ST_DIR) {
        e->isdir = 1;
        e->method = ADF_DIR;
    } else if (st == ST_FILE) {
        e->size = g32(b + B - 188);
        e->method = ADF_FILE;
    } else if (st == ST_SLINK) {
        unsigned long k = 0;
        while (k < B - 224 && b[24 + k]) k++;
        e->size = k;
        e->method = ADF_SLINK;
    } else if (st == ST_FLINK || st == ST_DLINK) {
        unsigned long t = g32(b + B - 44), ts;
        e->method = ADF_HLINK;
        e->aux1 = t;
        if (hdr(v, t, 0, v->b[2])) return -1;
        ts = g32(v->b[2] + B - 4);
        if (ts == ST_DIR) e->isdir = 1;
        else if (ts == ST_FILE) e->size = g32(v->b[2] + B - 188);
        else return -1;
    } else return -1;
    return l->add(l->ctx, e) ? 1 : 0;
}

/* listfn's -1 (an entry that cannot be) is counted, not passed on */
struct lst2 { struct lst l; int bad; };

static int listfn2(struct adfvol *v, void *ctx, unsigned long n)
{
    struct lst2 *l = ctx;
    int r = listfn(v, &l->l, n);
    if (r < 0) {
        l->bad = 1;
        return 1;
    }
    return r;
}

int adf_list(struct adfvol *v, const char *dirpath, arc_addfn add, void *ctx)
{
    struct lst2 l;
    unsigned long dir, st;
    int r;
    if (!(dir = find(v, dirpath, &r))) return r;
    if ((r = follow(v))) return r;
    st = g32(v->b[1] + v->bsize - 4);
    if (st != ST_ROOT && st != ST_DIR) return ARC_EUNSUP;
    if (st == ST_DIR) dir = g32(v->b[1] + 4);
    mcopy(v->b[0], v->b[1], v->bsize);
    l.l.add = add;
    l.l.ctx = ctx;
    l.bad = 0;
    r = walkdir(v, dir, listfn2, &l);
    return r ? r : l.bad ? ARC_ECORRUPT : ARC_OK;
}

/* ---- reading a file -------------------------------------------------- */

int adf_extract(struct adfvol *v, const char *path, struct arcsink *out)
{
    unsigned long B = v->bsize, key, st, size, per, ext, seq = 0;
    unsigned long tort = 0, pw = 1, lam = 0;
    unsigned char *cur;
    int r;
    if (!(key = find(v, path, &r))) return r;
    if ((r = follow(v))) return r;
    key = g32(v->b[1] + 4);
    st = g32(v->b[1] + B - 4);
    if (st == ST_SLINK) {
        unsigned long k = 0;
        while (k < B - 224 && v->b[1][24 + k]) k++;
        return !k || out->write(out->ctx, v->b[1] + 24, (long)k) ? ARC_OK : ARC_ESTOP;
    }
    if (st != ST_FILE) return ARC_EUNSUP;
    size = g32(v->b[1] + B - 188);
    per = v->ffs ? B : B - 24;
    if (size / per >= v->nblocks) return ARC_ECORRUPT;
    cur = v->b[1];
    while (size) {
        unsigned long cnt = g32(cur + 8), k = 0;
        if (cnt > v->tsize) cnt = v->tsize;
        while (k < cnt && size) {
            const unsigned char *t = cur + 24 + 4 * (v->tsize - 1 - k);
            unsigned long n = g32(t), got, run = 1;
            const unsigned char *d = v->b[3];
            if (v->ffs) {
                /* blocks that follow each other: one read */
                while (run < v->run && k + run < cnt && (unsigned long)run * B < size &&
                       g32(t - 4 * run) == n + run) run++;
                if ((r = blkn(v, n, run, v->b[3]))) return r;
                got = run * B;
            } else {
                if ((r = blk(v, n, v->b[3]))) return r;
                if (g32(d) != T_DATA || g32(d + 4) != key || g32(d + 8) != ++seq)
                    return ARC_ECORRUPT;
                got = g32(d + 12);
                if (!got || got > B - 24) return ARC_ECORRUPT;
                d += 24;
            }
            if (got > size) got = size;
            if (!out->write(out->ctx, d, (long)got)) return ARC_ESTOP;
            size -= got;
            k += run;
        }
        if (!size) break;
        /* the next extension block; its chain checked for loops too */
        ext = g32(cur + B - 8);
        if (!ext || ext == tort) return ARC_ECORRUPT;
        if (lam == pw) {
            tort = ext;
            pw <<= 1;
            lam = 0;
        }
        lam++;
        if ((r = blk(v, ext, v->b[2]))) return r;
        cur = v->b[2];
        if (g32(cur) != T_LIST || g32(cur + 4) != ext || g32(cur + B - 12) != key ||
            !g32(cur + 8)) return ARC_ECORRUPT;
    }
    return ARC_OK;
}

/* ---- opening ------------------------------------------------------- */

static int isroot(struct adfvol *v, unsigned long n)
{
    return !hdr(v, n, 0, v->b[0]) && g32(v->b[0] + v->bsize - 4) == ST_ROOT;
}

/* the first DOS partition of a Rigid Disk Block found in b (block at),
   into v's geometry; ARC_EUNSUP when there is none */
static int rdb(struct adfvol *v, unsigned char *b)
{
    struct arcsrc *s = v->src;
    unsigned long n = g32(b + 28), i;
    if (g32(b + 16) != 512) return ARC_EUNSUP;    /* rdb_BlockBytes */
    for (i = 0; i < 128 && n != 0xffffffffUL && n; i++) {
        const unsigned char *e = b + 128;            /* the DosEnvec */
        unsigned long sz, sur, spb, bpt, low, high, bpc, dt;
        if (n >= s->size / 512 || s->read(s->ctx, n * 512, b, 512) != 512) return ARC_ECORRUPT;
        if (g32(b) != 0x50415254UL) return ARC_ECORRUPT;    /* "PART" */
        n = g32(b + 16);
        sz = g32(e + 4);
        sur = g32(e + 12);
        spb = g32(e + 16);
        bpt = g32(e + 20);
        low = g32(e + 36);
        high = g32(e + 40);
        dt = g32(e) >= 16 ? g32(e + 64) : 0x444f5300UL;
        if (spb == 0) spb = 1;
        if ((dt >> 8) != 0x444f53UL || (dt & 255) > 7) continue;
        if (sz < 128 || sz > 8192 || spb > 64 || sz * 4 * spb > 32768 ||
            !sur || sur > 256 || !bpt || bpt > 65536 || high < low) continue;
        v->bsize = sz * 4 * spb;
        bpc = sur * bpt / spb;                       /* blocks a cylinder */
        if (!bpc || bpc > 0xffffffffUL / v->bsize ||
            low > s->size / (bpc * v->bsize)) continue;
        v->base = low * bpc * v->bsize;
        /* clipped to the image */
        v->nblocks = (s->size - v->base) / v->bsize;
        if (high - low < v->nblocks / bpc) v->nblocks = (high - low + 1) * bpc;
        v->lo = g32(e + 24);
        if (v->lo < 1 || v->lo > 1024) v->lo = 2;
        cstr(v->part, b + 37, b[36] > 31 ? 31 : b[36]);
        return ARC_OK;
    }
    return ARC_EUNSUP;
}

void adf_close(struct adfvol *v)
{
    int i;
    if (v->wfree) v->wfree(v);
    v->wfree = 0;
    for (i = 0; i < 4; i++) {
        if (v->b[i]) arc_free(v->b[i]);
        v->b[i] = 0;
    }
}

const char *adf_volname(struct adfvol *v)
{
    return v->name;
}

int adf_open(struct arcsrc *s, struct adfvol *v)
{
    unsigned char *b;
    unsigned long i;
    int r, len;

    for (i = 0; i < sizeof *v; i++) ((char *)v)[i] = 0;
    v->src = s;
    v->bsize = 512;
    v->lo = 2;
    if (s->size < 1024) return ARC_EUNSUP;
    if (!(b = v->b[0] = arc_alloc(512))) return ARC_ENOMEM;
    /* a hard disk image: "RDSK" in one of its first 16 blocks */
    for (i = 0, len = 0; !len && i < 16 && (i + 1) * 512 <= s->size; i++) {
        if (s->read(s->ctx, i * 512, b, 512) != 512) {
            r = ARC_EREAD;
            goto fail;
        }
        len = g32(b) == 0x5244534bUL;
    }
    if (len) {
        if ((r = rdb(v, b))) goto fail;
    } else
        v->nblocks = s->size / 512;
    if (s->read(s->ctx, v->base, b, 4) != 4) {
        r = ARC_EREAD;
        goto fail;
    }
    if (b[0] != 'D' || b[1] != 'O' || b[2] != 'S' || b[3] > 7 || v->nblocks < 4) {
        r = ARC_EUNSUP;
        goto fail;
    }
    v->dostype = b[3];
    v->ffs = b[3] & 1;
    v->intl = b[3] >= 2;            /* DOS\4..7 are international too */
    v->lnfs = b[3] >= 6;
    v->hd = !v->base && s->size == 1802240UL;
    v->tsize = v->bsize / 4 - 56;
    /* the buffers, now that the block size is known */
    if (v->bsize != 512) {
        arc_free(b);
        v->b[0] = 0;
    }
    for (i = 0; i < 3; i++)
        if (!v->b[i] && !(v->b[i] = arc_alloc(v->bsize))) {
            r = ARC_ENOMEM;
            goto fail;
        }
    v->run = v->ffs ? RUN : 1;
    if (!(v->b[3] = arc_alloc(v->run * v->bsize))) {
        v->run = 1;
        if (!(v->b[3] = arc_alloc(v->bsize))) {
            r = ARC_ENOMEM;
            goto fail;
        }
    }
    /* the root is in the middle; a floppy's boot block names it too */
    v->root = (v->nblocks - 1 + v->lo) / 2;
    if (!isroot(v, v->root)) {
        unsigned long lo = v->lo;
        v->lo = 0;
        r = blk(v, 0, v->b[1]);
        v->lo = lo;
        if (r == ARC_EREAD) goto fail;
        if (r || !isroot(v, v->root = g32(v->b[1] + 8))) {
            r = ARC_ECORRUPT;
            goto fail;
        }
    }
    {
        const unsigned char *p = v->b[0] + v->bsize - 80;
        len = *p > 30 ? 30 : *p;
        cstr(v->name, p + 1, len);
    }
    return ARC_OK;
fail:
    adf_close(v);
    return r;
}
