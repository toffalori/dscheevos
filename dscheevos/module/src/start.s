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
	.global dscm_rec_ptr
dscm_rec_ptr:
	.word 0                     @ 0x4C: the streamer's recording (dscm_rec_t, physical) for the ARM7
.ifdef DSCM_STREAM
	.word reconnect_entry       @ 0x50: the in-game menu's "Reconnect" (streamer only)
.else
	.word 0
.endif
	.space 12

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
	.equ	IRQ_RECONNECT, 44           @ dscm_irq_t.reconnect (reconnect_entry)
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

@ void dscm_publish_rec(void* rec): as above, for the recording (once, at
@ init: kept out of the hot code).
	.pushsection .text_cold, "ax"
	.arm
	.global dscm_publish_rec
	.type dscm_publish_rec, %function
dscm_publish_rec:
	ldr	r1, =dscm_rec_ptr
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr
	.ltorg
	.popsection

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
@ arming put us there with IRQs masked, right before the game's halt. At the
@ first IRQ, before any game code runs, it puts the game's handler back on
@ the vector and notes the line and IE & IF, for the strip's statistics.
@
@ Then, usually, the band ends here:
@  - stops our two NDMA channels, recognised by their destinations (NDMA3
@    writes DISPCNT from a table at each H-Blank, NDMA2 feeds the display FIFO),
@    the DISPCNT writer first, and waits a little for a write already in flight;
@  - puts display mode 1 back if the band left mode 3 (the game never uses it:
@    the arming checks for mode 1), byte 2 of DISPCNT only;
@  - jumps to the game's handler, with r0 (DTCM + 0x4000, from the BIOS) and lr
@    (the BIOS's return) as the BIOS left them.
@ So no game thread ever sees mode 3, and no DISPCNT write of a game thread
@ can be overwritten by a table entry. Only r1-r3 and r12 change (the BIOS
@ saved them) and nothing goes on the stack: the game's handler finds the IRQ
@ stack as the BIOS left it.
@
@ Passing (popup.c sets WRAP_PASS once it has recognised the game's dispatcher,
@ NitroSDK's OS_IrqHandler, and its OS_IrqHandler_ThreadSwitch): the IPC
@ receive-FIFO interrupt alone (the ARM7's PXI messages, about one a frame,
@ often inside the band), inside the frame's lines, doesn't end the band. We
@ do the dispatcher's part for it (its return on the IRQ stack, IF
@ acknowledged, the table's handler called) and come back here, before
@ ThreadSwitch. If the handler woke no thread (OSi_ThreadInfo.
@ isNeedRescheduling), set no new IRQ check flag (DTCM+0x3FF8) and, in SDK 2.0,
@ left no IRQ wake bits for ThreadSwitch (WRAP_WAKE), ThreadSwitch has nothing
@ to do but wake OS_WaitIrq's sleepers to look at flags that didn't change:
@ we return to the BIOS instead, as it would, and those sleepers look at the
@ next interrupt that isn't passed (the VBlank at the latest). So with the
@ band up only the game's IPC handler runs, never a game thread. Otherwise
@ the band ends and ThreadSwitch runs as usual. IPC sync never passes:
@ nds-bootstrap's in-game menu and reset run in its handler.
@
@ The words after the code are its data, read and written as data through
@ this code's own (main RAM) address, as popup.c does: one view. They are
@ cleaned to RAM before game code runs, so a game's cache invalidate can't
@ undo them.
	.pushsection .text.popup, "ax"
	.arm
	.align	2
	.equ	WRAP_ORIG, 0                @ the words after the code, from dscm_wrap_orig
	.equ	WRAP_HITS, 4
	.equ	WRAP_LINE, 8
	.equ	WRAP_IRQS, 12
	.equ	WRAP_PASS, 16
	.equ	WRAP_PASSES, 20
	.equ	WRAP_WOKE, 24
	.equ	WRAP_OPEN, 28
	.equ	WRAP_TABLE, 32              @ second cache line: popup.c sets these with WRAP_PASS
	.equ	WRAP_SWITCH, 36
	.equ	WRAP_INFO, 40
	.equ	WRAP_WAKE, 44
	.equ	WRAP_FLAGS, 48
	.equ	IPC_RECV, 0x40000           @ IE/IF: IPC receive FIFO not empty
	.equ	PASS_LINES, 191             @ passing only before this line (popup.c LAST_LINE + 1)
	@ the line path's words (popup.c LW_*), lines three to six
	.equ	L_STATE, 64                 @ 1: the line path is installed (popup.c)
	.equ	L_STAGE, 68                 @ 0 watch only, 1 dry (the feed, and mode 1 written), 2 on (mode 3)
	.equ	L_UP, 72                    @ our band is up in this frame
	.equ	L_OWN, 76                   @ we own the VCOUNT IRQ (0: stood down for the game)
	.equ	L_NFIRST, 80                @ the next frame's band (popup.c; latched at VBlank): first line, 0 none
	.equ	L_NSRC, 84                  @ its feed's source,
	.equ	L_NWORDS, 88                @ word count,
	.equ	L_NSCREEN, 92               @ and POWCNT1 bit 15 it was drawn for
	.equ	L_FIRST, 96                 @ this frame's, as latched
	.equ	L_SRC, 100
	.equ	L_WORDS, 104
	.equ	L_SCREEN, 108
	.equ	L_LYC, 112                  @ the LYC we wrote last
	.equ	L_BIOS_LR, 116              @ the BIOS's return while lr serves as a scratch register
	.equ	L_LATCHED, 120              @ VBlanks seen (a latch happened)
	.equ	C_VC, 128                   @ counters: our VCOUNT IRQs
	.equ	C_ARMS, 132                 @ bands switched on
	.equ	C_WHOLE, 136                @ bands that lasted to the VBlank
	.equ	C_CUT, 140                  @ ended by a game write of the mode
	.equ	C_LATE, 144                 @ too late in the line for this frame
	.equ	C_SKIP, 148                 @ held back by a guard
	.equ	C_LEAK, 152                 @ mode 3 found while no band was up: mode 1 put back
	.equ	C_MODE0, 156                @ display mode 0 at a band's end (a GX_DispOff during it)
	.equ	C_REASSERT, 160             @ the VCOUNT IRQ's enables put back after the game cleared them
	.equ	C_ENDED, 164                @ bands ended early (IPC sync, an IRQ in the VBlank lines)
	.equ	C_LATENCY, 168              @ the most lines between LYC and our VCOUNT IRQ's entry
	.equ	C_SPIN, 172                 @ the most spin loops to the band's H-Blank
	.equ	C_STANDDOWN, 176            @ why we stopped owning the VCOUNT IRQ: 1 LYC changed, 2 LYC bit 8
	.equ	C_SKIPWHY, 180              @ guard bits seen (popup.c LINE_SKIP_*)
	.equ	L_T2ADDR, 192               @ seventh line: &OS_IRQTable[2] (the game's VCOUNT handler slot)
	.equ	L_T2VAL, 196                @ what it held at install: a change means the game wants VCOUNT
	.equ	L_NEXPIRE, 200              @ L_LATCHED after which the parameters are stale (popup.c stopped
	                                    @ refreshing them: no idle time): no band
	.equ	L_LYC2, 204                 @ the LYC we wrote before L_LYC (a game DISPSTAT RMW may put it back)
	.equ	C_DMASEEN, 208              @ telemetry: channels seen busy at a band's start or VBlank (dma_busy bits)
	.equ	C_BUSYS, 212                @ bands that started with another channel busy
	.equ	C_BUSYE, 216                @ bands that reached the VBlank with another channel busy
	.equ	L_HOLD, 232                 @ popup.c: 1 = hold the game's CPU through the band's lines (on stage)
	.equ	C_HELD, 236                 @ bands held to their end
	.equ	C_HOLDIPC, 240              @ holds ended early for an IPC sync
	.equ	C_GXS, 244                  @ bands that started with the geometry engine busy or its FIFO not empty
	.equ	C_GXE, 248                  @ bands that reached the VBlank so
	.equ	C_HOLDOUT, 252              @ holds ended by the bound or an odd VCOUNT (the band left up)
	.equ	HOLD_MAX, 8192              @ hold loop passes (1-2.5 us each by the CPU clock: 33 lines take < 2500)
	.equ	HOLD_PAUSE, 32              @ register-only loops between the hold's I/O reads, off the bus
	.equ	SPIN_MAX, 4096
	.equ	REASSERT_MAX, 64            @ the game keeps turning our VCOUNT IRQ off: leave it to the game

@ Our channels off and mode 1 back; r12 = 0x04000000 on entry; r1-r3, r12 change.
	.macro	STOP_BAND
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
4:
	.endm

	.global dscm_irq_wrapper
	.type dscm_irq_wrapper, %function
dscm_irq_wrapper:
	adr	r3, dscm_wrap_orig          @ r3: our words (WRAP_*)
	ldr	r1, [r3, #L_STATE]
	cmp	r1, #0
	bne	line_path                   @ the line path: we stay on the vector (below the words)
	ldr	r1, [r3, #WRAP_ORIG]
	str	r1, [r0, #-4]               @ the game's handler back on the vector, first
	mov	r12, #0x04000000
	ldrh	r2, [r12, #6]               @ VCOUNT
	str	r2, [r3, #WRAP_LINE]
	ldr	r2, [r12, #0x210]           @ IE
	ldr	r1, [r12, #0x214]           @ IF
	and	r2, r2, r1
	str	r2, [r3, #WRAP_IRQS]
	ldr	r1, [r3, #WRAP_PASS]
	cmp	r1, #0
	beq	5f
	cmp	r2, #IPC_RECV               @ that interrupt alone
	bne	5f
	ldr	r1, [r3, #WRAP_LINE]
	cmp	r1, #PASS_LINES
	bhs	5f
	ldr	r1, [r12, #0x208]           @ IME: the dispatcher does nothing while it is 0
	tst	r1, #1
	beq	5f
	@ the dispatcher's part: its return on the IRQ stack (ThreadSwitch pops
	@ it), the interrupt acknowledged, the table's handler called
	str	lr, [sp, #-4]!
	str	r2, [r12, #0x214]           @ IF
	ldr	r1, [r0, #-8]               @ DTCM+0x3FF8: the IRQ check flags before the handler
	str	r1, [r3, #WRAP_FLAGS]
	mov	r1, #1
	str	r1, [r3, #WRAP_OPEN]
	mcr	p15, 0, r3, c7, c10, 1      @ both lines of our words to RAM
	add	r1, r3, #32
	mcr	p15, 0, r1, c7, c10, 1
	mov	r1, #0
	mcr	p15, 0, r1, c7, c10, 4      @ and out of the write buffer
	ldr	r1, [r3, #WRAP_TABLE]
	ldr	r0, [r1, #18 * 4]           @ OS_IRQTable[18], called as the dispatcher does
	adr	lr, 6f
	bx	r0
6:	mrs	r2, cpsr                    @ IRQs masked, whatever the handler left
	orr	r2, r2, #0x80
	msr	cpsr_c, r2
	adr	r3, dscm_wrap_orig
	mov	r2, #0
	str	r2, [r3, #WRAP_OPEN]
	ldr	r1, [r3, #WRAP_INFO]
	ldrh	r1, [r1]                    @ OSi_ThreadInfo.isNeedRescheduling
	cmp	r1, #0
	bne	7f                          @ the handler woke a thread
	ldr	r1, [r3, #WRAP_WAKE]        @ SDK 2.0: IRQ wake bits ThreadSwitch would act on
	cmp	r1, #0
	ldrhne	r1, [r1]
	cmpne	r1, #0
	bne	7f
	mrc	p15, 0, r1, c9, c1, 0
	lsr	r1, r1, #12
	lsl	r1, r1, #12
	add	r1, r1, #0x4000             @ r1: DTCM + 0x4000
	ldr	r2, [r1, #-8]
	ldr	r12, [r3, #WRAP_FLAGS]
	bics	r2, r2, r12
	bne	7f                          @ a new check flag: an OS_WaitIrq waiter goes on
	mov	r12, #0x04000000
	ldrh	r2, [r12, #6]
	cmp	r2, #PASS_LINES
	bhs	8f                          @ the band is over
	add	r12, r12, #0x4100
	ldr	r2, [r12, #0x70]            @ our DISPCNT writer still running?
	tst	r2, #0x80000000
	beq	8f
	ldr	r2, [r12, #0x5C]
	cmp	r2, #0x04000000
	bne	8f
	ldr	r2, [r1, #-4]
	ldr	r12, [r3, #WRAP_ORIG]
	cmp	r2, r12
	bne	8f                          @ the handler changed the vector: leave it be
	adr	r2, dscm_irq_wrapper
	str	r2, [r1, #-4]               @ back on the vector: the band goes on
	ldr	r2, [r3, #WRAP_PASSES]
	add	r2, r2, #1
	str	r2, [r3, #WRAP_PASSES]
	mcr	p15, 0, r3, c7, c10, 1
	ldr	pc, [sp], #4                @ back to the BIOS, as ThreadSwitch with nothing to do
7:	ldr	r2, [r3, #WRAP_WOKE]
	add	r2, r2, #1
	str	r2, [r3, #WRAP_WOKE]
8:	mov	r12, #0x04000000            @ the band ends now, then ThreadSwitch
	ldrh	r2, [r12, #6]
	str	r2, [r3, #WRAP_LINE]
	ldr	r2, [r3, #WRAP_HITS]
	add	r2, r2, #1
	str	r2, [r3, #WRAP_HITS]
	mcr	p15, 0, r3, c7, c10, 1
	STOP_BAND
	adr	r3, dscm_wrap_orig
	ldr	pc, [r3, #WRAP_SWITCH]
5:	ldr	r2, [r3, #WRAP_HITS]        @ the band ends now, then the game's handler
	add	r2, r2, #1
	str	r2, [r3, #WRAP_HITS]
	mcr	p15, 0, r3, c7, c10, 1
	mov	r12, #0x04000000
	STOP_BAND
	adr	r1, dscm_wrap_orig
	ldr	r1, [r1, #WRAP_ORIG]
	bx	r1
	.global dscm_wrap_orig, dscm_wrap_hits, dscm_wrap_line, dscm_wrap_irqs
	.global dscm_wrap_pass, dscm_wrap_passes, dscm_wrap_woke, dscm_wrap_open
	.global dscm_wrap_table, dscm_wrap_switch, dscm_wrap_info, dscm_wrap_wake
	.global dscm_wrap_words
	.balign	32                          @ six cache lines for the words
dscm_wrap_words:                        @ all of them, as an array (popup.c's line path)
dscm_wrap_orig:
	.word	0                           @ the game's IRQ handler, saved at arming
dscm_wrap_hits:
	.word	0                           @ bands ended here so far (one per arming)
dscm_wrap_line:
	.word	0                           @ VCOUNT where the last band ended (else at the last entry)
dscm_wrap_irqs:
	.word	0                           @ IE & IF at the last entry
dscm_wrap_pass:
	.word	0                           @ popup.c: 1 = the IPC receive-FIFO interrupt may pass
dscm_wrap_passes:
	.word	0                           @ IPC interrupts passed with the band going on
dscm_wrap_woke:
	.word	0                           @ IPC interrupts whose handler woke a thread (the band ended)
dscm_wrap_open:
	.word	0                           @ 1 while a handler we called hasn't returned
dscm_wrap_table:
	.word	0                           @ OS_IRQTable
dscm_wrap_switch:
	.word	0                           @ OS_IrqHandler_ThreadSwitch
dscm_wrap_info:
	.word	0                           @ &OSi_ThreadInfo (isNeedRescheduling: its first halfword)
dscm_wrap_wake:
	.word	0                           @ SDK 2.0: the IRQ wake bits' halfword ThreadSwitch reads, else 0
	.word	0                           @ WRAP_FLAGS
	.word	0, 0, 0
	.space	192                         @ L_* and C_*: the line path

@ ---- the line path (popup.c installs it when the game leaves the VCOUNT IRQ
@ unused) ----
@ While a pop-up is up the wrapper stays on the vector. Our own VCOUNT IRQ,
@ at LYC = the band's first line - 2, starts the feed (NDMA2 into the display
@ FIFO) and, at the H-Blank of the line before the band, writes display mode 3
@ into DISPCNT byte 2 by a CPU read-modify-write: only the mode bits change.
@ The band then runs to line 191 through every IRQ and while game threads
@ run; the VBlank IRQ puts mode 1 back before the game's handler runs. A
@ game RMW of DISPCNT in between keeps our mode and its own change; an
@ absolute write ends the band for the frame. Mode 3 found while no band of
@ ours is up (a thread RMW that straddled the VBlank, or GX_DispOn writing
@ back a mode GX_DispOff saved during a band) is put back to 1 at any IRQ. IPC
@ sync (nds-bootstrap's in-game menu and reset, which may not return) and any
@ IRQ in the VBlank lines end a band first. Every path ends in the game's
@ handler with r0 and lr as the BIOS left them; nothing goes on the stack and
@ IRQs stay masked. r3 = our words on entry (from the wrapper's first line).
line_path:
	str	lr, [r3, #L_BIOS_LR]        @ lr serves as scratch until the chain
	mov	r12, #0x04000000
	ldr	r2, [r12, #0x210]
	ldr	r1, [r12, #0x214]
	and	r2, r2, r1                  @ r2: IE & IF
	ldr	r1, [r3, #L_UP]
	cmp	r1, #0
	bne	1f
	ldrb	r1, [r12, #2]               @ mode 3 with no band of ours up: mode 1 back (bit 1 of byte 2),
	tst	r1, #1
	tstne	r1, #2
	beq	1f
	ldr	r0, [r12, #0xB8]            @ unless a legacy DMA feeds the FIFO (the game's own mode 3)
	and	r0, r0, #0xB8000000
	cmp	r0, #0xA0000000
	ldrne	r0, [r12, #0xC4]
	andne	r0, r0, #0xB8000000
	cmpne	r0, #0xA0000000
	ldrne	r0, [r12, #0xD0]
	andne	r0, r0, #0xB8000000
	cmpne	r0, #0xA0000000
	ldrne	r0, [r12, #0xDC]
	andne	r0, r0, #0xB8000000
	cmpne	r0, #0xA0000000
	beq	1f
	bic	r1, r1, #2
	strb	r1, [r12, #2]
	ldr	r1, [r3, #C_LEAK]
	add	r1, r1, #1
	str	r1, [r3, #C_LEAK]
1:	tst	r2, #1                      @ VBlank first: a VCOUNT match pending with it is stale
	bne	line_vblank
	tst	r2, #4
	bne	line_vcount
	ldr	r1, [r3, #L_UP]
	cmp	r1, #0
	beq	line_chain
	tst	r2, #0x10000                @ IPC sync: its handler may never come back
	bne	2f
	ldrh	r1, [r12, #6]
	cmp	r1, #192                    @ in the VBlank lines, the VBlank IRQ still to come
	bhs	2f
	ldr	r2, [r3, #L_LYC]
	cmp	r1, r2                      @ before LYC: a new frame came without a VBlank IRQ of ours
	bhs	line_chain
2:	bl	band_down
	ldr	r1, [r3, #C_ENDED]
	add	r1, r1, #1
	str	r1, [r3, #C_ENDED]
	b	line_chain

@ The band down: mode 1 back if 3, our feed off, L_UP 0. r3 = words, r12 =
@ 0x04000000; r1, r2 change.
band_down:
	mov	r1, #0
	str	r1, [r3, #L_UP]
	ldrb	r1, [r12, #2]
	tst	r1, #1
	tstne	r1, #2
	bicne	r1, r1, #2
	strbne	r1, [r12, #2]
	add	r2, r12, #0x4100
	ldr	r1, [r2, #0x40]             @ NDMA2DAD: ours feeds DISP_MMEM_FIFO
	sub	r1, r1, #0x04000000
	cmp	r1, #0x68
	moveq	r1, #0
	streq	r1, [r2, #0x54]             @ NDMA2CNT off (it stalls once mode 3 is gone)
	bx	lr

@ Telemetry: the channels other than our feed that are busy now, in r0 as
@ C_DMASEEN's bits (0-3 DMA0-3, 4-7 NDMA0-3 without our NDMA2, 8-11 DMA0-3 in
@ geometry-FIFO mode), and the geometry engine (12 busy, 13 its command FIFO
@ not empty, 14 half full or more). r12 = 0x04000000; r1, r2 change.
dma_busy:
	mov	r0, #0
	ldr	r1, [r12, #0xB8]            @ DMA0CNT
	tst	r1, #0x80000000
	beq	1f
	orr	r0, r0, #0x001
	and	r1, r1, #0x38000000         @ start mode 7: the geometry FIFO
	cmp	r1, #0x38000000
	orreq	r0, r0, #0x100
1:	ldr	r1, [r12, #0xC4]            @ DMA1CNT
	tst	r1, #0x80000000
	beq	1f
	orr	r0, r0, #0x002
	and	r1, r1, #0x38000000
	cmp	r1, #0x38000000
	orreq	r0, r0, #0x200
1:	ldr	r1, [r12, #0xD0]            @ DMA2CNT
	tst	r1, #0x80000000
	beq	1f
	orr	r0, r0, #0x004
	and	r1, r1, #0x38000000
	cmp	r1, #0x38000000
	orreq	r0, r0, #0x400
1:	ldr	r1, [r12, #0xDC]            @ DMA3CNT
	tst	r1, #0x80000000
	beq	1f
	orr	r0, r0, #0x008
	and	r1, r1, #0x38000000
	cmp	r1, #0x38000000
	orreq	r0, r0, #0x800
1:	add	r2, r12, #0x4100
	ldr	r1, [r2, #0x1C]             @ NDMA0CNT (nds-bootstrap's card engine)
	tst	r1, #0x80000000
	orrne	r0, r0, #0x010
	ldr	r1, [r2, #0x38]             @ NDMA1CNT (the card engine too)
	tst	r1, #0x80000000
	orrne	r0, r0, #0x020
	ldr	r1, [r2, #0x70]             @ NDMA3CNT
	tst	r1, #0x80000000
	orrne	r0, r0, #0x080
	add	r2, r12, #0x600
	ldr	r1, [r2]                    @ GXSTAT
	tst	r1, #0x08000000             @ the geometry engine busy
	orrne	r0, r0, #0x1000
	tst	r1, #0x04000000             @ its command FIFO empty? else
	orreq	r0, r0, #0x2000
	tst	r1, #0x02000000             @ less than half full? else
	orreq	r0, r0, #0x4000
	bx	lr

line_vcount:
	ldr	r1, [r3, #L_OWN]
	cmp	r1, #0
	beq	line_chain                  @ not ours any more: the game's handler takes it
	mov	r1, #4
	str	r1, [r12, #0x214]           @ acknowledged: the game's dispatcher never sees ours
	ldr	r1, [r3, #C_VC]
	add	r1, r1, #1
	str	r1, [r3, #C_VC]
	ldr	r1, [r3, #L_UP]             @ our band still up from the last frame: its VBlank never
	cmp	r1, #0                      @ reached us (the game masked it), so down with it first
	blne	band_down
	ldrh	r1, [r12, #6]
	ldr	r2, [r3, #L_LYC]
	sub	r1, r1, r2                  @ lines after LYC (a VCOUNT before it reads huge)
	cmp	r1, #256
	bhs	line_chain                  @ before LYC: a stale match (an old LYC), not counted
	ldr	r2, [r3, #C_LATENCY]
	cmp	r1, r2
	strhi	r1, [r3, #C_LATENCY]
	cmp	r1, #2
	bhi	line_chain                  @ too far past our line: nothing this frame
	ldr	r1, [r3, #L_FIRST]
	cmp	r1, #0
	beq	line_chain                  @ no band this frame
	ldr	r1, [r3, #L_STAGE]
	cmp	r1, #0
	beq	line_chain                  @ watching only
	@ guards; r0 holds the reason bit (put back at the chain)
	mov	r0, #1
	ldr	r1, [r12, #0x210]           @ IE
	ldrh	r2, [r12, #4]               @ DISPSTAT
	tst	r1, #1                      @ no VBlank IRQ: nothing would put mode 1 back
	beq	line_skip
	tst	r2, #8
	beq	line_skip
	@ (a game's H-Blank IRQ is no guard: the spin and the hold keep it off
	@ through the band's lines, and its handler runs once at the VBlank)
	mov	r0, #4
	ldr	r1, [r12]                   @ DISPCNT: mode 1, no forced blank
	tst	r1, #0x80
	bne	line_skip
	and	r2, r1, #0x30000
	cmp	r2, #0x10000
	bne	line_skip
	mov	r0, #8
	ldrh	r1, [r12, #0x6C]            @ MASTER_BRIGHT: a full fade (factor 16 or more, the screen
	tst	r1, #0xC000                 @ all black or white) hides it anyway; a partial one dims it
	beq	1f                          @ with the game (HeartGold sets small factors indoors every
	and	r1, r1, #0x1F               @ few frames)
	cmp	r1, #16
	bhs	line_skip
1:
	mov	r0, #128
	ldr	r1, [r12, #0x64]            @ DISPCAPCNT: a capture that takes the main-memory FIFO (source
	tst	r1, #0x80000000             @ B the FIFO, B in the capture) would take the band's words. One
	tstne	r1, #0x02000000             @ of the graphics screen (A) or of VRAM never sees the band:
	tstne	r1, #0x60000000             @ HeartGold captures every 8-16 frames while walking
	bne	line_skip
	mov	r0, #16
	add	r1, r12, #0x300
	ldrh	r1, [r1, #4]                @ POWCNT1: engine A on the screen the band was drawn for
	and	r1, r1, #0x8000
	ldr	r2, [r3, #L_SCREEN]
	cmp	r1, r2
	bne	line_skip
	mov	r0, #32
	add	r1, r12, #0x4100
	ldr	r2, [r1, #0x54]             @ NDMA2: busy (a copy, or a feed not stopped)
	tst	r2, #0x80000000
	bne	line_skip
	mov	r0, #64                     @ a legacy DMA in main-memory display mode: the game feeds the FIFO
	ldr	r1, [r12, #0xB8]
	and	r1, r1, #0xB8000000
	cmp	r1, #0xA0000000
	beq	line_skip
	ldr	r1, [r12, #0xC4]
	and	r1, r1, #0xB8000000
	cmp	r1, #0xA0000000
	beq	line_skip
	ldr	r1, [r12, #0xD0]
	and	r1, r1, #0xB8000000
	cmp	r1, #0xA0000000
	beq	line_skip
	ldr	r1, [r12, #0xDC]
	and	r1, r1, #0xB8000000
	cmp	r1, #0xA0000000
	beq	line_skip
	@ the feed only before line first-1's H-Blank: never a late start
	ldr	r0, [r3, #L_FIRST]
	sub	r0, r0, #1                  @ r0: the line whose H-Blank switches the mode
	ldrh	r1, [r12, #6]
	cmp	r1, r0
	bhi	line_late
	bne	3f
	ldrh	r1, [r12, #4]
	tst	r1, #2
	bne	line_late
3:	add	r2, r12, #0x4100            @ NDMA2: the rows into the display FIFO, from the first mode-3 line
	mov	r1, #0
	str	r1, [r2, #0x54]
	ldr	r1, [r3, #L_SRC]
	str	r1, [r2, #0x3C]
	add	r1, r12, #0x68
	str	r1, [r2, #0x40]
	ldr	r1, [r3, #L_WORDS]
	str	r1, [r2, #0x44]
	mov	r1, #4
	str	r1, [r2, #0x48]
	mov	r1, #0
	str	r1, [r2, #0x4C]
	mov	r1, #0x89000000             @ on, startup 09h (main-memory display), 4-word blocks, fixed destination
	orr	r1, r1, #0x00020000
	orr	r1, r1, #0x00000800
	str	r1, [r2, #0x54]
	mov	r2, #SPIN_MAX               @ to line first-1's H-Blank (bounded)
4:	ldrh	r1, [r12, #6]
	cmp	r1, r0
	bhi	5f                          @ past it
	bne	6f
	ldrh	r1, [r12, #4]
	tst	r1, #2
	bne	7f
6:	subs	r2, r2, #1
	bne	4b
5:	add	r2, r12, #0x4100            @ too late: the feed off again (no mode-3 line has drawn from it)
	mov	r1, #0
	str	r1, [r2, #0x54]
	b	line_late
7:	ldrb	r1, [r12, #2]               @ the mode switch, byte 2 only
	and	r0, r1, #3                  @ still mode 1 and no forced blank (a game thread may have run
	cmp	r0, #1                      @ GX_DispOff since the guards): else nothing this frame
	ldrbeq	r0, [r12]
	tsteq	r0, #0x80
	bne	9f
	ldr	r0, [r12, #0x214]           @ IPC sync pending: nds-bootstrap's menu would run with it up
	ldr	lr, [r12, #0x210]
	and	r0, r0, lr
	tst	r0, #0x10000
	beq	8f
9:	add	r2, r12, #0x4100            @ the feed off again, nothing drawn from it
	mov	r1, #0
	str	r1, [r2, #0x54]
	mov	r0, #4
	b	line_skip
8:	bic	r1, r1, #3
	ldr	r0, [r3, #L_STAGE]
	cmp	r0, #2
	orreq	r1, r1, #3                  @ on: main-memory display from the next line
	orrne	r1, r1, #1                  @ dry: the game's own mode written back
	strb	r1, [r12, #2]
	mov	r1, #1
	str	r1, [r3, #L_UP]
	ldr	r1, [r3, #C_ARMS]
	add	r1, r1, #1
	str	r1, [r3, #C_ARMS]
	rsb	r2, r2, #SPIN_MAX           @ loops spent
	ldr	r1, [r3, #C_SPIN]
	cmp	r2, r1
	strhi	r2, [r3, #C_SPIN]
	bl	dma_busy                    @ telemetry: other channels busy as the band starts
	ldr	r1, [r3, #C_DMASEEN]
	orr	r1, r1, r0
	str	r1, [r3, #C_DMASEEN]
	tst	r0, #0xFF                   @ a DMA channel
	tsteq	r0, #0xF00
	ldrne	r1, [r3, #C_BUSYS]
	addne	r1, r1, #1
	strne	r1, [r3, #C_BUSYS]
	tst	r0, #0x4000                 @ the geometry FIFO half full or more (the engine's busy bit
	ldrne	r1, [r3, #C_GXS]            @ also stays up while a buffer swap waits for the VBlank)
	addne	r1, r1, #1
	strne	r1, [r3, #C_GXS]
	@ the hold test (DSS_DISPLAY_ON_HOLD): the game's CPU kept here, off the
	@ bus, until the band is drawn, so its writes to a full geometry FIFO
	@ can't stall the bus under our feed. An IPC sync ends it early.
	ldr	r1, [r3, #L_STAGE]
	cmp	r1, #2
	bne	line_chain
	ldr	r1, [r3, #L_HOLD]
	cmp	r1, #0
	beq	line_chain
	mov	r2, #HOLD_MAX
1:	mov	r1, #HOLD_PAUSE
2:	subs	r1, r1, #1
	bne	2b
	ldr	r1, [r12, #0x214]
	tst	r1, #1                      @ the VBlank flag up (with or without its IE bit): the band is drawn
	bne	3f
	ldr	r0, [r12, #0x210]
	and	r1, r1, r0
	tst	r1, #0x10000                @ IPC sync due: nds-bootstrap's menu or reset must not wait
	bne	4f
	ldrh	r1, [r12, #6]               @ VCOUNT
	ldr	r0, [r3, #L_FIRST]
	sub	r0, r0, #2
	cmp	r1, r0
	blo	5f                          @ before the band: the VBlank flag never came (never expected)
	subs	r2, r2, #1
	bne	1b
5:	ldr	r1, [r3, #C_HOLDOUT]        @ the bound or an odd line: back to the game, the band up,
	add	r1, r1, #1                  @ unless the VBlank came meanwhile (then its own path)
	str	r1, [r3, #C_HOLDOUT]
	ldr	r2, [r12, #0x210]
	ldr	r1, [r12, #0x214]
	and	r2, r2, r1
	tst	r2, #1
	bne	line_vblank
	b	line_chain
3:	ldr	r1, [r3, #C_HELD]           @ the VBlank's own path next: the band down, then the
	add	r1, r1, #1                  @ game's handler for it
	str	r1, [r3, #C_HELD]
	ldr	r2, [r12, #0x210]
	ldr	r1, [r12, #0x214]
	and	r2, r2, r1                  @ r2: IE & IF, as line_vblank takes it
	b	line_vblank
4:	ldr	r1, [r3, #C_HOLDIPC]        @ the band down first, as at any IPC sync
	add	r1, r1, #1
	str	r1, [r3, #C_HOLDIPC]
	bl	band_down
	b	line_chain
line_late:
	ldr	r1, [r3, #C_LATE]
	add	r1, r1, #1
	str	r1, [r3, #C_LATE]
	b	line_chain
line_skip:
	ldr	r1, [r3, #C_SKIP]
	add	r1, r1, #1
	str	r1, [r3, #C_SKIP]
	ldr	r1, [r3, #C_SKIPWHY]
	orr	r1, r1, r0
	str	r1, [r3, #C_SKIPWHY]
	b	line_chain

line_vblank:
	tst	r2, #4                      @ our VCOUNT match too, held off past the band: dropped
	beq	5f
	ldr	r1, [r3, #L_OWN]
	cmp	r1, #0
	movne	r1, #4
	strne	r1, [r12, #0x214]
5:	ldr	r1, [r3, #L_UP]
	cmp	r1, #0
	beq	2f
	bl	dma_busy                    @ telemetry: other channels busy at the band's end
	ldr	r1, [r3, #C_DMASEEN]
	orr	r1, r1, r0
	str	r1, [r3, #C_DMASEEN]
	tst	r0, #0xFF
	tsteq	r0, #0xF00
	ldrne	r1, [r3, #C_BUSYE]
	addne	r1, r1, #1
	strne	r1, [r3, #C_BUSYE]
	tst	r0, #0x4000
	ldrne	r1, [r3, #C_GXE]
	addne	r1, r1, #1
	strne	r1, [r3, #C_GXE]
	ldrb	r1, [r12, #2]               @ the band's frame is over: counted, then mode 1 back
	and	r1, r1, #3
	ldr	r0, [r3, #L_STAGE]
	cmp	r0, #2
	moveq	r0, #3
	movne	r0, #1
	cmp	r1, r0
	beq	1f
	ldr	r2, [r3, #C_CUT]            @ the game wrote the mode during the band
	add	r2, r2, #1
	str	r2, [r3, #C_CUT]
	cmp	r1, #0
	ldreq	r2, [r3, #C_MODE0]          @ display mode 0: a GX_DispOff saw our mode
	addeq	r2, r2, #1
	streq	r2, [r3, #C_MODE0]
	b	3f
1:	ldr	r2, [r3, #C_WHOLE]
	add	r2, r2, #1
	str	r2, [r3, #C_WHOLE]
3:	bl	band_down
2:	ldr	r1, [r3, #L_NFIRST]         @ the next frame's band, latched
	str	r1, [r3, #L_FIRST]
	ldr	r1, [r3, #L_NSRC]
	str	r1, [r3, #L_SRC]
	ldr	r1, [r3, #L_NWORDS]
	str	r1, [r3, #L_WORDS]
	ldr	r1, [r3, #L_NSCREEN]
	str	r1, [r3, #L_SCREEN]
	ldr	r1, [r3, #L_LATCHED]
	add	r1, r1, #1
	str	r1, [r3, #L_LATCHED]
	ldr	r2, [r3, #L_NEXPIRE]
	subs	r2, r1, r2                  @ popup.c hasn't refreshed them in time (no idle time): no band
	movgt	r1, #0
	strgt	r1, [r3, #L_FIRST]
	ldr	r1, [r3, #L_OWN]
	cmp	r1, #0
	beq	line_chain
	ldr	r1, [r3, #L_T2ADDR]         @ the game's VCOUNT handler slot changed: it wants the IRQ
	ldr	r1, [r1]
	ldr	r2, [r3, #L_T2VAL]
	cmp	r1, r2
	movne	r0, #3
	bne	line_standdown
	ldr	r1, [r3, #C_REASSERT]       @ the game keeps turning it off
	cmp	r1, #REASSERT_MAX
	movhs	r0, #4
	bhs	line_standdown
	ldrb	r1, [r12, #4]
	tst	r1, #0x80                   @ LYC bit 8: not a line we'd write
	movne	r0, #2
	bne	line_standdown
	ldrb	r1, [r12, #5]               @ LYC as we left it (or the one before, put back by a game
	ldr	r2, [r3, #L_LYC]            @ DISPSTAT RMW that straddled our write)? Else the game set it
	cmp	r1, r2
	ldrne	r2, [r3, #L_LYC2]
	cmpne	r1, r2
	movne	r0, #1
	bne	line_standdown
	ldr	r1, [r3, #L_FIRST]
	cmp	r1, #0
	beq	3f
	sub	r1, r1, #2                  @ LYC = the band's first line - 2
	ldr	r2, [r3, #L_LYC]
	cmp	r1, r2
	strne	r2, [r3, #L_LYC2]
	strne	r1, [r3, #L_LYC]
	ldrh	r2, [r12, #4]               @ DISPSTAT (16-bit): LYC in bits 8-15, the rest as read
	and	r2, r2, #0xFF
	orr	r2, r2, r1, lsl #8
	strh	r2, [r12, #4]
3:	ldrh	r1, [r12, #4]
	tst	r1, #0x20
	bne	4f
	mov	r2, #4                      @ the game turned our VCOUNT IRQ off: a stale match first cleared
	str	r2, [r12, #0x214]
	orr	r1, r1, #0x20
	strh	r1, [r12, #4]
	ldr	r2, [r3, #C_REASSERT]
	add	r2, r2, #1
	str	r2, [r3, #C_REASSERT]
4:	ldr	r1, [r12, #0x210]
	tst	r1, #4
	bne	line_chain
	mov	r2, #4
	str	r2, [r12, #0x214]
	orr	r1, r1, #4
	str	r1, [r12, #0x210]
	ldr	r2, [r3, #C_REASSERT]
	add	r2, r2, #1
	str	r2, [r3, #C_REASSERT]
	b	line_chain
line_standdown:
	mov	r1, #0
	str	r1, [r3, #L_OWN]
	str	r0, [r3, #C_STANDDOWN]

line_chain:                          @ our lines to RAM, r0 and lr as the BIOS left them, the game's handler
	add	r1, r3, #64
	mcr	p15, 0, r1, c7, c10, 1
	add	r1, r3, #96
	mcr	p15, 0, r1, c7, c10, 1
	add	r1, r3, #128
	mcr	p15, 0, r1, c7, c10, 1
	add	r1, r3, #160
	mcr	p15, 0, r1, c7, c10, 1
	add	r1, r3, #192
	mcr	p15, 0, r1, c7, c10, 1
	add	r1, r3, #224                @ L_HOLD .. C_HOLDOUT
	mcr	p15, 0, r1, c7, c10, 1
	ldr	lr, [r3, #L_BIOS_LR]
	mrc	p15, 0, r0, c9, c1, 0
	lsr	r0, r0, #12
	lsl	r0, r0, #12
	add	r0, r0, #0x4000
	ldr	r1, [r3, #WRAP_ORIG]
	bx	r1
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

.ifdef DSCM_STREAM
@ Reconnect (streamer only): nds-bootstrap's in-game menu calls it with the
@ game stopped. It only sets dscm_irq.reconnect, written as reset_entry
@ writes H (main RAM view, both views' lines written back and dropped);
@ the next idle pass acts on it (module.c, stream.c dscm_stream_reconnect).
	.global reconnect_entry
	.type reconnect_entry, %function
reconnect_entry:
	mov	r0, #MIRROR_HI
	orr	r0, r0, #REGION_END
	sub	r0, r0, #GUARD_BACK
	sub	r0, r0, #PAGE_GUARD - PAGE_IRQ - IRQ_RECONNECT   @ the word, mirror view
	mcr	p15, 0, r0, c7, c14, 1      @ clean and drop: a cached data view (SDK5) may hold it dirty
	mov	r2, #0
	mcr	p15, 0, r2, c7, c10, 4
	bic	r0, r0, #0x0F000000          @ 0x0Cxxxxxx -> 0x02xxxxxx (no change in the cached build)
	orr	r0, r0, #0x02000000
	mov	r1, #1
	str	r1, [r0]
	mcr	p15, 0, r0, c7, c14, 1
	mcr	p15, 0, r2, c7, c10, 4
	bx	lr
.endif

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

