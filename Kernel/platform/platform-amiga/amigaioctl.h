#ifndef __AMIGAIOCTL_DOT_H__
#define __AMIGAIOCTL_DOT_H__

/*
 *	The Amiga's own ioctls, for the kernel and the programs in utils/
 *	(which include it as "../amigaioctl.h"). Numbers only: no types.
 */

/* Hard disks: read the partition table again (the Linux name and
   number), after setup rewrote it. The partitions must be closed and
   synced first. */
#ifndef BLKRRPART
#define BLKRRPART	0x4108
#endif

/* Screens: copy one of the standard 8x8 fonts (256 characters, 2048
   bytes). The buffer's first byte chooses: 0 = Fuzix's own font, 1 = the
   Topaz 8 in the Kickstart ROM. ENOENT if there is no such font. */
#define VTGETSTDFONT	0x38
#define VTSTDFONT_FUZIX	0
#define VTSTDFONT_ROM	1

/* Screen modes. VTSETMODE's buffer holds one byte: the mode, plus
   VTMODE_BOOT when /etc/rc sets the saved one (the left mouse button
   held then keeps the machine's own mode: EBUSY). ENOMEM: that mode
   needs a restart. VTGETMODE returns the mode in bits 0-3, the
   machine's own in 4-7, the ones it can show in 8-11 and the ones that
   fit without a restart in 12-15 (bit n = mode n). */
#define VTSETMODE	0x39
#define VTGETMODE	0x3A
#define VTMODE_PAL	0
#define VTMODE_NTSC	1
#define VTMODE_VGA	2		/* 640x480, 31kHz */
#define VTMODE_BOOT	0x80

#endif
