/*
 *	Shared by the floppy bootblock and the hard disk loader: hand the
 *	machine from AmigaOS to the Fuzix kernel.
 *
 *	In:  a6 = ExecBase, a4 = the kernel image, d7 = its size in bytes
 *	     (a multiple of 4). Never returns. The kernel is copied to $400
 *	     and entered with d0 = chip top, d1/d2 = largest fast block,
 *	     d3 = AttnFlags.
 */
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


/* Position independent; the load buffer is always above $400 so a
   forward copy is safe even when they overlap */
copier:
	move.l	(%a0)+,(%a1)+
	subq.l	#1,%d7
	bne.s	copier
	jmp	0x400
copier_end:
