#include <kernel.h>
#include <timer.h>
#include <kdata.h>
#include <printf.h>
#include <devtty.h>
#include <blkdev.h>
#include "amiga.h"
#include "devfd.h"

/* Filled in by crt0.S from what the bootblock found in exec */
uint32_t chip_top;
uint32_t fast_base;
uint32_t fast_top;
uint16_t attn_flags;

uaddr_t ramtop;
uint8_t need_resched;

void do_beep(void)
{
}

void map_init(void)
{
}

uint8_t plt_param(char *p)
{
	return 0;
}

void plt_discard(void)
{
}

void memzero(void *p, usize_t len)
{
	memset(p, 0, len);
}

/*
 *	Gayle (A1200/A600) raises level 2 for the IDE drive. Kickstart's disk
 *	driver leaves it on, and when Kickstart has just booted from the hard
 *	disk an IDE interrupt can still be pending: level 2 then fires
 *	continuously (only CIA-A is acknowledged there) and everything until
 *	the IDE driver's reset crawls - the floppy probe took minutes. We
 *	poll the drive, so: interrupts off at the drive (nIEN), Gayle's IDE
 *	interrupt off, the drive's and Gayle's pending ones cleared. On the
 *	A1200 Gayle's latch clears by writing (0x7C keeps the other bits);
 *	the same fix zenix-old found ("fork takes 2.8 seconds").
 */
static void gayle_quiet(void)
{
	*(volatile uint8_t *)0xDA101A = 0x02;	/* IDE control: nIEN */
	*(volatile uint8_t *)0xDAA000 = 0x00;	/* Gayle interrupt enables */
	(void)*(volatile uint8_t *)0xDA001E;	/* IDE status: ack the drive */
	*(volatile uint8_t *)0xDA9000 = 0x7C;	/* Gayle interrupt latch */
	INTREQ = 0x0008;
}

/* Everything but the hardware vectors */
void amiga_hw_init(void)
{
	gayle_quiet();
	serial_init();
	video_init();
	kbd_init();
}

/* Vertical blank: the 50Hz tick */
void plt_vblank(void)
{
	timer_interrupt();
	fd_timer();
}

void pagemap_init(void)
{
	extern uint8_t _end;
	uint32_t e = ((uint32_t)&_end + 15) & ~15;

	kprintf("Commodore Amiga, %dK chip RAM", chip_top >> 10);
	if (fast_top > fast_base)
		kprintf(", %dK fast RAM", (fast_top - fast_base) >> 10);
	kputs("\n");

	/* Chip RAM between the kernel and the screen, then fast RAM */
	kmemaddblk((void *)e, video_base - e);
	if (fast_top > fast_base) {
		uint32_t fb = (fast_base + 15) & ~15;
		kmemaddblk((void *)fb, (fast_top & ~15) - fb);
	}

	kprintf("Motorola 680%s%d processor detected.\n",
		sysinfo.cpu[1]?"":"0",sysinfo.cpu[1]);
	enable_icache();
}

/* Udata and kernel stacks */
/* We need an initial kernel stack and udata so the slot for init is
   set up at compile time */
u_block udata_block0;
static u_block *udata_block[PTABSIZE] = {&udata_block0,};

/* This will belong in the core 68K code once finalized */

void install_vdso(void)
{
	extern uint8_t vdso[];
	/* Should be uput etc */
	memcpy((void *)udata.u_codebase, &vdso, 0x20);
}

uint8_t plt_udata_set(ptptr p)
{
	u_block **up = &udata_block[p - ptab];
	if (*up == NULL) {
		*up = kmalloc(sizeof(struct u_block), 0);
		if (*up == NULL)
			return ENOMEM;
	}
	p->p_udata = &(*up)->u_d;
	return 0;
}
