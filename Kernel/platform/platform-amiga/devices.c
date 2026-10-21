#include <kernel.h>
#include <version.h>
#include <kdata.h>
#include <printf.h>
#include <devide.h>
#include <blkdev.h>
#include <devsys.h>
#include <tty.h>
#include <vt.h>
#include "amiga.h"
#include "devfd.h"
#include "rdb.h"
#include "amigaioctl.h"

struct devsw dev_tab[] =  /* The device driver switch table */
{
// minor    open         close        read      write       ioctl
// -----------------------------------------------------------------
  /* 0: /dev/hd		Disc block devices  */
  {  blkdev_open,   no_close,    blkdev_read,   blkdev_write, amiga_blk_ioctl },
  /* 1: /dev/fd		Hard disc block devices (absent) */
  {  nxio_open,     no_close,    no_rdwr,   no_rdwr,   no_ioctl },
  /* 2: /dev/tty	TTY devices */
  {  tty_open,     tty_close,   tty_read,  tty_write,  amiga_vt_ioctl },
  /* 3: /dev/lpr	Printer devices */
  {  no_open,     no_close,   no_rdwr,   no_rdwr,  no_ioctl  },
  /* 4: /dev/mem etc	System devices (one offs) */
  {  no_open,      no_close,    sys_read, sys_write, sys_ioctl  },
  /* Pack to 7 with nxio if adding private devices and start at 8 */
};

bool validdev(uint16_t dev)
{
    /* This is a bit uglier than needed but the right hand side is
       a constant this way */
    if(dev > ((sizeof(dev_tab)/sizeof(struct devsw)) << 8) - 1)
	return false;
    else
        return true;
}

unsigned short amiga_root_dev = 0x0001;

/*
 *	Amiga partition tables (rdb.c), on every drive the shared code found
 *	no PC partitions on. blkdev_ioctl() with a request it does not know
 *	only points blk_op.blkdev at the drive: that is how we get to it.
 */
static void rdb_scan(uint_fast8_t drive)
{
  blkdev_ioctl(drive << 4, 0, NULL);
  if (blk_op.blkdev->drive_lba_count && blk_op.blkdev->lba_count[0] == 0 &&
      rdb_parse('a' + drive))
    kputchar('\n');
}

/* BLKRRPART: after setup rewrote the table, read the partitions again
   (the caller has closed and synced them) */
int amiga_blk_ioctl(uint_fast8_t minor, uarg_t request, char *data)
{
  blkdev_t *blk;

  if (request != BLKRRPART)
    return blkdev_ioctl(minor, request, data);
  blkdev_ioctl(minor, 0, NULL);
  blk = blk_op.blkdev;
  memset(blk->lba_first, 0, sizeof(blk->lba_first));
  memset(blk->lba_count, 0, sizeof(blk->lba_count));
  blkdev_scan(blk, 0);
  rdb_scan(minor >> 4);
  return 0;
}

/*
 *	The floppy goes first, so it is always hda and the IDE disk hdb.
 *	The root file system: the boot floppy's Fuzix partition if it has
 *	one; else the IDE disk's Fuzix partition in its Amiga partition
 *	table (it shares the disk with AmigaOS); else its first partition
 *	(the PC partition table of the first disks we made).
 */
void device_init(void)
{
  blkdev_t *fd;
  uint16_t r;

  /* The clock counts ticks per tenth of a second: 6 at 60Hz */
  if (ntsc)
    ticks_per_dsecond = 6;
  rtc_init();
  fd = fd_init();

  devide_init();
  for (r = 0; r < MAX_BLKDEV; r++)
    if (blkdev_open(r << 4, 0) == 0)
      rdb_scan(r);
  udata.u_error = 0;		/* the drives that are not there */
  if (fd && fd->lba_count[0])
    return;				/* hda1 */
  r = rdb_fuzix_minor(fd ? 1 : 0);
  if (r)
    amiga_root_dev = r;
  else
    amiga_root_dev = fd ? 0x0011 : 0x0001;
}
