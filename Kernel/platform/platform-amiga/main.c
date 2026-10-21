#include <kernel.h>
#include <timer.h>
#include <kdata.h>
#include <printf.h>
#include <devtty.h>
#include <blkdev.h>
#include "amiga.h"

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

/* Everything but the hardware vectors */
void amiga_hw_init(void)
{
	serial_init();
	video_init();
	kbd_init();
}

/* Vertical blank: the 50Hz tick */
void plt_vblank(void)
{
	timer_interrupt();
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
