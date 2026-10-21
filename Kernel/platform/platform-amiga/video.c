/*
 *	Text screen: 640x256 PAL hires, one bitplane, 80x32 characters of
 *	8x8. The screen and a small Copper list sit at the top of chip RAM.
 *	One bitplane makes a character cell one byte wide, so drawing is
 *	eight byte stores.
 */
#include <kernel.h>
#include <kdata.h>
#include <vt.h>
#include "amiga.h"

#define COLS		80
#define ROWS		32
#define LINEBYTES	COLS
#define ROWBYTES	(LINEBYTES * 8)
#define SCREENBYTES	(ROWBYTES * ROWS)	/* 20480 */
#define COPPERBYTES	64

uint32_t video_base;
static uint8_t *screen;
static volatile uint16_t *copper;

extern const unsigned char fontdata_8x8[];

/* The console font: Fuzix's own at boot, replaceable with VTSETFONT */
uint8_t vtfont[2048];
/* The Kickstart ROM's Topaz 8 as the same 256 x 8 byte table */
uint8_t romfont[2048];
uint8_t have_romfont;

static int8_t cur_y = -1, cur_x;

static uint8_t *cell(int8_t y, int8_t x)
{
	return screen + (uint16_t)y * ROWBYTES + x;
}

static void invert_cell(int8_t y, int8_t x)
{
	uint8_t *p = cell(y, x);
	uint8_t i;

	for (i = 0; i < 8; i++) {
		*p ^= 0xFF;
		p += LINEBYTES;
	}
}

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

void video_init(void)
{
	uint32_t *p;
	uint16_t i;
	volatile uint16_t *c;

	video_base = (chip_top - SCREENBYTES - COPPERBYTES) & ~0xFF;
	screen = (uint8_t *)video_base;
	copper = (volatile uint16_t *)(video_base + SCREENBYTES);

	p = (uint32_t *)screen;
	for (i = 0; i < SCREENBYTES / 4; i++)
		*p++ = 0;

	/* The bitplane pointer runs on during the frame: the Copper sets
	   it back at the top of every frame */
	c = copper;
	*c++ = 0x00E0; *c++ = video_base >> 16;
	*c++ = 0x00E2; *c++ = video_base & 0xFFFF;
	*c++ = 0xFFFF; *c++ = 0xFFFE;

	BPLCON0 = 0x9200;	/* hires, one bitplane, colour burst */
	BPLCON1 = 0;
	BPLCON2 = 0;
	BPLCON3 = 0x0C00;	/* AGA: ECS-compatible defaults */
	BPLCON4 = 0x0011;
	FMODE = 0;		/* OCS fetch mode on AGA */
	BPL1MOD = 0;
	BPL2MOD = 0;
	DDFSTRT = 0x003C;
	DDFSTOP = 0x00D4;
	DIWSTRT = 0x2C81;
	DIWSTOP = 0x2CC1;
	COLOR(0) = 0x0000;
	COLOR(1) = 0x0CCC;
	COP1LCH = (uint32_t)copper >> 16;
	COP1LCL = (uint32_t)copper & 0xFFFF;
	COPJMP1 = 0;
	/* SET | DMAEN | BPLEN | COPEN */
	DMACON = 0x8000 | 0x0200 | 0x0100 | 0x0080;

	memcpy(vtfont, fontdata_8x8, sizeof(vtfont));
	have_romfont = find_rom_font();

	vtinit();
}

void plot_char(int8_t y, int8_t x, uint16_t c)
{
	uint8_t *p = cell(y, x);
	const uint8_t *f = vtfont + ((c & 0xFF) << 3);
	uint8_t inv = (vtattr & VTA_INVERSE) ? 0xFF : 0;
	uint8_t i;

	for (i = 0; i < 8; i++) {
		uint8_t b = *f++;
		if (i == 7 && (vtattr & VTA_UNDERLINE))
			b = 0xFF;
		*p = b ^ inv;
		p += LINEBYTES;
	}
}

void clear_lines(int8_t y, int8_t ct)
{
	uint32_t *p = (uint32_t *)cell(y, 0);
	uint16_t n = (uint16_t)ct * (ROWBYTES / 4);

	while (n--)
		*p++ = 0;
}

void clear_across(int8_t y, int8_t x, int16_t l)
{
	uint8_t *p = cell(y, x);
	uint8_t i;
	int16_t j;

	for (i = 0; i < 8; i++) {
		for (j = 0; j < l; j++)
			p[j] = 0;
		p += LINEBYTES;
	}
}

void scroll_up(void)
{
	uint32_t *d = (uint32_t *)screen;
	uint32_t *s = (uint32_t *)(screen + ROWBYTES);
	uint16_t n = (SCREENBYTES - ROWBYTES) / 4;

	while (n--)
		*d++ = *s++;
}

void scroll_down(void)
{
	uint32_t *d = (uint32_t *)(screen + SCREENBYTES);
	uint32_t *s = (uint32_t *)(screen + SCREENBYTES - ROWBYTES);
	uint16_t n = (SCREENBYTES - ROWBYTES) / 4;

	while (n--)
		*--d = *--s;
}

void cursor_on(int8_t y, int8_t x)
{
	cur_y = y;
	cur_x = x;
	invert_cell(y, x);
}

void cursor_off(void)
{
	if (cur_y >= 0)
		invert_cell(cur_y, cur_x);
	cur_y = -1;
}

void cursor_disable(void)
{
}

void vtattr_notify(void)
{
}
