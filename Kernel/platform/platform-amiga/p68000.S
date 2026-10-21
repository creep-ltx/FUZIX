/*
 *	Commodore Amiga: low level start-up, interrupts and IDE transfer.
 *
 *	Interrupts arrive on the 68000 autovectors:
 *	level 1  TBE, disk block, soft	(unused, cleared)
 *	level 2  CIA-A (keyboard), Gayle IDE (kept off)
 *	level 3  vertical blank		-> the system tick
 *	level 4  audio			(unused, cleared)
 *	level 5  serial receive
 *	level 6  CIA-B			(unused, cleared)
 */
#include "../../cpu-68000/kernel-68000.def"

		.globl plt_reboot
		.globl init_early
		.globl init_hardware
		.globl program_vectors
		.globl plt_idle
		.globl outchar
		.globl plt_monitor
		.globl udata_block0
		.globl devide_read_data
		.globl devide_write_data

#define INTENA	$DFF09A
#define INTREQ	$DFF09C

.mri 1
plt_monitor:
plt_reboot:
	    or #$0700,sr
	    ; Jump into the Kickstart ROM's reset code the way exec's
	    ; ColdReboot does: RESET and the jump must share a longword so
	    ; the jump is already fetched when the ROM overlay returns.
	    lea.l $01000000,a0
	    sub.l -$14(a0),a0		; ROM size is stored at $FFFFEC
	    move.l 4(a0),a0		; ROM's initial PC
	    subq.l #2,a0
	    bra doreset
		.align 4
doreset:
	    reset
	    jmp (a0)

init_early:
	    lea.l udata_block0,a5	; udata ptr
	    move.l a5,udata_shadow	; shadow copy for entry/exit
            rts

plt_idle:
	    stop #($2000+EI_MASK)
	    rts

init_hardware:
	    ; RAM size in KB for the kernel's accounting
	    move.l chip_top,d0
	    move.l fast_top,d1
	    sub.l fast_base,d1
	    add.l d1,d0
	    moveq #10,d1
	    lsr.l d1,d0
	    move.w d0,ramsize
	    move.l #_end+512,d1
	    lsr.l #8,d1
	    lsr.l #2,d1
	    sub.w d1,d0
	    move.w d0,procmem

	    bsr install_vectors

	    move.w #0,a0
	    move.l #int_level1,$64(a0)
	    move.l #int_level2,$68(a0)
	    move.l #int_level3,$6C(a0)
	    move.l #int_level4,$70(a0)
	    move.l #int_level5,$74(a0)
	    move.l #int_level6,$78(a0)
	    move.l #int_level7,$7C(a0)

	    jsr amiga_hw_init

	    ; Interrupts on: master, CIA-A ports, vertical blank, serial RX
	    move.w #$7FFF,INTREQ
	    move.w #$C828,INTENA
            rts

;
;	Common interrupt exit: preempt if the tick asked for it, deliver
;	signals. Entered with a0-a1/a5/d0-d1 stacked.
;
int_tail:
	    clr.b U_DATA__U_ININTERRUPT(a5)
	    tst.b U_DATA__U_INSYS(a5)
	    bne no_preempt
	    tst.b need_resched
	    beq no_preempt
	    ;
	    ;	Vanish into the scheduler. Some other task will pop back out
	    ;   and eventually we'll re-appear here and continue.
	    ;
	    move.l U_DATA__U_PTAB(a5),a0
	    ; We might be in STOPPED state in which case do not go back
	    ; to ready !
	    cmp.b #P_RUNNING,P_TAB__P_STATUS_OFFSET(a0)
	    bne no_ready
	    move.b #P_READY,P_TAB__P_STATUS_OFFSET(a0)
	    ori.b #PFL_BATCH,P_TAB__P_FLAGS_OFFSET(a0)
no_ready:
	    bsr switchout
no_preempt:
	    tst.b U_DATA__U_CURSIG(a5)
	    beq no_signal
	    ;   FIXME: this is ugly
	    movem.l (sp)+,a0-a1/a5/d0-d1
	    move.w sr,-(sp)
	    move.w #0,-(sp)
	    movem.l a0-a6/d0-d7,-(sp)
	    move.l (sp),-(sp)
	    bsr exception
	    addq #4,sp
	    movem.l (sp)+,a0-a6/d0-d7
	    addq #4,sp
	    rte
no_signal:
	    movem.l (sp)+,a0-a1/a5/d0-d1
	    rte

int_enter	macro
	    movem.l a0-a1/a5/d0-d1,-(sp)
	    move.l udata_shadow,a5	; set up the register global
	    move.b #1,U_DATA__U_ININTERRUPT(a5)
	    endm

; Vertical blank: the system tick
int_level3:
	    int_enter
	    move.w #$0070,INTREQ	; VERTB, COPER, BLIT
	    move.w #$0070,INTREQ	; twice: the write can lag on fast CPUs
	    jsr plt_vblank
	    bra int_tail

; CIA-A: keyboard
int_level2:
	    int_enter
	    jsr kbd_interrupt
	    move.w #$0008,INTREQ
	    move.w #$0008,INTREQ
	    bra int_tail

; Serial receive buffer full
int_level5:
	    int_enter
	    jsr serial_interrupt
	    bra int_tail

; The rest are not used: clear and return
int_level1:
	    move.w #$0007,INTREQ
	    move.w #$0007,INTREQ
	    rte
int_level4:
	    move.w #$0780,INTREQ
	    move.w #$0780,INTREQ
	    rte
int_level6:
	    tst.b $BFDD00		; CIA-B ICR: reading clears it
	    move.w #$2000,INTREQ
	    move.w #$2000,INTREQ
	    rte
int_level7:
	    rte

;
;	Nothing to do in 68000 - all set up once at boot
;
program_vectors:
	rts

;
;	We do no banking so we need to do nothing here.
;
map_process_always:
map_process:
map_kernel:
map_restore:
map_save:
	rts

; outchar: polled serial output of the character in d0
outchar:
	    btst #5,$DFF018		; SERDATR TBE (bit 13 of the word)
	    beq outchar
	    and.w #$00FF,d0
	    or.w #$0100,d0		; stop bit
	    move.w d0,$DFF030
	    rts

;
;	IDE: 256 words through Gayle's data port
;
devide_read_data:
	move.l blk_op,a0
	move.l #$00DA0000,a1
	move.w #255,d0
devide_read_l:
	move.w (a1),(a0)+
	dbra d0,devide_read_l
	rts

devide_write_data:
	move.l blk_op,a0
	move.l #$00DA0000,a1
	move.w #255,d0
devide_write_l:
	move.w (a0)+,(a1)
	dbra d0,devide_write_l
	rts
