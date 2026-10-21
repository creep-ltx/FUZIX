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

#endif
