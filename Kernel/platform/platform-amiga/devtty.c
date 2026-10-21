/*
 *	Amiga terminals: tty1-tty4 are screens sharing the keyboard
 *	(Amiga+F1..F4 picks one), tty5 is Paula's serial port. Kernel
 *	messages go to tty1 and the serial port, so a serial cable (or the
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
#include "amigaioctl.h"

static unsigned char tbuf1[TTYSIZ];
static unsigned char tbuf2[TTYSIZ];
static unsigned char tbuf3[TTYSIZ];
static unsigned char tbuf4[TTYSIZ];
static unsigned char tbuf5[TTYSIZ];

struct s_queue ttyinq[NUM_DEV_TTY + 1] = {	/* ttyinq[0] is never used */
	{NULL, NULL, NULL, 0, 0, 0},
	{tbuf1, tbuf1, tbuf1, TTYSIZ, 0, TTYSIZ / 2},
	{tbuf2, tbuf2, tbuf2, TTYSIZ, 0, TTYSIZ / 2},
	{tbuf3, tbuf3, tbuf3, TTYSIZ, 0, TTYSIZ / 2},
	{tbuf4, tbuf4, tbuf4, TTYSIZ, 0, TTYSIZ / 2},
	{tbuf5, tbuf5, tbuf5, TTYSIZ, 0, TTYSIZ / 2},
};

tcflag_t termios_mask[NUM_DEV_TTY + 1] = {
	0,
	_CSYS,
	_CSYS,
	_CSYS,
	_CSYS,
	_CSYS | CBAUD,
};

uint8_t vtattr_cap;

/* The screen being written (curtty) and the one shown and typed into
   (inputtty), 0-3; the vt state of the others is kept here */
static uint8_t curtty, inputtty;
static struct vt_switch ttysave[MAX_VT];

/* Colour clock / baud = SERPER + 1. The clock is the machine's crystal:
   an NTSC Agnus or Alice (bit 12 of VPOSR) has the NTSC one, whatever
   video mode it runs in. */
#define PAL_CLOCK	3546895UL
#define NTSC_CLOCK	3579545UL
static uint32_t colour_clock = PAL_CLOCK;

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
	if (CUSTOM(0x004) & 0x1000)
		colour_clock = NTSC_CLOCK;
	SERPER = colour_clock / 9600 - 1;
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
	if (minor <= MAX_VT)
		return TTY_READY_NOW;
	return (SERDATR & 0x2000) ? TTY_READY_NOW : TTY_READY_SOON;
}

/* Interrupts stay off while a screen prints: a key's echo (or a kernel
   message) from an interrupt would otherwise switch screens under the
   vt code. Drawing a character is short; scrolling copies nothing. */
void tty_putc(uint8_t minor, unsigned char c)
{
	irqflags_t irq;

	if (minor > MAX_VT) {
		SERDAT = 0x100 | c;
		return;
	}
	irq = di();
	if (curtty != minor - 1) {
		vt_save(&ttysave[curtty]);
		curtty = minor - 1;
		vt_load(&ttysave[curtty]);
		set_active_vt(curtty);
	}
	vtoutput(&c, 1);
	irqrestore(irq);
}

/* Every screen starts white on black at the top left */
void vt_switch_init(void)
{
	uint8_t i;

	for (i = 0; i < MAX_VT; i++) {
		vt_save(&ttysave[i]);
		ttysave[i].ink = 7;
		ttysave[i].paper = 0;
		ttysave[i].cursorx = 0;
		ttysave[i].cursory = 0;
	}
}

static void show_vt(uint8_t n)
{
	if (n == inputtty || n >= MAX_VT)
		return;
	inputtty = n;
	set_visible_vt(n);
}

void tty_setup(uint8_t minor, uint8_t flags)
{
	struct termios *t;
	uint8_t baud;

	if (minor != SERIAL_TTY)
		return;
	t = &ttydata[minor].termios;
	baud = t->c_cflag & CBAUD;
	if (baud == B0 || baud > B115200)
		baud = B9600;
	SERPER = colour_clock / baudrate[baud] - 1;
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
		tty_inproc(SERIAL_TTY, c);
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
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
	KEY_F9, KEY_F10, '(', ')', '/', '*', '+', KEY_HELP
};

#define KEY_LSHIFT	0x60
#define KEY_RSHIFT	0x61
#define KEY_CAPS	0x62
#define KEY_CTRL	0x63
#define KEY_LALT	0x64
#define KEY_RALT	0x65
#define KEY_LAMIGA	0x66
#define KEY_RAMIGA	0x67

static uint8_t shift, ctrl, caps, alt, amiga;

/*
 *	A loaded AmigaOS keymap, converted by mkkeymap.py (format there):
 *	every key's character for none, Shift, Alt and Shift+Alt, dead keys
 *	(an accent, then the letter), and which keys Caps Lock affects.
 *	Without one the US tables above are used.
 */
#define AKM_SIZE	4096
#define AKM_KEYS	0x68
#define AKM_ENTRIES	24
static uint8_t akm[AKM_SIZE];
static uint8_t have_akm;
static uint8_t dead;			/* pending dead key, 0 = none */

static uint8_t akm_key(uint8_t code)
{
	uint8_t s = shift;
	uint8_t *e;
	uint16_t mod;
	uint8_t c;

	if (caps && (akm[8 + (code >> 3)] & (1 << (code & 7))))
		s = 1;
	e = akm + AKM_ENTRIES + 16 * code + 4 * (s | (alt << 1));
	if (e[1]) {			/* a dead key: wait for the next */
		dead = e[1];
		return 0;
	}
	c = e[0];
	mod = (e[2] << 8) | e[3];
	if (mod && dead <= akm[4] && mod + akm[4] < AKM_SIZE)
		c = akm[mod + dead];
	dead = 0;
	return c;
}

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
	case KEY_LALT:
	case KEY_RALT:
		alt = !up;
		return;
	case KEY_LAMIGA:
	case KEY_RAMIGA:
		amiga = !up;
		return;
	}
	if (up || code >= 0x60)
		return;

	/* Amiga+F1..F4 (or Alt+F1..F4, as on Linux): another screen */
	if ((amiga || alt) && code >= 0x50 && code < 0x50 + MAX_VT) {
		show_vt(code - 0x50);
		return;
	}

	/* The keymap first; cursor, function and Help keys (strings in
	   AmigaOS keymaps) from the table above */
	if (have_akm && code < AKM_KEYS) {
		c = akm_key(code);
		if (c) {
			if (ctrl && c >= 0x40 && c < 0x80)
				c &= 0x1F;
			/* straight in: vt_inproc would turn 0xA0-0xFF (Latin-1
			   letters) into escape sequences */
			tty_inproc(inputtty + 1, c);
			return;
		}
		if (dead)
			return;
	}
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
	vt_inproc(inputtty + 1, c);
}

/* KBSETTRANS: a keymap from mkkeymap.py, or "AKM0" for the built-in US */
static int set_keymap(char *data)
{
	have_akm = 0;
	dead = 0;
	if (uget(data, akm, AKM_SIZE) == -1)
		return -1;
	if (memcmp(akm, "AKM0", 4) == 0)
		return 0;
	if (memcmp(akm, "AKM1", 4)) {
		udata.u_error = EINVAL;
		return -1;
	}
	have_akm = 1;
	return 0;
}

/*
 *	Console fonts: the screen draws from vtfont (Fuzix's own font at
 *	boot). VTSETFONT replaces it, VTGETSTDFONT hands out Fuzix's font
 *	or the Kickstart ROM's Topaz 8 so a program can offer both.
 */
extern uint8_t vtfont[2048];
extern uint8_t romfont[2048];
extern uint8_t have_romfont;
extern uint8_t fuzixfont[2048];

static const struct fontinfo fontinfo = {
	0, 255, 0, 0, FONT_INFO_8X8
};

int amiga_vt_ioctl(uint_fast8_t minor, uarg_t request, char *data)
{
	if (minor <= MAX_VT) {
		switch (request) {
		case VTFONTINFO:
			return uput(&fontinfo, data, sizeof(fontinfo));
		case VTSETFONT:
			return uget(data, vtfont, sizeof(vtfont));
		case VTGETFONT:
			return uput(vtfont, data, sizeof(vtfont));
		case KBSETTRANS:
			return set_keymap(data);
		case VTGETSTDFONT:
			switch (ugetc(data)) {
			case VTSTDFONT_FUZIX:
				return uput(fuzixfont, data, 2048);
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
