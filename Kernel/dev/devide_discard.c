/*-----------------------------------------------------------------------*/
/* IDE interface driver                                                  */
/* 2014-11-02 Will Sowerbutts (unreleased UZI-mark4)                     */
/* 2014-12-22 WRS ported to Fuzix                                        */
/* 2014-12-25 WRS updated to also support P112 GIDE                      */
/* 2015-01-04 WRS updated to new blkdev API                              */
/* 2015-01-25 WRS updated to newer blkdev API                            */
/*-----------------------------------------------------------------------*/

#define _IDE_PRIVATE

#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <stdbool.h>
#include <timer.h>
#include <devide.h>
#include <blkdev.h>

#ifdef CONFIG_IDE

/****************************************************************************/
/* Code in this file is used only once, at startup, so we want it to live   */
/* in the DISCARD segment. sdcc only allows us to specify one segment for   */
/* each source file.                                                        */
/****************************************************************************/

#ifdef IDE_REG_CONTROL

#define IDE_HAS_RESET

/* A platform with a slow tick can wait longer: at least 25ms */
#ifndef IDE_RESET_MS
#define IDE_RESET_MS 25
#endif

static void devide_delay(void)
{
    timer_t timeout;

    timeout = set_timer_ms(IDE_RESET_MS);

    while(!timer_expired(timeout))
       plt_idle();
}

/* Reset depends upon the presence of alt control, which is optional */
void devide_reset(void)
{
    kputs("IDE reset\n");

    /* reset both drives */
    devide_writeb(ide_reg_devhead, 0xE0); /* select master */
    devide_writeb(ide_reg_control, 0x06); /* assert reset, no interrupts */
    devide_delay();

    devide_writeb(ide_reg_control, 0x02); /* release reset, no interrupts */
    devide_delay();
}
#endif

void devide_init_drive(uint_fast8_t drive)
{
    blkdev_t *blk;
    uint8_t *buffer;
    uint_fast8_t select;
#ifdef CONFIG_IDE_AUTO
    uint_fast8_t i;
#endif

    select = (drive & 1) ? 0xF0 : 0xE0;

    ide_select(drive);

    devide_writeb(ide_reg_devhead, select);
    kprintf("IDE drive %d: ", drive);

    /* Cleaner way to probe */
    devide_writeb(ide_reg_lba_0, 0xAA);
    if (devide_readb(ide_reg_lba_0) != 0xAA)
        goto out;
    devide_writeb(ide_reg_lba_0, 0x55);
    if (devide_readb(ide_reg_lba_0) != 0x55)
        goto out;

#ifdef IDE_8BIT_ONLY
    if (IDE_IS_8BIT(drive)) {
    /* set 8-bit mode -- mostly only supported by CF cards */
        if (!devide_wait(IDE_STATUS_READY))
            goto out;

        devide_writeb(ide_reg_devhead, select);
        if (!devide_wait(IDE_STATUS_READY))
            goto out;

        devide_writeb(ide_reg_features, 0x01); /* Enable 8-bit PIO transfer mode (CFA feature set only) */
        devide_writeb(ide_reg_command, IDE_CMD_SET_FEATURES);
    }
#endif

    /* confirm drive has LBA support */
    if (!devide_wait(IDE_STATUS_READY))
        goto out;

#ifdef CONFIG_IDE_DRAIN
    /* A transfer the boot firmware left unfinished keeps DRQ up (seen
       on an emulated A1200 booted from the disk: the "identify data"
       was the last sector the boot ROM read). Read it away first. */
    if (devide_readb(ide_reg_status) & IDE_STATUS_DATAREQUEST) {
        uint16_t n = 0;
        buffer = (uint8_t *)tmpbuf();
        blk_op.is_user = false;
        blk_op.addr = buffer;
        blk_op.nblock = 1;
        while ((devide_readb(ide_reg_status) & IDE_STATUS_DATAREQUEST) && n++ < 512)
            devide_read_data();
        tmpfree(buffer);
    }
#endif

    /* allocate temporary sector buffer memory */
    buffer = (uint8_t *)tmpbuf();

#ifdef CONFIG_IDE_AUTO
    /* An emulated drive just used by the boot ROM has handed back stale
       sector data as the identify block: check the default geometry
       (every drive reports one) and ask again if it makes no sense */
    for (i = 0; i < 4; i++) {
#endif
    /* send identify command */
    devide_writeb(ide_reg_devhead, select);
    devide_writeb(ide_reg_command, IDE_CMD_IDENTIFY);

    if (!devide_wait(IDE_STATUS_DATAREQUEST))
	goto failout;

    blk_op.is_user = false;
    blk_op.addr = buffer;
    blk_op.nblock = 1;
    devide_read_data();
#ifdef CONFIG_IDE_AUTO
    if (buffer[6] >= 1 && buffer[6] <= 16 && buffer[12] >= 1 &&
        buffer[12] <= 63 && (buffer[2] | buffer[3]))
        break;
#ifdef IDE_HAS_RESET
    devide_delay();
#endif
    }
    if (i == 4) {
        kputs("no sense in its identify data.");
        goto failout;
    }
#endif

#if defined(CONFIG_IDE_AUTO)
    /* LBA when the drive has it, else cylinder/head/sector */
#ifdef CONFIG_IDE_BSWAP
    ide_chs[drive] = !(buffer[98] & 0x02);
#else
    ide_chs[drive] = !(buffer[99] & 0x02);
#endif
    if (ide_chs[drive]) {
#endif
#if defined(CONFIG_IDE_CHS) || defined(CONFIG_IDE_AUTO)
    /* TODO: CHS + BSWAP */
    ide_spt[drive] = buffer[12];
    ide_heads[drive] = buffer[6];
    ide_cyls[drive] = buffer[2]|(buffer[3] << 8);
    kprintf(" C/H/S %u/%u/%u", ide_cyls[drive], buffer[6], buffer[12]);

    devide_writeb(ide_reg_devhead, select | (buffer[6] - 1));
    devide_writeb(ide_reg_sec_count, buffer[12]);
    devide_writeb(ide_reg_lba_1, buffer[2]);
    devide_writeb(ide_reg_lba_2, buffer[3]);
    devide_writeb(ide_reg_command, IDE_CMD_INIT_DEV_PARAM);
    if (!devide_wait(IDE_STATUS_READY))
        goto out;
#endif
#if defined(CONFIG_IDE_AUTO)
    }
#elif !defined(CONFIG_IDE_CHS)
#ifdef CONFIG_IDE_BSWAP
    if(!(buffer[98] & 0x02)) {
#else
    if(!(buffer[99] & 0x02)) {
#endif
        kputs("LBA unsupported.");
        goto failout;
    }
#endif
    blk = blkdev_alloc();
    if(!blk)
	goto failout;

    blk->transfer = devide_transfer_sector;
    blk->flush = devide_flush_cache;
    blk->driver_data = drive & IDE_DRIVE_NR_MASK;

    if( !(((uint16_t*)buffer)[82] == 0x0000 && ((uint16_t*)buffer)[83] == 0x0000) ||
         (((uint16_t*)buffer)[82] == 0xFFFF && ((uint16_t*)buffer)[83] == 0xFFFF) ){
	/* command set notification is supported */
	if(buffer[164] & 0x20){
	    /* write cache is supported */
            blk->driver_data |= FLAG_WRITE_CACHE;
	}
    }

#if defined(CONFIG_IDE_CHS)
    blk->drive_lba_count = ide_heads[drive] * (unsigned int)ide_spt[drive] * (unsigned long)ide_cyls[drive];
#else
    /* read out the drive's sector count */
    blk->drive_lba_count = le32_to_cpu(*((uint32_t*)&buffer[120]));
#ifdef CONFIG_IDE_AUTO
    if (ide_chs[drive])
        blk->drive_lba_count = ide_heads[drive] * (unsigned int)ide_spt[drive] * (unsigned long)ide_cyls[drive];
#endif
#endif

    /* done with our temporary memory */
    tmpfree(buffer);

    /* Deselect the IDE, as we will re-select it in the partition scan and
       it may not recursively stack de-selections */
    ide_deselect();

    /* scan partitions */
    blkdev_scan(blk, SWAPSCAN);
    return;
failout:
    tmpfree(buffer);
out:
    kputchar('\n');
    ide_deselect();
    return;
}

void devide_init(void)
{
    uint_fast8_t d;

#ifdef IDE_HAS_RESET
    devide_reset();
#endif

    for(d=0; d < IDE_DRIVE_COUNT; d++)
        devide_init_drive(d);
}

#endif /* CONFIG_IDE */
