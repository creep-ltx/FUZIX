/*
 *	Floppy bootblock for Fuzix on the Amiga (from the zenix-old one).
 *
 *	Kickstart loads sectors 0-1, checks the sum and jumps to offset 12
 *	with a1 = trackdisk IORequest and a6 = ExecBase. We read the kernel
 *	(disk offset 1024, size at bootblock offset 8, put there by
 *	mkadf.py) through trackdisk, note the memory layout from exec, stop
 *	the drive motor, shut the OS down, copy the kernel to $400 and jump
 *	to it with:
 *	d0 = top of chip RAM
 *	d1/d2 = start/end of the largest fast RAM block (0/0 if none)
 *	d3 = AttnFlags
 */
	.text
	.globl	_start
_start:
	move.l	%a1,%a5			/* trackdisk ioreq */
	move.l	_start-4(%pc),%d7	/* kernel size (bootblock offset 8) */

	move.l	%d7,%d0
	moveq	#0,%d1			/* MEMF_ANY */
	jsr	-198(%a6)		/* AllocMem */
	tst.l	%d0
	beq	fail
	move.l	%d0,%a4			/* load buffer */

	move.w	#2,28(%a5)		/* io_Command = CMD_READ */
	move.l	%d7,36(%a5)		/* io_Length */
	move.l	%a4,40(%a5)		/* io_Data */
	move.l	#1024,44(%a5)		/* io_Offset */
	move.l	%a5,%a1
	jsr	-456(%a6)		/* DoIO */
	tst.l	%d0
	bne	fail

	move.w	#9,28(%a5)		/* TD_MOTOR */
	clr.l	36(%a5)			/* off */
	move.l	%a5,%a1
	jsr	-456(%a6)		/* DoIO */

	/* Walk exec's memory list: chip top, largest fast block */
	moveq	#0,%d4			/* chip top */
	moveq	#0,%d5			/* best fast lower */
	moveq	#0,%d6			/* best fast upper */
	move.l	322(%a6),%a2		/* MemList lh_Head */
mloop:
	tst.l	(%a2)			/* ln_Succ == 0: end of list */
	beq.s	mdone
	move.w	14(%a2),%d0		/* mh_Attributes */
	move.l	20(%a2),%d1		/* mh_Lower */
	move.l	24(%a2),%d2		/* mh_Upper */
	btst	#1,%d0			/* MEMF_CHIP */
	beq.s	1f
	cmp.l	%d4,%d2
	bls.s	mnext
	move.l	%d2,%d4
	bra.s	mnext
1:	btst	#2,%d0			/* MEMF_FAST */
	beq.s	mnext
	move.l	%d2,%d3
	sub.l	%d1,%d3
	move.l	%d6,%d0
	sub.l	%d5,%d0
	cmp.l	%d0,%d3
	bls.s	mnext
	move.l	%d1,%d5
	move.l	%d2,%d6
mnext:
	move.l	(%a2),%a2
	bra.s	mloop
mdone:
	moveq	#0,%d3
	move.w	296(%a6),%d3		/* AttnFlags */

	jsr	-150(%a6)		/* SuperState: supervisor from here on */
	or.w	#0x0700,%sr

	lea	0xDFF000,%a0
	move.w	#0x7FFF,0x9A(%a0)	/* INTENA: everything off */
	move.w	#0x7FFF,0x9C(%a0)	/* INTREQ: everything clear */
	move.w	#0x7FFF,0x96(%a0)	/* DMACON: everything off */
	move.b	#0x7F,0xBFED01		/* CIA-A ICR: all sources off */
	move.b	#0x7F,0xBFDD00		/* CIA-B ICR: all sources off */

	btst	#0,%d3			/* 68010 or later: vectors at 0 */
	beq.s	2f
	moveq	#0,%d0
	movec	%d0,%vbr
	btst	#3,%d3			/* 68040/060: leave the caches */
	bne.s	2f
	btst	#1,%d3			/* 68020/030: caches off and empty */
	beq.s	2f
	move.l	#0x0808,%d0
	movec	%d0,%cacr
2:

	/* The copier runs from $100 (unused vectors), clear of both the
	   load buffer and the kernel's home at $400 */
	lea	copier(%pc),%a0
	lea	0x100,%a1
	moveq	#(copier_end-copier)/2-1,%d0
3:	move.w	(%a0)+,(%a1)+
	dbf	%d0,3b

	move.l	%d4,%d0			/* chip top */
	move.l	%d5,%d1			/* fast lower */
	move.l	%d6,%d2			/* fast upper */
	move.l	%a4,%a0			/* src: load buffer */
	lea	0x400,%a1		/* dst */
	lsr.l	#2,%d7			/* longwords (size is a 512 multiple) */
	jmp	0x100

fail:
	move.w	#0xF00,0xDFF180		/* red screen: boot failed */
	bra.s	fail

/* Position independent; the load buffer is always above $400 so a
   forward copy is safe even when they overlap */
copier:
	move.l	(%a0)+,(%a1)+
	subq.l	#1,%d7
	bne.s	copier
	jmp	0x400
copier_end:
