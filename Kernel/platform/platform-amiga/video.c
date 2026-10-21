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

	vtinit();
}

void plot_char(int8_t y, int8_t x, uint16_t c)
{
	uint8_t *p = cell(y, x);
	const uint8_t *f = fontdata_8x8 + ((c & 0xFF) << 3);
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
