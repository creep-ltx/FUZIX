#include <kernel.h>
#include <version.h>
#include <kdata.h>
#include <devide.h>
#include <blkdev.h>
#include <devsys.h>
#include <tty.h>
#include <vt.h>
#include "amiga.h"
#include "devfd.h"
#include <rdb.h>

struct devsw dev_tab[] =  /* The device driver switch table */
{
// minor    open         close        read      write       ioctl
// -----------------------------------------------------------------
  /* 0: /dev/hd		Disc block devices  */
  {  blkdev_open,   no_close,    blkdev_read,   blkdev_write, blkdev_ioctl },
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

  rtc_init();
  fd = fd_init();

  devide_init();
  if (fd && fd->lba_count[0])
    return;				/* hda1 */
  r = rdb_fuzix_minor(fd ? 1 : 0);
  if (r)
    amiga_root_dev = r;
  else
    amiga_root_dev = fd ? 0x0011 : 0x0001;
}
