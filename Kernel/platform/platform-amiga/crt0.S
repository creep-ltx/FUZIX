/*
 *	Entry from the floppy bootblock. The OS is gone: interrupts and DMA
 *	are off, we are in supervisor mode and have been copied to $400.
 *
 *	The bootblock hands over what it learned from exec before it died:
 *	d0 = top of chip RAM
 *	d1 = start of the largest fast RAM block (0 if none)
 *	d2 = end of that block
 *	d3 = exec AttnFlags (CPU and FPU type)
 */
		#include "../../cpu-68000/kernel-68000.def"

		.globl __end
		.globl __bss_start
		.globl start

.mri 1
start:
		or #$0700,sr
		move.l d0,d4
		move.l d1,d5
		move.l d2,d6
		move.l d3,d7
		move.l #__bss_start,a0
		move.l #__end,d0
		sub.l a0,d0
		lsr.l #2,d0
wipebss:
		clr.l (a0)+
		dbra d0,wipebss

		move.l d4,chip_top
		move.l d5,fast_base
		move.l d6,fast_top
		move.w d7,attn_flags

		lea udata_block0+UBLOCK_SIZE,a7
		bsr init_early
		bsr init_hardware
		bsr fuzix_main
		or #$0700,sr
stop:		bra stop
