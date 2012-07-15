/*
 * arch/arm/mach-s5pv210/idle2.S
 *
 * Copyright (c) Samsung Electronics Co. Ltd
 * Copyright (c) 2012 Will Tisdale - <willtisdale@gmail.com>
 *
 * S5PV210 CPU driver helper functions, most from arch/arm/plat-s5p/sleep.S
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/assembler.h>
#include <mach/regs-clock.h>
#include <mach/map.h>
#include <asm/asm-offsets.h>
#include <asm/memory.h>



#define NO_L2_CLEAN
/*
 *	v7_flush_l1_dcache()
 *
 *	Flush the L1 D-cache.
 *
 *
 */
ENTRY(v7_flush_l1_dcache)
	dmb					@ ensure ordering with previous memory accesses
	mrc	p15, 1, r0, c0, c0, 1		@ read clidr
	ands	r3, r0, #0x7000000		@ extract loc from clidr
	mov	r3, r3, lsr #23			@ left align loc bit field
	beq	finished			@ if loc is 0, then no need to clean
	mov	r10, #0				@ start clean at cache level 0
loop1:
	add	r2, r10, r10, lsr #1		@ work out 3x current cache level
	mov	r1, r0, lsr r2			@ extract cache type bits from clidr
	and	r1, r1, #7			@ mask of the bits for current cache only
	cmp	r1, #2				@ see what cache we have at this level
	blt	skip				@ skip if no cache, or just i-cache
	mcr	p15, 2, r10, c0, c0, 0		@ select current cache level in cssr
	isb					@ isb to sych the new cssr&csidr
	mrc	p15, 1, r1, c0, c0, 0		@ read the new csidr
	and	r2, r1, #7			@ extract the length of the cache lines
	add	r2, r2, #4			@ add 4 (line length offset)
	ldr	r4, =0x3ff
	ands	r4, r4, r1, lsr #3		@ find maximum number on the way size
	clz	r5, r4				@ find bit position of way size increment
	ldr	r7, =0x7fff
	ands	r7, r7, r1, lsr #13		@ extract max number of the index size
loop2:
	mov	r9, r4				@ create working copy of max way size
loop3:
	orr	r11, r10, r9, lsl r5		@ factor way and cache number into r11
	orr	r11, r11, r7, lsl r2		@ factor index number into r11
	mcr	p15, 0, r11, c7, c14, 2		@ clean & invalidate by set/way
@	mcr	p15, 0, r11, c7, c10, 2		@ clean by set/way
	subs	r9, r9, #1			@ decrement the way
	bge	loop3
	subs	r7, r7, #1			@ decrement the index
	bge	loop2
skip:
#if !defined(NO_L2_CLEAN)
	add	r10, r10, #2			@ increment cache number
	cmp	r3, r10
	bgt	loop1
#endif
finished:
	mov	r10, #0				@ swith back to cache level 0
	mcr	p15, 2, r10, c0, c0, 0		@ select current cache level in cssr
	dsb
	isb
	mov	pc, lr
ENDPROC(v7_flush_l1_dcache)

ENTRY(v7_flush_cache_for_idle2)
	stmfd	sp!, {r4-r5, r7, r9-r11, lr}
	bl	v7_flush_l1_dcache
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0		@ I+BTB cache invalidate
	ldmfd	sp!, {r4-r5, r7, r9-r11, lr}
	mov	pc, lr
ENDPROC(v7_flush_cache_for_idle2)

ENTRY(s5p_idle2)
	stmfd	sp!, {r4-r5, r7, r9-r11, lr}

	bl	v7_flush_cache_for_idle2

	ldmfd	sp!, {r4-r5, r7, r9-r11, lr}
	dmb
	dsb
	wfi

	b	.

	.text

	/* s5p_idle2_save
	 *
	 * entry:
	 *	r0 = save address (virtual addr of s3c_sleep_save_phys)
	*/

ENTRY(s5p_idle2_save)

	stmfd	sp!, { r3 - r12, lr }

	mrc	p15, 0, r4, c13, c0, 0	@ FCSE/PID
	mrc	p15, 0, r5, c3, c0, 0	@ Domain ID
	mrc	p15, 0, r6, c2, c0, 0	@ Translation Table BASE0
	mrc	p15, 0, r7, c2, c0, 1	@ Translation Table BASE1
	mrc	p15, 0, r8, c2, c0, 2	@ Translation Table Control
	mrc	p15, 0, r9, c1, c0, 0	@ Control register
	mrc	p15, 0, r10, c1, c0, 1	@ Auxiliary control register
	mrc	p15, 0, r11, c1, c0, 2	@ Co-processor access controls
	mrc	p15, 0, r12, c10, c2, 0	@ Read PRRR
	mrc	p15, 0, r3, c10, c2, 1	@ READ NMRR

	ldr	r2, =S5P_INFORM1
	ldr	r1, [r2]
	cmp	r1, #0x0
	bne	idle2_save

	stmia	r0, { r3 - r13 }

	@@ write our state back to RAM
	bl	v7_flush_cache_for_idle2

	@@ jump to final code to send system to sleep
	ldr	r0, =pm_cpu_sleep
	@@ldr	pc, [ r0 ]
	ldr	r0, [ r0 ]
	mov	pc, r0

idle2_save:
	/* Save CP15 registers */
	stmia	r0!, { r3 - r12}

	/* Save SVC status register */
	mrs	r2, spsr
	str	r2, [r0], #4

	/* Save FIQ mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | FIQ_MODE
	msr	cpsr_c, r1
	mrs	r2, spsr
	stmia	r0!, {r2, r8 - r12, sp, lr }

	/* Save ABT mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | ABT_MODE
	msr	cpsr_c, r1
	mrs	r2, spsr
	stmia	r0!, {r2, sp, lr }

	/* Save IRQ mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | IRQ_MODE
	msr	cpsr_c, r1
	mrs	r2, spsr
	stmia	r0!, {r2, sp, lr }

	/* Save UND mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | UND_MODE
	msr	cpsr_c, r1
	mrs	r2, spsr
	stmia	r0!, {r2, sp, lr }
#if 0
	/* Save SYS mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | SYSTEM_MODE
	msr	cpsr_c, r1
	stmia	r0!, {sp, lr }
#endif
	/* Return to SVC mode */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | SVC_MODE
	msr	cpsr_c, r1

	/* Save SVC mode stack pointer register (R13) */
	str	r13, [r0]

	mov 	r0, #0
	ldmfd	sp, { r3 - r12, pc }

	@@ return to the caller, after having the MMU
	@@ turned on, this restores the last bits from the
	@@ stack
resume_idle2_with_mmu:
#if defined(NO_L2_CLEAN)
	ldr	r0, =S5P_INFORM1
	ldr	r1, [r0]
	cmp	r1, #0x0
	beq	skip_idle2_l2_enable

	mrc     p15, 0, r0, c1, c0, 1	@enable L2 cache
	orr     r0, r0, #(1<<1)
	mcr     p15, 0, r0, c1, c0, 1
#endif
skip_idle2_l2_enable:
	mov	r0, #1

	/* delete added mmu table list */
	ldr	r9 , =(PAGE_OFFSET - PLAT_PHYS_OFFSET)
	add	r4, r4, r9
	str	r12, [r4]

	ldmfd	sp!, { r3 - r12, pc }

	.ltorg



	/* sleep magic, to allow the bootloader to check for an valid
	 * image to resume to. Must be the first word before the
	 * s5pv210_cpu_resume entry.
	*/

	.word	0x2bedf00d

	/* s3c_cpu_resume
	 *
	 * resume code entry for bootloader to call
	 *
	 * we must put this code here in the data segment as we have no
	 * other way of restoring the stack pointer after sleep, and we
	 * must not write to the code segment (code is read-only)
	*/

ENTRY(s5p_idle2_resume)
	mov	r0, #PSR_I_BIT | PSR_F_BIT | SVC_MODE
	msr	cpsr_c, r0

	@@ load UART to allow us to print the two characters for
	@@ resume debug

	mov	r1, #0
	mcr	p15, 0, r1, c8, c7, 0		@@ invalidate TLBs
	mcr	p15, 0, r1, c7, c5, 0		@@ invalidate I Cache

	ldr	r1, =0xe010f008 	@ Read INFORM2 register
	ldr	r0, [r1]		@ Load phy_regs_save value
	ldr	r1, =0xe010f004		@ Read INFORM1 register
	ldr	r1, [r1]
	cmp 	r1, #0x0
	bne	idle2_restore_1

	ldmia	r0, { r3 - r13 }
	b	common_idle2_restore_1

idle2_restore_1:
	/* Restore CP15 registers */
	ldmia	r0!, { r3 - r12 }

common_idle2_restore_1:
	mcr	p15, 0, r4, c13, c0, 0	@ FCSE/PID
	mcr	p15, 0, r5, c3, c0, 0	@ Domain ID

	mcr	p15, 0, r8, c2, c0, 2	@ Translation Table Control
	mcr	p15, 0, r7, c2, c0, 1	@ Translation Table BASE1
	mcr	p15, 0, r6, c2, c0, 0	@ Translation Table BASE0

#if defined(NO_L2_CLEAN)
	cmp	r1, #0x0		@ if idle2 wakeup
	bicne	r10, r10, #(1<<1)	@ disable L2cache
	mcr	p15, 0, r10, c1, c0, 1	@ Auxiliary control register
#endif
	mov	r1, #0
	mcr	p15, 0, r1, c8, c7, 0	@ Invalidate I & D TLB

	mov	r1, #0			@ restore copro access controls
	mcr	p15, 0, r11, c1, c0, 2	@ Co-processor access controls
	mcr 	p15, 0, r1, c7, c5, 4

	mcr	p15, 0, r12, c10, c2, 0	@ write PRRR
	mcr	p15, 0, r3, c10, c2, 1	@ write NMRR

	ldr	r1, =0xe010f004		@ Read INFORM1 register
	ldr	r1, [r1]
	cmp 	r1, #0x0
	beq	common_idle2_restore_2

	/* Restore SVC status register */
	ldr	r2, [r0], #4
	msr	spsr, r2

	/* Restore FIQ mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | FIQ_MODE
	msr	cpsr_c, r1
	ldr	r2, [r0], #4
	msr	spsr, r2
	ldmia	r0!, { r8 - r12, sp, lr }

	/* Restore ABT mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | ABT_MODE
	msr	cpsr_c, r1
	ldr	r2, [r0], #4
	msr	spsr, r2
	ldmia	r0!, { sp, lr }

	/* Restore IRQ mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | IRQ_MODE
	msr	cpsr_c, r1
	ldr	r2, [r0], #4
	msr	spsr, r2
	ldmia	r0!, { sp, lr }

	/* Restore UND mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | UND_MODE
	msr	cpsr_c, r1
	ldr	r2, [r0], #4
	msr	spsr, r2
	ldmia	r0!, { sp, lr }
#if 0
	/* Restore SYS mode register */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | SYSTEM_MODE
	msr	cpsr_c, r1
	ldmia	r0!, {sp, lr }
#endif
	/* Return to SVC mode */
	mov	r1, #PSR_I_BIT | PSR_F_BIT | SVC_MODE
	msr	cpsr_c, r1

	/* Restore SVC mode stack pointer register (R13) */
	ldr	r13, [r0]

common_idle2_restore_2:
	/* calculate first section address into r8 */
	mov	r4, r6
	ldr	r5, =0x3fff
	bic	r4, r4, r5
	ldr	r11, =0xe010f000
	ldr	r10, [r11, #0]
	mov	r10, r10 ,LSR #18
	bic	r10, r10, #0x3
	orr	r4, r4, r10

	/* calculate mmu list value into r9 */
	mov 	r10, r10, LSL #18
	ldr	r5, =0x40e
	orr	r10, r10, r5

	/* back up originally data */
	ldr	r12, [r4]

	/* Added list about mmu */
	str	r10, [r4]

	ldr	r2, =resume_idle2_with_mmu
	mcr	p15, 0, r9, c1, c0, 0		@ turn on MMU, etc

        nop
        nop
        nop
        nop
        nop					@ second-to-last before mmu

	mov	pc, r2				@ go back to virtual address

	.ltorg
