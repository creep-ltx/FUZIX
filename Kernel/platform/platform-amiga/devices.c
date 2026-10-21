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
 *	The floppy goes first so that when it carries the root file system
 *	it is hda. Otherwise the root is the first IDE disk: hda if there is
 *	no disk in the drive, hdb if there is.
 */
void device_init(void)
{
  blkdev_t *fd = fd_init();

  devide_init();
  if (fd && !fd->lba_count[0])
    amiga_root_dev = 0x0011;
}
