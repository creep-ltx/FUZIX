/*
 *	Commodore Amiga (68000-68030, OCS/ECS/AGA), flat memory, no MMU.
 *	First target: a stock A1200 (68EC020, 2MB chip RAM, Gayle IDE).
 */

/* Enable to make ^Z dump the inode table for debug */
#undef CONFIG_IDUMP
/* Enable to make ^A drop back into the monitor */
#undef CONFIG_MONITOR
/* Profil syscall support (not yet complete) */
#undef CONFIG_PROFIL

#define CONFIG_32BIT
#define CONFIG_LEVEL_2

#define CONFIG_MULTI
#define CONFIG_FLAT
#define CONFIG_SPLIT_ID
#define CONFIG_PARENT_FIRST
/* It's not that meaningful but we currently chunk to 512 bytes */
#define CONFIG_BANKS 	(65536/512)

#define CONFIG_LARGE_IO_DIRECT(x)	1

#define CONFIG_SPLIT_UDATA
#define UDATA_SIZE	1024
#define UDATA_BLKS	2

/* 50 ticks a second from the vertical blank, whatever the display's
   frame rate (plt_vblank) */
#define TICKSPERSEC 50

/* Four screens on tty1-tty4 (Amiga+F1..F4), Paula serial on tty5 */
#define CONFIG_VT
#define CONFIG_VT_MULTI
#define MAX_VT		4
#define SERIAL_TTY	5
#define CONFIG_FONT8X8
#define VT_WIDTH	80
#define VT_HEIGHT	vt_rows		/* 32 PAL, 25 NTSC, 60 at 640x480 */
#define VT_RIGHT	79
#define VT_BOTTOM	(vt_rows - 1)
extern uint8_t vt_rows;

#define BOOT_TTY (512 + 1)	/* tty1: the screen */

#define CMDLINE	NULL
/* Root file system: the boot floppy's Fuzix partition if it has one,
   else the first partition of the IDE disk (set by device_init) */
#ifndef __ASSEMBLER__
extern unsigned short amiga_root_dev;
#endif
#define BOOTDEVICE	amiga_root_dev

/* Device parameters */
#define NUM_DEV_TTY 5
#define TTYDEV   BOOT_TTY /* Device used by kernel for messages, panics */

#define NBUFS    16       /* Number of block buffers */
#define NMOUNTS	 8	  /* Number of mounts at a time */

#define MAX_BLKDEV 3

#define CONFIG_IDE
/* LBA, or cylinder/head/sector for drives without it */
#define CONFIG_IDE_AUTO
/* Read away a transfer the boot ROM left unfinished */
#define CONFIG_IDE_DRAIN
/* The 25ms reset wait, plus one tick: the tick is 20ms */
#define IDE_RESET_MS	(25 + 1000 / TICKSPERSEC)
/* Battery-backed clock at $DC0000 (MSM6242 or RP5C01), if there is one:
   /dev/rtc, read by setdate at boot */
#define CONFIG_RTC
#define CONFIG_RTC_FULL
#define CONFIG_RTC_INTERVAL	255

#define plt_copyright()

#define PTABSIZE	64
#define UFTSIZE		16
#define OFTSIZE		160
#define ITABSIZE	176

#define BOOTDEVICENAMES "hd#"

#define TTY_INIT_BAUD	B9600
