#ifndef __AMIGA_DOT_H__
#define __AMIGA_DOT_H__

/* Custom chip registers */
#define CUSTOM(r)	(*(volatile uint16_t *)(0xDFF000 + (r)))
#define SERDATR		CUSTOM(0x018)
#define INTREQR		CUSTOM(0x01E)
#define SERDAT		CUSTOM(0x030)
#define SERPER		CUSTOM(0x032)
#define COP1LCH		CUSTOM(0x080)
#define COP1LCL		CUSTOM(0x082)
#define COPJMP1		CUSTOM(0x088)
#define DIWSTRT		CUSTOM(0x08E)
#define DIWSTOP		CUSTOM(0x090)
#define DDFSTRT		CUSTOM(0x092)
#define DDFSTOP		CUSTOM(0x094)
#define DMACON		CUSTOM(0x096)
#define INTENA		CUSTOM(0x09A)
#define INTREQ		CUSTOM(0x09C)
#define BPLCON0		CUSTOM(0x100)
#define BPLCON1		CUSTOM(0x102)
#define BPLCON2		CUSTOM(0x104)
#define BPLCON3		CUSTOM(0x106)
#define BPL1MOD		CUSTOM(0x108)
#define BPL2MOD		CUSTOM(0x10A)
#define BPLCON4		CUSTOM(0x10C)
#define FMODE		CUSTOM(0x1FC)
#define COLOR(n)	CUSTOM(0x180 + 2 * (n))

#define INTF_RBF	0x0800

/* CIA-A: odd byte addresses, register stride 0x100 */
#define CIAA(r)		(*(volatile uint8_t *)(0xBFE001 + ((r) << 8)))
#define CIA_SDR		12
#define CIA_ICR		13
#define CIA_CRA		14

/* Hand-over from the bootblock (crt0.S) */
extern uint32_t chip_top;
extern uint32_t fast_base;
extern uint32_t fast_top;
extern uint16_t attn_flags;

/* Chip RAM above this is the screen; the kernel does not hand it out */
extern uint32_t video_base;

void serial_init(void);
void video_init(void);
void kbd_init(void);
void kbd_interrupt(void);
void serial_interrupt(void);
void plt_vblank(void);
void amiga_hw_init(void);
void rtc_init(void);
int amiga_vt_ioctl(uint_fast8_t minor, uarg_t request, char *data);

#endif
