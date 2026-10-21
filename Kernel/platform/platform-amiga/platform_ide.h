/*
 *	Gayle IDE on the A1200 and A600. The task file registers sit on
 *	byte lanes 4 bytes apart starting at base + 2; the 16-bit data port
 *	is at the base. Sector data comes out in the right byte order for
 *	a big-endian CPU, so no byte swapping.
 */
#define IDE_IS_MMIO  1

#define GAYLE_IDE		0x00DA0000

#define IDE_REG_DATA		(GAYLE_IDE)
#define IDE_REG_ERROR		(GAYLE_IDE + 0x06)
#define IDE_REG_FEATURES	(GAYLE_IDE + 0x06)
#define IDE_REG_SEC_COUNT	(GAYLE_IDE + 0x0A)
#define IDE_REG_LBA_0		(GAYLE_IDE + 0x0E)
#define IDE_REG_LBA_1		(GAYLE_IDE + 0x12)
#define IDE_REG_LBA_2		(GAYLE_IDE + 0x16)
#define IDE_REG_LBA_3		(GAYLE_IDE + 0x1A)
#define IDE_REG_DEVHEAD		(GAYLE_IDE + 0x1A)
#define IDE_REG_STATUS		(GAYLE_IDE + 0x1E)
#define IDE_REG_COMMAND		(GAYLE_IDE + 0x1E)
#define IDE_REG_ALTSTATUS	(GAYLE_IDE + 0x101A)
#define IDE_REG_CONTROL		(GAYLE_IDE + 0x101A)

#define ide_select(x)
#define ide_deselect()
