/*
 *	Hard disk boot for Fuzix on the Amiga.
 *
 *	Stored in the drive's Rigid Disk Block as the "file system" of
 *	DosType FUZ\0 (FileSysHeader + LoadSeg blocks). Kickstart's disk
 *	driver loads it at boot; when the Fuzix partition is the one booted
 *	(the default, or picked in Kickstart's boot menu), AmigaOS starts it
 *	as that partition's file system handler. It then:
 *
 *	- takes the startup packet every handler gets: it names the disk
 *	  driver, the unit and the partition's geometry (FileSysStartupMsg)
 *	- reads the kernel from the partition's last 256 blocks, through
 *	  that driver (so any controller AmigaOS boots from works): the
 *	  first is a header ("FUZK", the kernel's size), the kernel follows.
 *	  (At the end, so that Fuzix itself can write it through the
 *	  partition: Fuzix reaches only the first 32 MB of a device.)
 *	- hands the machine to it (takeover.S)
 *
 *	Kickstart 2.0 or later (CreateMsgPort). Red screen = it failed.
 *	Position independent: one code hunk, no relocations.
 */
	.text
	.globl	_start
_start:
	move.l	4.w,%a6			/* ExecBase */

	sub.l	%a1,%a1
	jsr	-294(%a6)		/* FindTask(NULL): our process */
	move.l	%d0,%a2
	lea	92(%a2),%a2		/* pr_MsgPort */
	move.l	%a2,%a0
	jsr	-384(%a6)		/* WaitPort */
	move.l	%a2,%a0
	jsr	-372(%a6)		/* GetMsg: the startup message */
	move.l	%d0,%a0
	move.l	10(%a0),%a3		/* its ln_Name: the DosPacket */

	/* AmigaOS starts every partition's handler sooner or later, not
	   only the boot partition's. Start Fuzix only when this partition is
	   the one being booted: then ours is the first handler, and no
	   volume exists yet. Booting AmigaOS from another partition (or a
	   floppy), its volume is there by the time ours is asked: then say
	   "not a DOS disk" and end, and the FUZIX partition just stays
	   without a volume. (The head of expansion.library's boot list does
	   not say it: the floppy drive is there first, disk or not.) */
	lea	dosname(%pc),%a1
	moveq	#0,%d0
	jsr	-552(%a6)		/* OpenLibrary */
	tst.l	%d0
	beq.s	3f			/* none: just boot */
	move.l	%d0,%a0
	move.l	34(%a0),%a0		/* dl_Root */
	move.l	24(%a0),%d0		/* rn_Info: BPTR DosInfo */
	lsl.l	#2,%d0
	move.l	%d0,%a4
	jsr	-132(%a6)		/* Forbid */
	move.l	4(%a4),%d0		/* di_DevInfo: the DosList */
	moveq	#0,%d5			/* volumes seen */
4:	tst.l	%d0
	beq.s	5f
	lsl.l	#2,%d0
	move.l	%d0,%a0
	cmp.l	#2,4(%a0)		/* dol_Type == DLT_VOLUME */
	bne.s	6f
	moveq	#1,%d5
6:	move.l	(%a0),%d0		/* dol_Next */
	bra.s	4b
5:	jsr	-138(%a6)		/* Permit */
	tst.l	%d5
	beq.s	3f			/* no volume yet: we are the boot */
	clr.l	12(%a3)			/* dp_Res1 = DOSFALSE */
	move.l	#225,16(%a3)		/* dp_Res2 = ERROR_NOT_A_DOS_DISK */
	move.l	4(%a3),%a0		/* reply to dp_Port... */
	move.l	%a2,4(%a3)		/* ...from our port */
	move.l	(%a3),%a1		/* dp_Link: the message */
	jsr	-366(%a6)		/* PutMsg */
	moveq	#0,%d0
	rts				/* the handler process ends */
3:
	move.l	24(%a3),%d0		/* dp_Arg2: BPTR FileSysStartupMsg */
	lsl.l	#2,%d0
	move.l	%d0,%a4			/* a4 = fssm */

	/* The driver's name: a BSTR, copied with a 0 at the end */
	move.l	4(%a4),%d0
	lsl.l	#2,%d0
	move.l	%d0,%a0
	lea	devname(%pc),%a1
	moveq	#0,%d1
	move.b	(%a0)+,%d1
	cmp.w	#62,%d1
	bhi	fail
	bra.s	2f
1:	move.b	(%a0)+,(%a1)+
2:	dbf	%d1,1b
	clr.b	(%a1)

	/* The kernel's header: 256 blocks before the partition's end,
	   (HighCyl + 1) x Surfaces x BlocksPerTrack x (SizeBlock / 128)
	   512-byte blocks from the disk's start */
	move.l	8(%a4),%d0		/* fssm_Environ: BPTR DosEnvec */
	lsl.l	#2,%d0
	move.l	%d0,%a5			/* a5 = DosEnvec */
	move.l	40(%a5),%d0		/* de_HighCyl */
	addq.l	#1,%d0
	move.l	12(%a5),%d1		/* de_Surfaces */
	bsr	mul32
	move.l	20(%a5),%d1		/* de_BlocksPerTrack */
	bsr	mul32
	move.l	4(%a5),%d1		/* de_SizeBlock (longs) */
	lsr.l	#7,%d1
	bsr	mul32
	sub.l	#256,%d0
	bcs	fail
	cmp.l	#0x7FFFFF,%d0		/* the byte offset must fit 32 bits */
	bhi	fail
	moveq	#9,%d1
	lsl.l	%d1,%d0
	move.l	%d0,%d6			/* d6 = byte offset of the header */

	jsr	-666(%a6)		/* CreateMsgPort */
	tst.l	%d0
	beq	fail
	lea	ioreq(%pc),%a1
	move.l	%d0,14(%a1)		/* mn_ReplyPort */
	move.w	#48,18(%a1)		/* mn_Length */
	lea	devname(%pc),%a0
	move.l	(%a4),%d0		/* fssm_Unit */
	move.l	12(%a4),%d1		/* fssm_Flags */
	jsr	-444(%a6)		/* OpenDevice */
	tst.l	%d0
	bne	fail

	/* The header block */
	lea	header(%pc),%a0
	move.l	%a0,%d0
	move.l	#512,%d1
	move.l	%d6,%d2
	bsr	readblocks
	lea	header(%pc),%a0
	cmp.l	#0x46555A4B,(%a0)	/* "FUZK" */
	bne	fail
	move.l	4(%a0),%d7		/* the kernel's size */
	beq	fail
	cmp.l	#0x80000,%d7
	bhi	fail

	/* The kernel, in a buffer rounded up to whole blocks */
	move.l	%d7,%d0
	add.l	#511,%d0
	and.l	#0xFFFFFE00,%d0
	move.l	%d0,%d5
	moveq	#0,%d1			/* MEMF_ANY */
	jsr	-198(%a6)		/* AllocMem */
	tst.l	%d0
	beq	fail
	move.l	%d0,%a4			/* a4 = the kernel, for takeover */
	move.l	%d5,%d1
	move.l	%d6,%d2
	add.l	#512,%d2
	bsr	readblocks

	/* End with a one-block read: after the long multi-block read some
	   drives (FS-UAE's IDE at least) ignored every command the Fuzix
	   driver sent next */
	lea	header(%pc),%a0
	move.l	%a0,%d0
	move.l	#512,%d1
	move.l	%d6,%d2
	bsr	readblocks

	addq.l	#3,%d7			/* longwords for the copier */
	and.l	#0xFFFFFFFC,%d7

#include "takeover.S"

/* CMD_READ d1 bytes from byte offset d2 into d0; fails on an error */
readblocks:
	lea	ioreq(%pc),%a1
	move.w	#2,28(%a1)		/* io_Command = CMD_READ */
	move.l	%d1,36(%a1)		/* io_Length */
	move.l	%d0,40(%a1)		/* io_Data */
	move.l	%d2,44(%a1)		/* io_Offset */
	jsr	-456(%a6)		/* DoIO */
	tst.l	%d0
	bne	fail
	rts

/* d0 = d0 x d1, low 32 bits, on a 68000 */
mul32:
	movem.l	%d2-%d3,-(%sp)
	move.w	%d0,%d2
	mulu	%d1,%d2			/* low x low */
	move.l	%d0,%d3
	swap	%d3
	mulu	%d1,%d3			/* high x low */
	swap	%d1
	mulu	%d0,%d1			/* low x high */
	add.l	%d3,%d1
	swap	%d1
	clr.w	%d1
	add.l	%d2,%d1
	move.l	%d1,%d0
	movem.l	(%sp)+,%d2-%d3
	rts

fail:
	move.w	#0xF00,0xDFF180		/* red screen: the boot failed */
	bra.s	fail

dosname:
	.asciz	"dos.library"
	.balign	4
ioreq:
	.space	48
devname:
	.space	64
header:
	.space	512
