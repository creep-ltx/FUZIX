/*
 *	Battery-backed clocks at $DC0000: the Oki MSM6242 (A500 with A501,
 *	A500+, A600 with A601, A2000, most A1200 clock-port and trapdoor
 *	clocks) and the Ricoh RP5C01 (A3000, A4000 and some add-ons). Both
 *	have 16 four-bit registers, one per longword: register n is the low
 *	nibble of the byte at $DC0003 + 4n.
 *
 *	   n:  0   1   2   3   4   5   6   7   8   9   10  11  12  13  14  15
 *	MSM:  s1  s10 m1  m10 h1  h10 d1  d10 mo1 mo10 y1 y10 wd  CD  CE  CF
 *	RP5C01: same up to h10, then wd d1 d10 mo1 mo10 y1 y10 MODE TEST RESET
 *	(RP5C01 bank 0; bank 1 register 10 bit 0 = 24 hour mode)
 *
 *	Without a clock this space shows a mirror of the custom chips, so
 *	nothing is written until the seconds were seen ticking (the Guru
 *	Book's advice). The chips keep two year digits: 78-99 are 19xx,
 *	00-77 20xx (AmigaOS does the same).
 *
 *	/dev/rtc gives Fuzix's struct cmos_rtc in BCD: setdate reads it at
 *	boot instead of asking, and setdate -w writes the clock.
 */
#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <rtc.h>
#include "amiga.h"

#define RTC(n)	(*(volatile uint8_t *)(0xDC0003UL + ((n) << 2)))

#define RTC_NONE	0
#define RTC_MSM6242	1
#define RTC_RP5C01	2

static uint8_t rtc_type;
static uint8_t rtc_get(uint8_t *t);

/* MSM6242 control registers */
#define MSM_CD_HOLD	0x01
#define MSM_CD_BUSY	0x02
#define MSM_CF_REST	0x01
#define MSM_CF_24	0x04
/* RP5C01 mode register */
#define RP_MODE_TIMER	0x08
#define RP_BANK(n)	(n)

/* Wait n times 64us: PAL/NTSC scan lines, two at 31kHz */
static void wait_lines(uint16_t n)
{
	volatile uint8_t *vpos = (volatile uint8_t *)0xDFF006;
	uint8_t c;

	n = (n * lines_per_ms + 15) / 16;
	while (n--) {
		c = *vpos;
		while (*vpos == c)
			;
	}
}

static uint8_t nib(uint8_t n)
{
	return RTC(n) & 0x0F;
}

static uint8_t two(uint8_t lo)
{
	return nib(lo) + 10 * nib(lo + 1);
}

/* Does the date in registers d1.. make sense? (day d, month d + 2) */
static uint8_t date_ok(uint8_t d, uint8_t wd)
{
	uint8_t day = two(d), mon = two(d + 2);

	return day >= 1 && day <= 31 && mon >= 1 && mon <= 12 &&
	       nib(d + 4) <= 9 && nib(wd) <= 6;	/* year tens may be 12 */
}

void rtc_init(void)
{
	uint8_t s, i, msm, rp, mode = 0;

#ifdef RTC_DEBUG
	for (s = 0; s < 16; s++)
		kprintf("%x ", nib(s));
	kputs("(raw)\n");
#endif
	/* Kickstart can leave an RP5C01 on another register bank (mode
	   reads 9: timer running, bank 1); the time is in bank 0. Only for
	   exactly that pattern, and put back if no clock turns up. */
	if ((nib(13) & RP_MODE_TIMER) && (nib(13) & 3)) {
		mode = nib(13);
		RTC(13) = mode & 0x0C;
	}
	/* Nothing there: all registers alike (an empty space reads 0) */
	for (i = 1; i < 16 && nib(i) == nib(0); i++)
		;
	if (i == 16)
		goto none;
	/* Plausible seconds and minutes in both chips' layouts */
	if (nib(0) > 9 || nib(1) > 5 || nib(2) > 9 || nib(3) > 5)
		goto none;
	/* The seconds must tick: up to 1.2 seconds, read only */
	s = nib(0);
	for (i = 0; i < 120 && nib(0) == s; i++)
		wait_lines(160);		/* about 10ms */
	if (nib(0) == s || nib(0) > 9)
		goto none;

	msm = date_ok(6, 12);
	rp = date_ok(7, 6);
	if (rp && !msm)
		rtc_type = RTC_RP5C01;
	else if (msm && !rp)
		rtc_type = RTC_MSM6242;
	else
		/* Both or neither (a clock never set): the RP5C01 runs with
		   its timer bit set in MODE, the MSM6242's matching bit is
		   the unused 30-second adjust */
		rtc_type = (nib(13) & RP_MODE_TIMER) ? RTC_RP5C01 : RTC_MSM6242;
	kprintf("Battery clock: %s\n",
		rtc_type == RTC_MSM6242 ? "MSM6242" : "RP5C01");
#ifdef RTC_DEBUG
	{
		uint8_t t[8], ok, n;
		for (n = 0; n < 16; n++)
			kprintf("%x", nib(n));
		ok = rtc_get(t);
		kprintf(" -> ok %d %d.%d.%d %d:%d:%d\n", ok, t[5], t[4], t[3], t[2], t[1], t[0]);
	}
#endif
	return;
none:
	if (mode)
		RTC(13) = mode;
}

static void msm_hold(void)
{
	uint8_t i;

	RTC(13) = MSM_CD_HOLD;
	/* Busy clears within 190us */
	for (i = 0; i < 8 && (nib(13) & MSM_CD_BUSY); i++) {
		RTC(13) = 0;
		wait_lines(1);
		RTC(13) = MSM_CD_HOLD;
		wait_lines(2);
	}
}

static uint8_t bcd(uint8_t v)
{
	return ((v / 10) << 4) | (v % 10);
}

/* Read the time; 0 if the clock holds no valid date */
static uint8_t rtc_get(uint8_t *t)
{
	uint8_t i, h, h10, pm = 0, h24, d;

	if (rtc_type == RTC_MSM6242) {
		msm_hold();
		h24 = nib(15) & MSM_CF_24;
		d = 6;
		t[7] = nib(12);
	} else {
		RTC(13) = (nib(13) & 0x0C) | RP_BANK(1);
		h24 = nib(10) & 1;
		RTC(13) = (nib(13) & 0x0C) | RP_BANK(0);
		d = 7;
		t[7] = nib(6);
	}
	t[0] = two(0);			/* seconds */
	t[1] = two(2);			/* minutes */
	h = nib(4);
	h10 = nib(5);
	if (!h24) {
		pm = (h10 & (rtc_type == RTC_MSM6242 ? 4 : 2)) ? 1 : 0;
		h10 &= 1;
	}
	t[2] = h + 10 * (h10 & 3);	/* hours */
	if (!h24) {			/* 12 AM is 0, 12 PM is 12 */
		if (t[2] == 12)
			t[2] = 0;
		if (pm)
			t[2] += 12;
	}
	t[3] = two(d);			/* day */
	t[4] = two(d + 2);		/* month */
	/* Year: two digits, but FS-UAE (and so presumably Kickstart) keeps
	   years since 1900, a tens digit of 12 for 2026: take it mod 100 */
	t[5] = two(d + 4) % 100;	/* year */
	if (rtc_type == RTC_MSM6242)
		RTC(13) = 0;		/* release hold */
	for (i = 0; i < 6; i++)
		if (t[i] > 99)
			return 0;
	return t[0] < 60 && t[1] < 60 && t[2] < 24 && t[3] >= 1 &&
	       t[3] <= 31 && t[4] >= 1 && t[4] <= 12;
}

static void put2(uint8_t lo, uint8_t v)
{
	RTC(lo) = v % 10;
	RTC(lo + 1) = v / 10;
}

static void rtc_set(uint8_t *t)
{
	if (rtc_type == RTC_MSM6242) {
		msm_hold();
		RTC(15) = MSM_CF_24 | MSM_CF_REST;	/* 24 hours, restart second */
		put2(0, t[0]);
		put2(2, t[1]);
		put2(4, t[2]);
		put2(6, t[3]);
		put2(8, t[4]);
		put2(10, t[5]);
		RTC(12) = t[7];
		RTC(15) = MSM_CF_24;
		RTC(13) = 0;
	} else {
		RTC(13) = RP_BANK(1);
		RTC(10) = 1;				/* 24 hours */
		RTC(13) = RP_BANK(0);			/* timer stopped */
		put2(0, t[0]);
		put2(2, t[1]);
		put2(4, t[2]);
		RTC(6) = t[7];
		put2(7, t[3]);
		put2(9, t[4]);
		put2(11, t[5]);
		RTC(13) = RP_MODE_TIMER | RP_BANK(0);	/* running */
	}
}

uint_fast8_t plt_rtc_secs(void)
{
	/* Not used to steer the system clock */
	return 0xFF;
}

int plt_rtc_read(void)
{
	struct cmos_rtc cmos;
	uint16_t len = sizeof(cmos);
	uint8_t t[8];
	uint8_t *p = cmos.data.bytes;
	irqflags_t irq;
	uint8_t ok;

	if (rtc_type == RTC_NONE) {
		udata.u_error = ENXIO;
		return -1;
	}
	irq = di();
	ok = rtc_get(t);
	irqrestore(irq);
	if (!ok) {
		udata.u_error = EIO;
		return -1;
	}
	if (udata.u_count < len)
		len = udata.u_count;
	*p++ = t[5] >= 78 ? 0x19 : 0x20;
	*p++ = bcd(t[5]);
	*p++ = bcd(t[4] - 1);		/* month from 0 */
	*p++ = bcd(t[3]);
	*p++ = bcd(t[2]);
	*p++ = bcd(t[1]);
	*p++ = bcd(t[0]);
	*p = t[7];
	cmos.type = CMOS_RTC_BCD;
	if (uput(&cmos, udata.u_base, len) == -1)
		return -1;
	return len;
}

static uint8_t unbcd(uint8_t v)
{
	return (v >> 4) * 10 + (v & 0x0F);
}

int plt_rtc_write(void)
{
	struct cmos_rtc cmos;
	uint8_t *p = cmos.data.bytes;
	uint8_t t[8];
	uint16_t year;
	irqflags_t irq;

	if (rtc_type == RTC_NONE) {
		udata.u_error = ENXIO;
		return -1;
	}
	if (udata.u_count != sizeof(cmos) ||
	    uget(udata.u_base, &cmos, sizeof(cmos)) == -1)
		return -1;
	if (cmos.type != CMOS_RTC_BCD) {
		udata.u_error = EINVAL;
		return -1;
	}
	year = unbcd(p[0]) * 100 + unbcd(p[1]);
	if (year < 1978 || year > 2077) {
		udata.u_error = EINVAL;
		return -1;
	}
	t[5] = year % 100;
	t[4] = unbcd(p[2]) + 1;
	t[3] = unbcd(p[3]);
	t[2] = unbcd(p[4]);
	t[1] = unbcd(p[5]);
	t[0] = unbcd(p[6]);
	t[7] = unbcd(p[7]) % 7;
	irq = di();
	rtc_set(t);
	irqrestore(irq);
	return sizeof(cmos);
}
