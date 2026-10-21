/*
 *	Text screens: 640x256 PAL hires, 80x32 characters of 8x8 (640x200
 *	and 80x25 on NTSC, 640x480 and 80x60 in the 31kHz mode), one per
 *	virtual console (tty1-tty4), all at the top of chip RAM. The Copper
 *	shows one of them; Amiga+F1..F4 picks which.
 *
 *	Screen modes (screenmode, VTSETMODE): PAL, NTSC and 640x480 at
 *	31kHz ("Productivity", the timings of Linux's amifb "multiscan").
 *	The machine starts in its own; another comes from /etc/screenmode
 *	at boot. Memory for the largest mode the chips can show is kept at
 *	boot; the first VTSETMODE (from /etc/rc) gives back what the chosen
 *	mode does not use.
 *
 *	AGA machines get four bitplanes (16 colours, the vt's ink and paper)
 *	fetched 64 bits at a time, so the display leaves the CPU most of the
 *	chip bus; older chips get one plane, as before.
 *
 *	Each plane of a screen is a block of its own (see planebytes).
 *
 *	Scrolling copies nothing: each screen is a ring of text rows and
 *	the Copper starts the display at the top row, then jumps back to
 *	the buffer's start where the ring wraps. Clearing rows is the
 *	blitter's job.
 */
#include <kernel.h>
#include <kdata.h>
#include <vt.h>
#include "amiga.h"

#define COLS		80
#define ROWS		vt_rows
#define MAXPLANES	4
#define COPWORDS	48		/* one Copper list */

#define MODE_PAL	0
#define MODE_NTSC	1
#define MODE_VGA	2		/* 640x480, 31kHz */
#define NMODES		3

uint32_t video_base;			/* the lowest byte kept for screens */
uint8_t ntsc;				/* the machine's own video is NTSC */
uint8_t vt_rows = 32;
uint8_t lines_per_ms = 16;
uint32_t frame_mhz = 50000;

static uint8_t mode, native, avail;
static uint8_t committed;		/* the spare memory was given back */
static uint8_t beam_set;		/* BEAMCON0 written by us */
static uint32_t screens_top;		/* the screens end here, the Copper
					   lists follow */
/* The Copper: the first displayed line, the position after a line's
   last bitplane fetch (pointers can change from there), one near the
   line's end (to get past lines 255 and 511) */
static uint16_t first_line;
static uint8_t hend, hlast;
/* 640x480 in four planes has no time between two lines for all eight
   pointer moves: where the ring wraps, planes 0-1 move after one line
   and 2-3 after the next, and those show a copy of the ring's first
   pixel line kept just after the ring (the guard line) */
static uint8_t split;

static uint8_t planes;			/* 4 on AGA, else 1 */
/* Each plane of a screen is a block of its own, lines COLS bytes apart
   (the modulo is 0: FS-UAE adds the modulo too early in 31kHz super
   hires, which only shows when it is not 0) */
#define rowbytes	(COLS * 8)	/* one text row of one plane */
static uint32_t planebytes;		/* one plane of a screen */
static uint32_t screenbytes;

struct vscreen {
	uint8_t *base;
	uint8_t top;			/* the ring row shown as row 0 */
	int8_t cur_y, cur_x;		/* the cursor drawn, or -1 */
};
static struct vscreen vs[MAX_VT];
static struct vscreen *act = vs;	/* the one being written */
static uint8_t shown;			/* the one on display */

/* Two Copper lists: a new one is built in the list the Copper is not
   running, and takes over at the next vertical blank */
static uint16_t *coplist[2];
static volatile uint8_t cop_run, cop_next;

/* Per plane: what a set and a clear font bit become (0x00 or 0xFF) */
static uint8_t fg[MAXPLANES], bg[MAXPLANES];

extern const unsigned char fontdata_8x8[];

/* The console font: Fuzix's own at boot, replaceable with VTSETFONT */
uint8_t vtfont[2048];
/* Fuzix's own font in the Latin-1 order the keymaps and Topaz use. The
   font is the PC's code page 437, which has most Latin-1 letters at
   other places: move them. A letter it lacks (Ã, Õ) gets the plain one. */
uint8_t fuzixfont[2048];
static const uint8_t latin1_cp437[96] = {
	0x20, 0xAD, 0x9B, 0x9C, 0x2A, 0x9D, 0x7C, 0x15, 0x22, 0x63, 0xA6, 0xAE,
	0xAA, 0x2D, 0x52, 0x2D, 0xF8, 0xF1, 0xFD, 0x33, 0x27, 0xE6, 0x14, 0xFA,
	0x2C, 0x31, 0xA7, 0xAF, 0xAC, 0xAB, 0x3F, 0xA8, 0x41, 0x41, 0x41, 0x41,
	0x8E, 0x8F, 0x92, 0x80, 0x45, 0x90, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49,
	0x44, 0xA5, 0x4F, 0x4F, 0x4F, 0x4F, 0x99, 0x78, 0xED, 0x55, 0x55, 0x55,
	0x9A, 0x59, 0x50, 0xE1, 0x85, 0xA0, 0x83, 0x61, 0x84, 0x86, 0x91, 0x87,
	0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B, 0x64, 0xA4, 0x95, 0xA2,
	0x93, 0x6F, 0x94, 0xF6, 0xED, 0x97, 0xA3, 0x96, 0x81, 0x79, 0x70, 0x98,
};
/* The Kickstart ROM's Topaz 8 as the same 256 x 8 byte table */
uint8_t romfont[2048];
uint8_t have_romfont;

/*
 *	Find Topaz 8 in the Kickstart ROM: the string "topaz.font", then the
 *	TextFont that names it. 2.0 and later keep the usual layout; 1.3's
 *	ROM copy is 4 bytes shorter in front (no mn_ReplyPort), so try both.
 *	Checked on Kickstart 1.3, 2.04, 3.1 (A1200 and A500) and 3.2.1.
 */
#define ROM_END		0x1000000UL

static uint32_t rom_start = 0xF80000UL;

#define TF_YSIZE	20
#define TF_XSIZE	24
#define TF_LOCHAR	32
#define TF_HICHAR	33
#define TF_CHARDATA	34
#define TF_MODULO	38
#define TF_CHARLOC	40

static uint16_t rd16(uint32_t a)
{
	return *(volatile uint16_t *)a;
}

static uint32_t rd32(uint32_t a)
{
	return ((uint32_t)rd16(a) << 16) | rd16(a + 2);
}

static uint8_t in_rom(uint32_t a)
{
	return a >= rom_start && a < ROM_END;
}

static uint8_t rom_font_from(uint32_t tf)
{
	uint8_t lo, hi;
	uint32_t data, charloc;
	uint16_t modulo, ch, y;

	if (rd16(tf + TF_YSIZE) != 8 || rd16(tf + TF_XSIZE) != 8)
		return 0;
	lo = *(volatile uint8_t *)(tf + TF_LOCHAR);
	hi = *(volatile uint8_t *)(tf + TF_HICHAR);
	data = rd32(tf + TF_CHARDATA);
	modulo = rd16(tf + TF_MODULO);
	charloc = rd32(tf + TF_CHARLOC);
	if (lo > 32 || hi < 126 || !in_rom(data) || !in_rom(charloc) ||
	    modulo == 0 || modulo > 512)
		return 0;

	memset(romfont, 0, sizeof(romfont));
	for (ch = lo; ch < 256; ch++) {
		/* Characters past the end or without a glyph get the
		   font's "no such character" glyph, which follows hichar */
		uint16_t idx = ch - lo;
		uint16_t bitoff, width;
		if (ch > hi || rd16(charloc + idx * 4 + 2) == 0)
			idx = hi + 1 - lo;
		bitoff = rd16(charloc + idx * 4);
		width = rd16(charloc + idx * 4 + 2);
		if (width == 0)
			continue;
		for (y = 0; y < 8; y++) {
			uint32_t row = data + (uint32_t)y * modulo + (bitoff >> 3);
			uint16_t pair = (*(volatile uint8_t *)row << 8) |
					*(volatile uint8_t *)(row + 1);
			uint8_t b = pair >> (8 - (bitoff & 7));
			if (width < 8)
				b &= 0xFF << (8 - width);
			romfont[(ch << 3) + y] = b;
		}
	}
	return 1;
}

/* Every TextFont naming the string at n: ln_Name is 10 bytes in, and in
   1.3's shorter copy tf_YSize is 6 bytes after the name, not 10 */
static uint8_t font_naming(uint32_t n)
{
	uint32_t r;

	for (r = rom_start + 16; r < ROM_END - 64; r += 2) {
		if (rd32(r) != n)
			continue;
		if (rom_font_from(r - 10) || rom_font_from(r - 14))
			return 1;
	}
	return 0;
}

static uint8_t find_rom_font(void)
{
	static const char name[] = "topaz.font";
	uint32_t a;

	/* The ROM's size is stored 20 bytes from its end: 256K ROMs
	   (1.x) sit at $FC0000 */
	a = rd32(ROM_END - 0x14);
	if (a == 0x40000UL || a == 0x80000UL)
		rom_start = ROM_END - a;

	/* The name can appear more than once (2.04 has two copies) */
	for (a = rom_start; a < ROM_END - sizeof(name); a++) {
		if (*(volatile uint8_t *)a != 't')
			continue;
		if (memcmp((void *)a, name, sizeof(name)) == 0 &&
		    font_naming(a))
			return 1;
	}
	return 0;
}

/* The IGRB colours: 1 blue, 2 red, 4 green, 8 bright */
static uint16_t igrb(uint8_t i)
{
	uint16_t on = (i & 8) ? 0xF : 0xA;
	uint16_t off = (i & 8) ? 0x5 : 0x0;

	return ((i & 2) ? on : off) << 8 | ((i & 4) ? on : off) << 4 |
	       ((i & 1) ? on : off);
}

static void blit_wait(void)
{
	(void)DMACONR;			/* the first read can be early */
	while (DMACONR & 0x4000)	/* BBUSY */
		;
}

static uint16_t *set_planes(uint16_t *c, uint8_t *p)
{
	uint8_t k;
	uint32_t a;

	for (k = 0; k < planes; k++) {
		a = (uint32_t)p + k * planebytes;
		*c++ = 0x00E0 + 4 * k; *c++ = a >> 16;
		*c++ = 0x00E2 + 4 * k; *c++ = a & 0xFFFF;
	}
	return c;
}

/* Planes from..to-1 of a line at p */
static uint16_t *set_some(uint16_t *c, uint8_t *p, uint8_t from, uint8_t to)
{
	uint32_t a;

	for (; from < to; from++) {
		a = (uint32_t)p + from * planebytes;
		*c++ = 0x00E0 + 4 * from; *c++ = a >> 16;
		*c++ = 0x00E2 + 4 * from; *c++ = a & 0xFFFF;
	}
	return c;
}

/* Wait for a line after its last fetch. The Copper compares only the
   low 8 bits of the line: first wait out lines 255 and 511 */
static uint16_t *cwait(uint16_t *c, uint16_t line, uint16_t *passed)
{
	while (line >= *passed + 256) {
		*c++ = 0xFF01 | hlast; *c++ = 0xFFFE;
		*passed += 256;
	}
	*c++ = (line & 0xFF) << 8 | hend | 1; *c++ = 0xFFFE;
	return c;
}

/* A Copper list for the screen on display: its top row first, and
   where the ring wraps (at the end of the line before, after that
   line's fetch) the buffer's start */
static void copper_build(void)
{
	struct vscreen *v = vs + shown;
	irqflags_t irq;
	uint16_t *c;
	uint16_t line, passed = 0;
	uint8_t n;

	irq = di();
	n = cop_run ^ 1;
	c = coplist[n];
	c = set_planes(c, v->base + v->top * rowbytes);
	if (v->top) {
		line = first_line + (ROWS - v->top) * 8 - 1;
		c = cwait(c, line, &passed);
		if (split) {
			c = set_some(c, v->base, 0, 2);
			c = cwait(c, line + 1, &passed);
			c = set_some(c, v->base + COLS, 2, 4);
		} else
			c = set_planes(c, v->base);
	}
	*c++ = 0xFFFF; *c++ = 0xFFFE;
	COP1LCH = (uint32_t)coplist[n] >> 16;
	COP1LCL = (uint32_t)coplist[n] & 0xFFFF;
	cop_next = n;
	irqrestore(irq);
}

/* At the vertical blank the Copper starts the list COP1LC names */
void video_vblank(void)
{
	cop_run = cop_next;
}

/* Lisa answers 0xF8, ECS Denise 0xFC; OCS Denise has no id register
   and gives whatever was last on the bus */
static uint8_t denise_id(void)
{
	uint8_t i, id = LISAID & 0xFF;

	for (i = 0; i < 16; i++)
		if ((LISAID & 0xFF) != id)
			return 0;
	return id;
}

/* The beam's last line: 312 on PAL, 262 on NTSC. That is the machine's
   own video, or on ECS and AGA what Kickstart was told (Early Startup's
   display option): the chips stay as AmigaOS left them. */
static uint8_t is_ntsc(void)
{
	volatile uint32_t *vpos = (volatile uint32_t *)0xDFF004;
	uint16_t v, last = 0, max = 0;
	uint8_t wraps = 0;
	uint32_t i;

	/* Two frames' worth (a long and a short one when interlaced) */
	for (i = 0; i < 1000000UL && wraps < 3; i++) {
		v = (*vpos >> 8) & 0x1FF;
		if (v < last)
			wraps++;
		else if (wraps && v > max)
			max = v;
		last = v;
	}
	return max && max < 280;
}

static uint8_t mode_rows(uint8_t m)
{
	return m == MODE_VGA ? 60 : m == MODE_NTSC ? 25 : 32;
}

/* One plane of a screen in mode m: the rows, the guard line if split
   (a multiple of 8 bytes, for the 64-bit fetches) */
static uint32_t plane_bytes(uint8_t m)
{
	uint32_t n = (uint32_t)mode_rows(m) * rowbytes;

	if (m == MODE_VGA && planes == 4)
		n += COLS;
	return n;
}

static uint32_t mode_bytes(uint8_t m)
{
	return planes * plane_bytes(m);
}

/* The beam and the bitplane fetch for the mode in "mode" */
static void set_display(void)
{
	uint8_t aga = planes == 4;

	if (mode == MODE_VGA) {
		/* amifb's "multiscan": 115 colour clocks a line (30.8kHz),
		   525 lines (58.7Hz on a PAL crystal), super hires */
		HTOTAL = 0x0072;
		HSSTRT = 0x000E;
		HSSTOP = 0x0017;
		HBSTRT = aga ? 0x0301 : 0x0072;
		HBSTOP = aga ? 0x0324 : 0x0022;
		HCENTER = 0x0047;
		VTOTAL = 0x020C;
		VSSTRT = 0x0008;
		VSSTOP = 0x0010;
		VBSTRT = 0x020C;
		VBSTOP = 0x002C;
		/* HARDDIS VARVBEN LOLDIS VARVSYEN VARHSYEN VARBEAMEN PAL
		   VARCSYEN; syncs active low */
		BEAMCON0 = 0x5BB0;
		beam_set = 1;
		BPLCON0 = aga ? 0x4241 : 0x1241;	/* shres, ECSENA */
		FMODE = aga ? 3 : 0;
		DDFSTRT = aga ? 0x0018 : 0x001E;
		DDFSTOP = aga ? 0x0060 : 0x0066;
		BPLCON1 = aga ? 0x3366 : 0x0022;
		BPLCON3 = aga ? 0x0CC1 : 0x0001;	/* EXTBLKEN */
		DIWSTRT = aga ? 0x2D47 : 0x2D43;
		DIWSTOP = aga ? 0x0DE7 : 0x0DE3;
		DIWHIGH = aga ? 0x1A18 : 0x0200;
		first_line = 0x2D;
		hend = aga ? 0x68 : 0x6E;
		hlast = 0x70;
		lines_per_ms = 31;
		frame_mhz = colour_clock * 1000UL / (115UL * 525);
		split = aga;
	} else {
		/* PAL 227 colour clocks x 313 lines, NTSC 227.5 x 263 */
		if (beam_set || mode != native)
			BEAMCON0 = mode == MODE_PAL ? 0x0020 : 0;
		if (aga) {
			BPLCON0 = 0xC200;	/* hires, four planes, colour */
			FMODE = 3;		/* 64-bit bitplane fetches */
			DDFSTRT = 0x0030;	/* the hires values for FMODE 3 */
			DDFSTOP = 0x00C0;
		} else {
			BPLCON0 = 0x9200;	/* hires, one plane, colour */
			FMODE = 0;
			DDFSTRT = 0x003C;
			DDFSTOP = 0x00D4;
		}
		BPLCON1 = 0;
		BPLCON3 = 0x0C00;	/* AGA: ECS-compatible defaults */
		/* (writing these turns DIWHIGH off) */
		DIWSTRT = 0x2C81;
		DIWSTOP = mode == MODE_NTSC ? 0xF4C1 : 0x2CC1;
		first_line = 0x2C;
		hend = 0xE0;
		hlast = 0xDE;
		lines_per_ms = 16;
		frame_mhz = mode == MODE_NTSC ?
			colour_clock * 1000UL / 59833 :	/* 227.5 x 263 */
			colour_clock * 1000UL / (227UL * 313);
		split = 0;
	}
	BPLCON2 = 0;
	BPLCON4 = 0x0011;
	BPL1MOD = 0;
	BPL2MOD = 0;
}

/* Go to mode m: every screen is cleared, its cursor forgotten */
static void apply_mode(uint8_t m)
{
	uint32_t *p, n;
	uint8_t i;

	mode = m;
	vt_rows = mode_rows(m);
	planebytes = plane_bytes(m);
	screenbytes = mode_bytes(m);
	DMACON = 0x0100;			/* bitplanes off meanwhile */
	blit_wait();
	for (i = 0; i < MAX_VT; i++) {
		vs[i].base = (uint8_t *)(screens_top - (i + 1) * screenbytes);
		vs[i].top = 0;
		vs[i].cur_y = -1;
	}
	p = (uint32_t *)vs[MAX_VT - 1].base;
	for (n = MAX_VT * screenbytes / 4; n; n--)
		*p++ = 0;
	set_display();
	copper_build();
	cop_run = cop_next;
	COPJMP1 = 0;
	DMACON = 0x8100;
}

void video_init(void)
{
	uint32_t need, most = 0;
	uint16_t i;
	uint8_t id, m;

	ntsc = is_ntsc();
	native = ntsc ? MODE_NTSC : MODE_PAL;
	id = denise_id();
	planes = id == 0xF8 ? 4 : 1;


	/* PAL and NTSC need an ECS Agnus (VPOSR id bit 5) to change; the
	   31kHz mode AGA for now. (ECS Denise has super hires too, and the
	   ECS values below are amifb's, but an A600 in FS-UAE and Amiberry
	   showed them cut short or garbled: not offered until that is
	   understood.) */
	avail = 1 << native;
	if ((CUSTOM(0x004) >> 8) & 0x20) {
		avail |= 1 << MODE_PAL | 1 << MODE_NTSC;
		if (planes == 4)
			avail |= 1 << MODE_VGA;
	}

	/* The Copper lists at the top of chip RAM, the screens below,
	   room for the largest mode below them */
	coplist[0] = (uint16_t *)((chip_top - 4 * COPWORDS) & ~7UL);
	coplist[1] = coplist[0] + COPWORDS;
	screens_top = (uint32_t)coplist[0];
	for (m = 0; m < NMODES; m++)
		if (avail & (1 << m)) {
			need = MAX_VT * mode_bytes(m);
			if (need > most)
				most = need;
		}
	video_base = (screens_top - most) & ~0xFFUL;

	if (planes == 4)
		for (m = 0; m < 16; m++)
			COLOR(m) = igrb(m);
	else {
		COLOR(0) = 0x0000;
		COLOR(1) = 0x0CCC;
	}
	cop_run = 1;
	apply_mode(native);
	/* SET | DMAEN | BPLEN | COPEN | BLTEN */
	DMACON = 0x8000 | 0x0200 | 0x0100 | 0x0080 | 0x0040;

	memcpy(fuzixfont, fontdata_8x8, 0x80 * 8);
	memset(fuzixfont + 0x80 * 8, 0, 0x20 * 8);	/* C1 controls */
	for (i = 0; i < 96; i++)
		memcpy(fuzixfont + (0xA0 + i) * 8,
		       fontdata_8x8 + latin1_cp437[i] * 8, 8);
	memcpy(vtfont, fuzixfont, sizeof(vtfont));
	have_romfont = find_rom_font();

	vtattr_cap = VTA_INVERSE | VTA_UNDERLINE;
	if (planes > 1)
		vtattr_cap |= VTA_BOLD;
	vtinit();
}

/* Writing goes to screen n (tty n + 1) */
void set_active_vt(uint8_t n)
{
	act = vs + n;
}

/* Screen n goes on display */
void set_visible_vt(uint8_t n)
{
	shown = n;
	copper_build();
}

/* The first VTSETMODE: the memory kept for a larger mode than the one
   in use goes to the programs */
static void commit(void)
{
	uint32_t low = screens_top - MAX_VT * screenbytes;

	committed = 1;
	low &= ~0xFFUL;
	if (low > video_base + 4096) {
		kmemaddblk((void *)video_base, low - video_base);
		video_base = low;
		procmem = (kmemavail() + kmemused()) >> 10;
	}
}

/* What commit() will give back if the machine's own mode stays */
uint32_t video_spare(void)
{
	return ((screens_top - MAX_VT * mode_bytes(native)) & ~0xFFUL) -
		video_base;
}

/* VTSETMODE: 0 or an error. At boot (from /etc/rc) the left mouse
   button held keeps the machine's own mode, and the spare memory is
   given back whatever happens. Called with interrupts off. */
int video_set_mode(uint8_t m, uint8_t boot)
{
	int err = 0;

	if (m >= NMODES || !(avail & (1 << m)))
		err = EINVAL;
	else if (boot && m != native && !(CIAA(0) & 0x40))
		err = EBUSY;
	else if (MAX_VT * mode_bytes(m) > screens_top - video_base)
		err = ENOMEM;		/* only after a restart */
	else if (m != mode)
		apply_mode(m);
	if (boot && !committed)
		commit();
	return err;
}

/* VTGETMODE: the mode, the machine's own << 4, the ones it can show
   << 8, the ones that fit now (without a restart) << 12 */
uint16_t video_get_mode(void)
{
	uint16_t fit = 0;
	uint8_t m;

	for (m = 0; m < NMODES; m++)
		if ((avail & (1 << m)) &&
		    MAX_VT * mode_bytes(m) <= screens_top - video_base)
			fit |= 1 << m;
	return mode | native << 4 | (uint16_t)avail << 8 | fit << 12;
}

/* The guard line (split) is a copy of the ring's first pixel line */
static void guard_sync(struct vscreen *v)
{
	uint8_t k;

	if (!split)
		return;
	blit_wait();
	for (k = 0; k < planes; k++)
		memcpy(v->base + k * planebytes + (uint16_t)ROWS * rowbytes,
		       v->base + k * planebytes, COLS);
}

static uint8_t ring_row(int8_t y)
{
	uint8_t r = y + act->top;

	if (r >= ROWS)
		r -= ROWS;
	return r;
}

static uint8_t *cell(int8_t y, int8_t x)
{
	return act->base + (uint16_t)ring_row(y) * rowbytes + x;
}

static void invert_cell(int8_t y, int8_t x)
{
	uint8_t *p = cell(y, x);
	uint8_t i, k;

	blit_wait();
	for (i = 0; i < 8; i++) {
		for (k = 0; k < planes; k++)
			p[k * planebytes] ^= 0xFF;
		p += COLS;
	}
	if (ring_row(y) == 0)
		guard_sync(act);
}

void plot_char(int8_t y, int8_t x, uint16_t c)
{
	uint8_t *p = cell(y, x);
	const uint8_t *f = vtfont + ((c & 0xFF) << 3);
	uint8_t inv = (vtattr & VTA_INVERSE) ? 0xFF : 0;
	uint8_t i, k, b;

	blit_wait();
	for (i = 0; i < 8; i++) {
		b = *f++;
		if (i == 7 && (vtattr & VTA_UNDERLINE))
			b = 0xFF;
		b ^= inv;
		if (planes == 1)
			*p = b;
		else
			for (k = 0; k < MAXPLANES; k++)
				p[k * planebytes] = (b & fg[k]) | (~b & bg[k]);
		p += COLS;
	}
	if (ring_row(y) == 0)
		guard_sync(act);
}

/* Ring rows r to r + n - 1 (no wrap) to the paper colour: one blit a
   plane, the A channel off so its data register is the constant */
static void blit_clear(uint8_t r, uint8_t n)
{
	uint8_t *p = act->base + (uint16_t)r * rowbytes;
	uint8_t k;

	for (k = 0; k < planes; k++) {
		blit_wait();
		BLTCON0 = 0x01F0;		/* D = A */
		BLTCON1 = 0;
		BLTAFWM = 0xFFFF;
		BLTALWM = 0xFFFF;
		BLTADAT = bg[k] ? 0xFFFF : 0;
		BLTDMOD = 0;
		BLTDPTH = (uint32_t)(p + k * planebytes) >> 16;
		BLTDPTL = (uint32_t)(p + k * planebytes) & 0xFFFF;
		BLTSIZE = ((uint16_t)n * 8) << 6 | (COLS / 2);
	}
	if (r == 0)
		guard_sync(act);
}

void clear_lines(int8_t y, int8_t ct)
{
	uint8_t r = y + act->top, n;

	if (ct <= 0)
		return;
	if (r >= ROWS)
		r -= ROWS;
	/* The ring may wrap inside the range: two parts */
	n = ROWS - r;
	if (n > ct)
		n = ct;
	blit_clear(r, n);
	if (ct > n)
		blit_clear(0, ct - n);
}

void clear_across(int8_t y, int8_t x, int16_t l)
{
	uint8_t *p = cell(y, x);
	uint8_t i, k;
	int16_t j;

	if (l <= 0)
		return;
	blit_wait();
	for (i = 0; i < 8; i++) {
		for (k = 0; k < planes; k++)
			for (j = 0; j < l; j++)
				p[k * planebytes + j] = bg[k];
		p += COLS;
	}
	if (ring_row(y) == 0)
		guard_sync(act);
}

/* The vt clears the row that comes in afterwards */
void scroll_up(void)
{
	if (++act->top == ROWS)
		act->top = 0;
	if (act == vs + shown)
		copper_build();
}

void scroll_down(void)
{
	act->top = act->top ? act->top - 1 : ROWS - 1;
	if (act == vs + shown)
		copper_build();
}

void cursor_on(int8_t y, int8_t x)
{
	act->cur_y = y;
	act->cur_x = x;
	invert_cell(y, x);
}

void cursor_off(void)
{
	if (act->cur_y >= 0)
		invert_cell(act->cur_y, act->cur_x);
	act->cur_y = -1;
}

void cursor_disable(void)
{
}

/* Ink and paper (IGRB), bold as the bright ink. One plane: white on
   black whatever the colours */
void vtattr_notify(void)
{
	uint8_t ink = vtink & 15, paper = vtpaper & 15, k;

	if (vtattr & VTA_BOLD)
		ink |= 8;
	if (planes == 1) {
		fg[0] = 0xFF;
		bg[0] = 0;
		return;
	}
	for (k = 0; k < MAXPLANES; k++) {
		fg[k] = (ink >> k) & 1 ? 0xFF : 0;
		bg[k] = (paper >> k) & 1 ? 0xFF : 0;
	}
}
