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

#include "takeover.S"

fail:
	move.w	#0xF00,0xDFF180		/* red screen: boot failed */
	bra.s	fail

