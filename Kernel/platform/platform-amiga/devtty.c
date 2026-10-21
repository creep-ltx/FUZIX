/*
 *	Amiga terminals: tty1 is the screen and keyboard, tty2 is Paula's
 *	serial port. Kernel messages go to both, so a serial cable (or the
 *	emulator's serial log) shows the boot even with no screen.
 */
#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <stdbool.h>
#include <devtty.h>
#include <vt.h>
#include <tty.h>
#include "amiga.h"

static unsigned char tbuf1[TTYSIZ];
static unsigned char tbuf2[TTYSIZ];

struct s_queue ttyinq[NUM_DEV_TTY + 1] = {	/* ttyinq[0] is never used */
	{NULL, NULL, NULL, 0, 0, 0},
	{tbuf1, tbuf1, tbuf1, TTYSIZ, 0, TTYSIZ / 2},
	{tbuf2, tbuf2, tbuf2, TTYSIZ, 0, TTYSIZ / 2},
};

tcflag_t termios_mask[NUM_DEV_TTY + 1] = {
	0,
	_CSYS,
	_CSYS | CBAUD,
};

uint8_t vtattr_cap;

/* PAL colour clock / baud = SERPER + 1 */
#define PAL_CLOCK	3546895UL

static const uint32_t baudrate[] = {
	0, 50, 75, 110, 134, 150, 300, 600, 1200, 2400, 4800, 9600,
	19200, 38400, 57600, 115200
};

static void serial_putc_polled(uint8_t c)
{
	while (!(SERDATR & 0x2000))	/* TBE */
		;
	SERDAT = 0x100 | c;		/* one stop bit */
}

void serial_init(void)
{
	SERPER = PAL_CLOCK / 9600 - 1;
}

/* Output for the system console (kprintf etc) */
void kputchar(uint8_t c)
{
	if (c == '\n')
		serial_putc_polled('\r');
	serial_putc_polled(c);
	if (c == '\n')
		tty_putc(1, '\r');
	tty_putc(1, c);
}

ttyready_t tty_writeready(uint8_t minor)
{
	if (minor == 1)
		return TTY_READY_NOW;
	return (SERDATR & 0x2000) ? TTY_READY_NOW : TTY_READY_SOON;
}

void tty_putc(uint8_t minor, unsigned char c)
{
	if (minor == 1)
		vtoutput(&c, 1);
	else
		SERDAT = 0x100 | c;
}

void tty_setup(uint8_t minor, uint8_t flags)
{
	struct termios *t;
	uint8_t baud;

	if (minor != 2)
		return;
	t = &ttydata[minor].termios;
	baud = t->c_cflag & CBAUD;
	if (baud == B0 || baud > B115200)
		baud = B9600;
	SERPER = PAL_CLOCK / baudrate[baud] - 1;
	t->c_cflag &= ~CBAUD;
	t->c_cflag |= baud;
}

int tty_carrier(uint8_t minor)
{
	return 1;
}

void tty_sleeping(uint8_t minor)
{
}

void tty_data_consumed(uint8_t minor)
{
}

/* Paula has a one byte receive buffer: drain it on every RBF interrupt */
void serial_interrupt(void)
{
	while (INTREQR & INTF_RBF) {
		uint8_t c = SERDATR & 0xFF;
		INTREQ = INTF_RBF;
		tty_inproc(2, c);
	}
}

/*
 *	Keyboard. Bytes arrive in CIA-A's serial shift register, rotated
 *	and inverted. After each one we must hold the SP line low for at
 *	least 85us (HRM chapter 8). Release = keycode | 0x80.
 *
 *	The 0x00-0x3F codes are positional; this is the US layout.
 */
static const uint8_t keymap[0x40] = {
	'`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\\', 0, '0',
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, '1', '2', '3',
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', 0, 0, '4', '5', '6',
	0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '.', '7', '8', '9'
};

static const uint8_t shiftmap[0x40] = {
	'~', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '|', 0, '0',
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, '1', '2', '3',
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', 0, 0, '4', '5', '6',
	0, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '.', '7', '8', '9'
};

/* 0x40-0x5F: the same on every layout */
static const uint8_t specialmap[0x20] = {
	' ', KEY_BS, KEY_TAB, '\r', '\r', KEY_ESC, KEY_DEL, 0,
	0, 0, '-', 0, KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, '(', ')', '/', '*', '+', KEY_HELP
};

#define KEY_LSHIFT	0x60
#define KEY_RSHIFT	0x61
#define KEY_CAPS	0x62
#define KEY_CTRL	0x63

static uint8_t shift, ctrl, caps;

/* Wait for two scanline changes: between 64 and 128us on any CPU */
static void kbd_handshake_wait(void)
{
	volatile uint8_t *vpos = (volatile uint8_t *)0xDFF006;
	uint8_t n;
	int i;

	for (i = 0; i < 2; i++) {
		n = *vpos;
		while (*vpos == n)
			;
	}
}

void kbd_init(void)
{
	CIAA(CIA_CRA) &= ~0x40;		/* SP is an input */
	(void)CIAA(CIA_ICR);
	CIAA(CIA_ICR) = 0x88;		/* SET | SP interrupt */
}

void kbd_interrupt(void)
{
	uint8_t icr = CIAA(CIA_ICR);	/* reading clears it */
	uint8_t raw, code, up, c = 0;

	if (!(icr & 0x08))
		return;
	raw = CIAA(CIA_SDR);
	code = ~((raw >> 1) | (raw << 7));

	/* Handshake */
	CIAA(CIA_CRA) |= 0x40;
	kbd_handshake_wait();
	CIAA(CIA_CRA) &= ~0x40;

	up = code & 0x80;
	code &= 0x7F;

	switch (code) {
	case KEY_LSHIFT:
	case KEY_RSHIFT:
		shift = !up;
		return;
	case KEY_CTRL:
		ctrl = !up;
		return;
	case KEY_CAPS:
		/* The caps key reports its LED state: down = on */
		caps = !up;
		return;
	}
	if (up || code >= 0x60)
		return;

	if (code < 0x40) {
		c = shift ? shiftmap[code] : keymap[code];
		if (caps && c >= 'a' && c <= 'z')
			c -= 32;
	} else
		c = specialmap[code - 0x40];
	if (!c)
		return;
	if (ctrl && c >= 0x40 && c < 0x80)
		c &= 0x1F;
	vt_inproc(1, c);
}

/*
 *	Console fonts: the screen draws from vtfont (Fuzix's own font at
 *	boot). VTSETFONT replaces it, VTGETSTDFONT hands out Fuzix's font
 *	or the Kickstart ROM's Topaz 8 so a program can offer both.
 */
extern uint8_t vtfont[2048];
extern uint8_t romfont[2048];
extern uint8_t have_romfont;
extern const unsigned char fontdata_8x8[];

static const struct fontinfo fontinfo = {
	0, 255, 0, 0, FONT_INFO_8X8
};

int amiga_vt_ioctl(uint_fast8_t minor, uarg_t request, char *data)
{
	if (minor == 1) {
		switch (request) {
		case VTFONTINFO:
			return uput(&fontinfo, data, sizeof(fontinfo));
		case VTSETFONT:
			return uget(data, vtfont, sizeof(vtfont));
		case VTGETFONT:
			return uput(vtfont, data, sizeof(vtfont));
		case VTGETSTDFONT:
			switch (ugetc(data)) {
			case VTSTDFONT_FUZIX:
				return uput(fontdata_8x8, data, 2048);
			case VTSTDFONT_ROM:
				if (have_romfont)
					return uput(romfont, data, 2048);
				break;
			}
			udata.u_error = ENOENT;
			return -1;
		}
	}
	return vt_ioctl(minor, request, data);
}
