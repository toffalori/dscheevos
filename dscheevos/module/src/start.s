@ DScheevos module header and the ARM entry points the hooks call.
	.syntax unified
	.arch armv5te

	.section .header, "ax"
	.global dscm_header
	.align 2
dscm_header:
	.word 0x56435344            @ DSCM_MAGIC "DSCV"
	.word 7                     @ DSCM_VERSION (the module test checks it matches)
	.word __image_size
	.word init_entry
	.word vblank_entry
	.word halt_entry
	.word module_shutdown
	.word dscm_g_status          @ 0x1C: the module's view (0x02 or 0x0C), for the ARM9 menu
	.word dscm_g_net
dscm_vblank_count:
	.word 0                     @ 0x24: bumped by the ARM7 VBlank hook
	.word reset_entry           @ 0x28
	.global dscm_list_ptr
dscm_list_ptr:
	.word 0                     @ 0x2C: published achievement list (module.c swaps it)
	.word dscm_g_notices        @ 0x30
	.word dscm_g_journal_phys   @ 0x34: physical (the ARM7 reads it); cleaned at each step
	.word flush_entry           @ 0x38: evaluate what is queued, before a journal copy
	.space 4
	.global dscm_notify_count
dscm_notify_count:
	.word 0                     @ 0x40: unlock count for the ARM7 (own cache line)
	.global dscm_net_ring
dscm_net_ring:
	.word 0                     @ 0x44: network ring for the ARM7 SDIO shim
	.global dscm_cues
dscm_cues:
	.word 0                     @ 0x48: LED cues for the ARM7 (same cache line)
	.space 20

@ The wake page (module.c wake_page_t): dscm_irq (frame counting and the
@ wake's flags), g_idle, H and the re-entry guard, the module region's last
@ WAKE_PAGE bytes, read and written through the 0x0C000000 mirror (uncached on
@ most games: no cache line at all) from a base built from immediates, so a
@ wake reads no literal pool (a D-cache line of ours, evicting one of the
@ game's, every wake). The idle stack starts below it, so no cached stack
@ line ever holds its bytes (module.ld checks __stack_top).
	.equ	WAKE_PAGE, 160              @ sizeof(wake_page_t)
	.equ	PAGE_IRQ, 0                 @ offsetof(wake_page_t, irq): dscm_irq_t
	.equ	PAGE_H, 96                  @ offsetof(wake_page_t, h)
	.equ	PAGE_GUARD, 128             @ offsetof(wake_page_t, guard)
	.equ	IRQ_DATA_REGION, 9          @ dscm_irq_t.data_region
	@ module.c's H: what reset_entry writes when the data view is read-only
	.equ	H_RESET_PENDING, 14
	.equ	H_BUSY, 28
	.equ	MIRROR_HI, 0x0C000000
	.equ	REGION_END, 0x00F80000      @ with MIRROR_HI: 0x0CF80000, the region's end, mirror view
	.equ	GUARD_BACK, WAKE_PAGE - PAGE_GUARD   @ the guard: this far below the region's end
	@ from the guard to data_region (negative)
	.equ	GUARD_TO_REGION, PAGE_IRQ + IRQ_DATA_REGION - PAGE_GUARD
	.equ	STACK_TOP_HI, 0x02F80000    @ the region's end, main-RAM view
@ STACK_GAP (Makefile, >= WAKE_PAGE): the idle stack's top is that far below
@ the region's end; the gap's size sets which D-cache sets the wake's few
@ stack lines use.

	.section .header, "ax"

	.arm
@ The VBlank entry is no longer called: the ARM7 bumps dscm_vblank_count and
@ the idle thread reads it (module.c, vblank_poll), so DScheevos runs nothing
@ in the game's interrupts and uses none of its stacks. Kept as a stub so the
@ header layout (and old callers) stay valid.
	.global vblank_entry
	.type vblank_entry, %function
vblank_entry:
	bx	lr

	.section .text.entries, "ax"
	.arm

@ Halt: the game's OS_Halt jumps here. Run idle work on the module stack,
@ then wait for an interrupt as OS_Halt does. A game thread woken by an IRQ
@ preempts the idle thread (and this work) as usual. If the work is already
@ running (preempted, then OS_Halt called again), just halt.
@ With IRQs masked (CPSR.I or IME 0) nothing could preempt the work, so it
@ is skipped. If the work took a scanline or more, an IRQ may already have
@ come, so return without waiting: the caller checks and calls OS_Halt again.
	.global halt_entry
	.type halt_entry, %function
halt_entry:
	@ Nothing goes on the caller's (the game's) stack: until the switch to
	@ the module stack only r0-r3 and r12 are used, which the caller's
	@ OS_Halt call leaves free.
	mrs	r0, cpsr
	tst	r0, #0x80
	bne	2f
	mov	r0, #0x04000000
	ldr	r0, [r0, #0x208]            @ REG_IME
	tst	r0, #1
	beq	2f
	@ The module's data must be writable under the game's MPU right now:
	@ NitroSDK's CTRDG_Init makes the GBA-slot region (0x08000000-
	@ 0x0FFFFFFF, which holds the 0x0C000000 mirror our data goes through)
	@ read-only for data until the ARM7 answers, and CTRDG_Enable(FALSE)
	@ can do it at any time. A write there data-aborts into the game's
	@ exception handler (Professor Layton and Peggle idle inside that
	@ window and froze). Then this wake is the stock halt: nothing of ours
	@ runs. CP15 is only read: the data access permissions (c5) of the
	@ region holding dscm_irq (found once from c6, data_region_scan).
	@ r12 = the guard (wake page), from immediates: no literal pool.
	mov	r12, #MIRROR_HI
	orr	r12, r12, #REGION_END
	sub	r12, r12, #GUARD_BACK
	ldrb	r0, [r12, #GUARD_TO_REGION]
	cmp	r0, #8
	adrhs	r3, 1f
	bhs	data_region_scan            @ r0 = the region, 8 if none (then nothing is writable)
1:	mrc	p15, 0, r1, c5, c0, 2
	mov	r2, r0, lsl #2              @ a register shift of 32 or more gives 0: no access
	mov	r1, r1, lsr r2
	and	r1, r1, #15
	sub	r1, r1, #1
	cmp	r1, #3                      @ 1, 2, 3: privileged read/write
	bhs	2f
	@ the guard: test and set in one step:
	@ a thread that preempts us between a read and a write would otherwise
	@ also take the module stack.
	mov	r0, #1
	swpb	r1, r0, [r12]
	cmp	r1, #0
	bne	2f
	@ r0 = 1 if the caller is NitroSDK's idle loop: the instruction after
	@ its call is a branch back to the call (ARM "b .-4" or Thumb "b .-4").
	@ Read through the 0x0C000000 mirror when it is in main RAM: a data
	@ cache line fill here, every wake, would evict a line of the game's.
	@ SDK5 games map the mirror cached: then the read does fill a line, and
	@ a line left there could go stale (an overlay loaded at that address
	@ through 0x02xxxxxx), so clean and drop it after the read. Clean, not
	@ just drop: nothing of the game's in that line is ever lost.
	bic	r3, lr, #1
	mov	r12, r3, lsr #24
	cmp	r12, #2
	addeq	r3, r3, #0x0A000000         @ 0x02xxxxxx -> 0x0Cxxxxxx
	mov	r0, #0
	tst	lr, #1
	bne	4f
	ldr	r1, [r3]
	mvn	r2, #0x15000000             @ 0xEAFFFFFF
	bic	r2, r2, #2                  @ 0xEAFFFFFD: ARM "b .-4" (no literal pool)
	b	5f
4:	ldrh	r1, [r3]
	mov	r2, #0xE700
	orr	r2, r2, #0xFC               @ 0xE7FC: Thumb "b .-4"
5:	cmp	r12, #2
	mcreq	p15, 0, r3, c7, c14, 1      @ clean and invalidate the mirror's line
	cmp	r1, r2
	moveq	r0, #1
	mov	r12, sp
	mov	sp, #STACK_TOP_HI
	sub	sp, sp, #STACK_GAP          @ = __stack_top (module.ld)
	push	{r12, lr}
	bl	module_idle                 @ ARM code (.text.wake)
	pop	{r12, lr}
	mov	sp, r12
	mov	r1, #MIRROR_HI
	orr	r1, r1, #REGION_END
	mov	r2, #0
	strb	r2, [r1, #-GUARD_BACK]
	cmp	r0, #0
	bxne	lr
2:	mov	r0, #0
	mcr	p15, 0, r0, c7, c0, 4       @ wait for interrupt
	bx	lr

@ The entries below stay where they were before the MPU check grew
@ halt_entry (init_entry and reset_entry moved to .text_cold): hot code at
@ pinned addresses (hot-layout.ref).
	.org	0xEC

@ void dscm_publish_notify(uint32_t count): store the unlock count and clean
@ its cache line to RAM so the ARM7 sees it.
	.global dscm_publish_notify
	.type dscm_publish_notify, %function
dscm_publish_notify:
	ldr	r1, =dscm_notify_count
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1      @ clean D-cache line
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4      @ drain write buffer
	bx	lr

@ void dscm_publish_cues(uint32_t cues): as above, for the LED cue word.
	.global dscm_publish_cues
	.type dscm_publish_cues, %function
dscm_publish_cues:
	ldr	r1, =dscm_cues
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_publish_net_ring(void* ring): as above, for the ring pointer.
	.global dscm_publish_net_ring
	.type dscm_publish_net_ring, %function
dscm_publish_net_ring:
	ldr	r1, =dscm_net_ring
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_publish_list(void* list): store the list pointer (header 0x2C)
@ so that it is in RAM and the line is clean.
	.global dscm_publish_list
	.type dscm_publish_list, %function
dscm_publish_list:
	ldr	r1, =dscm_list_ptr
	@ Through the cache, cleaned before an IRQ can drop the line (the VBlank
	@ IRQ invalidates it to read the ARM7's count): IRQs off for three steps.
	mrs	r3, cpsr
	orr	r2, r3, #0xC0
	msr	cpsr_c, r2
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1      @ clean D-cache line
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4      @ drain write buffer
	msr	cpsr_c, r3
	bx	lr

@ void dscm_dc_clean(const void* addr, uint32_t len): write cached lines to RAM.
	.global dscm_dc_clean
	.type dscm_dc_clean, %function
dscm_dc_clean:
	add	r1, r0, r1
	bic	r0, r0, #31
1:	cmp	r0, r1
	bhs	2f
	mcr	p15, 0, r0, c7, c10, 1
	add	r0, r0, #32
	b	1b
2:	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_dc_invalidate(const void* addr, uint32_t len): drop cached lines
@ so the next read comes from RAM. Only for lines the ARM9 never writes.
	.global dscm_dc_invalidate
	.type dscm_dc_invalidate, %function
dscm_dc_invalidate:
	add	r1, r0, r1
	bic	r0, r0, #31
1:	cmp	r0, r1
	bhs	2f
	mcr	p15, 0, r0, c7, c6, 1
	add	r0, r0, #32
	b	1b
2:	bx	lr

@ void dscm_dc_drain(void): wait until the write buffer has reached RAM.
	.global dscm_dc_drain
	.type dscm_dc_drain, %function
dscm_dc_drain:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_mpu_read(uint32_t out[10]): the ARM9 protection unit as the game
@ set it: regions 0-7 (CP15 c6), then the data-cacheable (c2) and bufferable
@ (c3) bits.
	.global dscm_mpu_read
	.type dscm_mpu_read, %function
dscm_mpu_read:
	mrc	p15, 0, r1, c6, c0, 0
	str	r1, [r0, #0]
	mrc	p15, 0, r1, c6, c1, 0
	str	r1, [r0, #4]
	mrc	p15, 0, r1, c6, c2, 0
	str	r1, [r0, #8]
	mrc	p15, 0, r1, c6, c3, 0
	str	r1, [r0, #12]
	mrc	p15, 0, r1, c6, c4, 0
	str	r1, [r0, #16]
	mrc	p15, 0, r1, c6, c5, 0
	str	r1, [r0, #20]
	mrc	p15, 0, r1, c6, c6, 0
	str	r1, [r0, #24]
	mrc	p15, 0, r1, c6, c7, 0
	str	r1, [r0, #28]
	mrc	p15, 0, r1, c2, c0, 0
	str	r1, [r0, #32]
	mrc	p15, 0, r1, c3, c0, 0
	str	r1, [r0, #36]
	bx	lr

@ uint32_t dscm_vblank_count_read(void): the ARM7 writes the count behind
@ the data cache, so drop the cached line first. Nothing on the ARM9 writes
@ that line (header words and code), so the invalidate loses no data.
	.global dscm_vblank_count_read
	.type dscm_vblank_count_read, %function
dscm_vblank_count_read:
	ldr	r1, =dscm_vblank_count
	mcr	p15, 0, r1, c7, c6, 1
	ldr	r0, [r1]
	bx	lr

@ uint32_t dscm_take(volatile uint32_t* p): return *p and clear it in one
@ step, so an IRQ can't come between the read and the write.
	.global dscm_take
	.type dscm_take, %function
dscm_take:
	mov	r1, #0
	swp	r2, r1, [r0]
	mov	r0, r2
	bx	lr

@ uint32_t dscm_lock(volatile uint32_t* p): set *p to 1 in one step and
@ return what it was (0: the caller holds the lock now).
	.global dscm_lock
	.type dscm_lock, %function
dscm_lock:
	mov	r1, #1
	swp	r2, r1, [r0]
	mov	r0, r2
	bx	lr

@ uint32_t dscm_irq_off(void): mask IRQs (CPSR.I), return the old CPSR.
@ void dscm_irq_restore(uint32_t cpsr): put the old IRQ mask back.
	.global dscm_irq_off
	.type dscm_irq_off, %function
dscm_irq_off:
	mrs	r0, cpsr
	orr	r1, r0, #0x80
	msr	cpsr_c, r1
	bx	lr
	.global dscm_irq_restore
	.type dscm_irq_restore, %function
dscm_irq_restore:
	msr	cpsr_c, r0
	bx	lr

@ void dscm_wfi(void): halt until an IRQ is requested (works with IRQs masked).
	.global dscm_wfi
	.type dscm_wfi, %function
dscm_wfi:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c0, 4
	bx	lr

@ uint32_t dscm_dtcm_base(void): the game's DTCM base from CP15.
	.global dscm_dtcm_base
	.type dscm_dtcm_base, %function
dscm_dtcm_base:
	mrc	p15, 0, r0, c9, c1, 0
	lsr	r0, r0, #12
	lsl	r0, r0, #12
	bx	lr

	.pool

@ The pop-up strip's abort (popup.c, the Cheevotron streamer build; other
@ builds don't reference it, and --gc-sections drops it). While a strip is armed, the BIOS's IRQ
@ entry jumps here through [DTCM+0x3FFC] instead of to the game's handler; the
@ arming put us there with IRQs masked, right before the game's halt. The first
@ IRQ of any kind, before any game code runs:
@  - puts the game's handler back on the vector (so this runs once per arming);
@  - notes the line and IE & IF, for the strip's statistics;
@  - stops our two NDMA channels, recognised by their destinations (NDMA3
@    writes DISPCNT from a table at each H-Blank, NDMA2 feeds the display FIFO),
@    the DISPCNT writer first, and waits a little for a write already in flight;
@  - puts display mode 1 back if the band left mode 3 (the game never uses it:
@    the arming checks for mode 1), byte 2 of DISPCNT only;
@  - jumps to the game's handler, with r0 (DTCM + 0x4000, from the BIOS) and lr
@    (the BIOS's return) as the BIOS left them.
@ So no game code ever sees mode 3, and no DISPCNT write of the game's can be
@ overwritten by a table entry. Only r1-r3 and r12 change (the BIOS saved them)
@ and nothing goes on the stack: the game's handler finds the IRQ stack as the
@ BIOS left it. The words after the code are its data, read and written as
@ data through this code's own (main RAM) address, as popup.c does: one view.
	.pushsection .text.popup, "ax"
	.arm
	.align	2
	.equ	WRAP_ORIG, 0                @ the words after the code, from dscm_wrap_orig
	.equ	WRAP_HITS, 4
	.equ	WRAP_LINE, 8
	.equ	WRAP_IRQS, 12
	.global dscm_irq_wrapper
	.type dscm_irq_wrapper, %function
dscm_irq_wrapper:
	adr	r3, dscm_wrap_orig          @ r3: our words (WRAP_*)
	ldr	r1, [r3, #WRAP_ORIG]
	str	r1, [r0, #-4]               @ the game's handler back on the vector, first
	mov	r12, #0x04000000
	ldrh	r2, [r12, #6]               @ VCOUNT
	str	r2, [r3, #WRAP_LINE]
	ldr	r2, [r12, #0x210]           @ IE
	ldr	r1, [r12, #0x214]           @ IF
	and	r2, r2, r1
	str	r2, [r3, #WRAP_IRQS]
	ldr	r2, [r3, #WRAP_HITS]
	add	r2, r2, #1
	str	r2, [r3, #WRAP_HITS]
	add	r12, r12, #0x4100           @ the NDMA registers
	mov	r3, #0
	ldr	r2, [r12, #0x70]            @ NDMA3CNT
	tst	r2, #0x80000000
	beq	2f
	ldr	r2, [r12, #0x5C]            @ NDMA3DAD: ours writes DISPCNT
	cmp	r2, #0x04000000
	bne	2f
	str	r3, [r12, #0x70]
	mov	r1, #64                     @ bounded: an H-Blank word in flight lands first
1:	ldr	r2, [r12, #0x70]
	tst	r2, #0x80000000
	subsne	r1, r1, #1
	bne	1b
2:	ldr	r2, [r12, #0x54]            @ NDMA2CNT
	tst	r2, #0x80000000
	beq	3f
	ldr	r2, [r12, #0x40]            @ NDMA2DAD: ours feeds DISP_MMEM_FIFO
	sub	r2, r2, #0x04000000
	cmp	r2, #0x68
	streq	r3, [r12, #0x54]
3:	mov	r12, #0x04000000
	ldrb	r2, [r12, #2]               @ DISPCNT bits 16-23: the display mode in bits 0-1
	and	r1, r2, #3
	cmp	r1, #3
	bne	4f
	bic	r2, r2, #3
	orr	r2, r2, #1
	strb	r2, [r12, #2]
	ldrb	r2, [r12, #2]               @ once more, should a last table word have landed
	and	r1, r2, #3
	cmp	r1, #3
	biceq	r2, r2, #3
	orreq	r2, r2, #1
	strbeq	r2, [r12, #2]
4:	adr	r1, dscm_wrap_orig
	ldr	r1, [r1, #WRAP_ORIG]
	bx	r1
	.global dscm_wrap_orig, dscm_wrap_hits, dscm_wrap_line, dscm_wrap_irqs
dscm_wrap_orig:
	.word	0                           @ the game's IRQ handler, saved at arming
dscm_wrap_hits:
	.word	0                           @ entries so far (one per arming)
dscm_wrap_line:
	.word	0                           @ VCOUNT at the last entry
dscm_wrap_irqs:
	.word	0                           @ IE & IF at the last entry
	.size dscm_irq_wrapper, . - dscm_irq_wrapper

@ uint32_t dscm_cp15_control(void): CP15 c1, the control register (bit 13:
@ high exception vectors, so the BIOS's IRQ entry reads [DTCM+0x3FFC]).
	.global dscm_cp15_control
	.type dscm_cp15_control, %function
dscm_cp15_control:
	mrc	p15, 0, r0, c1, c0, 0
	bx	lr
	.popsection

@ Flush: nds-bootstrap's ARM9 calls it with the game stopped, before the ARM7
@ copies the journal (in-game menu, reset). The module stack is free unless
@ idle work is running or parked there (in_halt, test and set as halt_entry
@ does): then it does nothing, and a later safe point writes the journal.
	.pushsection .text_cold, "ax"
	.arm
	.global flush_entry
	.type flush_entry, %function
flush_entry:
	push	{r4, lr}
	@ the data view must be writable now, as halt_entry checks it; if not, a
	@ later safe point writes the journal
	mov	r12, #MIRROR_HI
	orr	r12, r12, #REGION_END
	sub	r12, r12, #GUARD_BACK
	ldrb	r0, [r12, #GUARD_TO_REGION]
	cmp	r0, #8
	adrhs	r3, 1f
	bhs	data_region_scan
1:	mrc	p15, 0, r1, c5, c0, 2
	mov	r2, r0, lsl #2
	mov	r1, r1, lsr r2
	and	r1, r1, #15
	sub	r1, r1, #1
	cmp	r1, #3
	bhs	9f
	mov	r0, #1
	swpb	r1, r0, [r12]
	cmp	r1, #0
	bne	9f
	mov	r4, sp
	ldr	sp, =__stack_top
	push	{r4, r5}
	ldr	r1, =module_flush
	blx	r1
	pop	{r4, r5}
	mov	sp, r4
	mov	r1, #MIRROR_HI
	orr	r1, r1, #REGION_END
	mov	r2, #0
	strb	r2, [r1, #-GUARD_BACK]
9:	pop	{r4, lr}
	bx	lr

@ Init: called once by nds-bootstrap's ARM9 before the game starts, with
@ the session file in r0. Its stack is tiny, so run on the module stack.
	.global init_entry
	.type init_entry, %function
init_entry:
	push	{r4, lr}
	mov	r1, #MIRROR_HI               @ the wake page starts zeroed (that RAM was never ours)
	orr	r1, r1, #REGION_END
	mov	r2, #0
	mov	r3, #WAKE_PAGE / 4
1:	str	r2, [r1, #-4]!
	tst	r1, #31
	mcreq	p15, 0, r1, c7, c14, 1      @ each line to RAM, if the MPU caches the mirror now (the game's may not)
	subs	r3, r3, #1
	bne	1b
	mcr	p15, 0, r2, c7, c10, 4      @ drain write buffer
	mov	r2, #0xFF                   @ data_region: not known yet (data_region_scan)
	strb	r2, [r1, #PAGE_IRQ + IRQ_DATA_REGION]
	mcr	p15, 0, r1, c7, c14, 1
	mov	r2, #0
	mcr	p15, 0, r2, c7, c10, 4
	mov	r4, sp
	ldr	sp, =__stack_top
	push	{r4, r5}
	ldr	r1, =module_init
	blx	r1
	pop	{r4, r5}
	mov	sp, r4
	pop	{r4, lr}
	bx	lr

@ Reset: called from nds-bootstrap's reset path with IRQs off. The game's
@ threads are gone, so work that was preempted will never resume: clear
@ the re-entry guards (the guard, H.busy) and ask for the achievement reset,
@ which happens in idle time (H.reset_pending). The game's MPU may make the
@ data view read-only right now (CTRDG, see halt_entry), so the three
@ writes go to the same RAM through the 0x02000000 view (main RAM, always
@ writable), with the lines of both views written back and dropped around
@ them: no stale copy is left in the data cache. No stack is used.
	.global reset_entry
	.type reset_entry, %function
reset_entry:
	mov	r0, #MIRROR_HI               @ r0: the guard, r1: H (wake page, mirror view)
	orr	r0, r0, #REGION_END
	sub	r0, r0, #GUARD_BACK
	sub	r1, r0, #PAGE_GUARD - PAGE_H
	mcr	p15, 0, r0, c7, c14, 1      @ clean and drop: a cached data view (SDK5) may hold them dirty
	mcr	p15, 0, r1, c7, c14, 1
	mov	r2, #0
	mcr	p15, 0, r2, c7, c10, 4
	bic	r0, r0, #0x0F000000          @ 0x0Cxxxxxx -> 0x02xxxxxx (no change in the cached build)
	orr	r0, r0, #0x02000000
	bic	r1, r1, #0x0F000000
	orr	r1, r1, #0x02000000
	strb	r2, [r0]
	str	r2, [r1, #H_BUSY]
	mov	r2, #1
	strb	r2, [r1, #H_RESET_PENDING]
	mcr	p15, 0, r0, c7, c14, 1
	mcr	p15, 0, r1, c7, c14, 1
	mov	r2, #0
	mcr	p15, 0, r2, c7, c10, 4
	bx	lr

@ The protection region holding dscm_irq (the highest enabled one), from
@ CP15 c6, for halt_entry and flush_entry (r12 = the guard, in the wake page,
@ return address in r3, uses r0-r2). Kept in dscm_irq once it is writable,
@ so this runs on the first wakes only. Returns r0 = the region, 8 if none.
	.macro	DATA_REGION n
	mrc	p15, 0, r1, c6, c\n, 0
	tst	r1, #1
	beq	10f
	mov	r2, r1, lsr #1
	and	r2, r2, #31
	add	r2, r2, #1
	mov	r0, #1
	mov	r2, r0, lsl r2              @ size: 2 << N (0 for 4 GB: every address)
	bic	r1, r1, #0xFF0
	bic	r1, r1, #0xF
	sub	r1, r12, r1                 @ offset from the base
	cmp	r2, #0
	cmpne	r1, r2
	movlo	r0, #\n
	blo	11f
	cmp	r2, #0
	moveq	r0, #\n
	beq	11f
10:
	.endm
	.type data_region_scan, %function
data_region_scan:
	DATA_REGION 7
	DATA_REGION 6
	DATA_REGION 5
	DATA_REGION 4
	DATA_REGION 3
	DATA_REGION 2
	DATA_REGION 1
	DATA_REGION 0
	mov	r0, #8
	mov	pc, r3
	@ found: keep it if the region lets us write it (else the next wake looks again)
11:	mrc	p15, 0, r1, c5, c0, 2
	mov	r2, r0, lsl #2
	mov	r1, r1, lsr r2
	and	r1, r1, #15
	sub	r1, r1, #1
	cmp	r1, #3
	strblo	r0, [r12, #GUARD_TO_REGION]
	mov	pc, r3
	.pool
	.popsection

