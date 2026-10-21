/*
 *	Hard disk boot for Fuzix on the Amiga.
 *
 *	This is stored in the drive's Rigid Disk Block as the "file system"
 *	for DosType FUZ\0 (FileSysHeader + LoadSeg blocks, put there by
 *	mkrdb.py or the installer). Kickstart's disk driver loads it at
 *	boot; when the Fuzix partition is the boot partition, AmigaOS starts
 *	it as that partition's handler. Instead of serving AmigaOS it hands
 *	the machine to the Fuzix kernel, which is attached after this code
 *	in the same hunk.
 *
 *	Position independent: one code hunk, no relocations.
 */
	.text
	.globl	_start
_start:
	move.l	4.w,%a6			/* ExecBase */
	lea	kernel(%pc),%a4
	move.l	kernel_size(%pc),%d7

#include "takeover.S"

	.balign	4
kernel_size:
	.long	0			/* patched by mkhunk.py */
kernel:
