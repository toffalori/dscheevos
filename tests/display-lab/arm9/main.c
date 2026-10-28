/*
 * display-lab: DSi hardware tests for the DScheevos notice strip
 * (docs/display-hw-tests.md has the questions and the pass marks).
 *
 * The top screen (engine A) shows a scrolling stand-in "game"; the strip is
 * the display library's own rendering of an unlock, shown with engine A's
 * Main Memory Display mode fed by NDMA0 (startup 09h). The bottom screen is
 * the log. Results go to sd:/dscheevos-lab/log.txt between tests.
 *
 * Two IRQ setups: libnds's handler while logging (SD and the ARM7 LED
 * command use the FIFO), and a raw handler during the strip tests, where
 * the timing is the point.
 */
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include "dsd.h"

extern const dsd_font_t dsd_font_latin;

#define N 32                          /* strip lines */
#define M 4                           /* margin lines fed past the strip */
#define LINE_TICKS_8M 532             /* one scanline (2130 cycles at 33.5 MHz) at prescaler 1 */

#define REG32(a) (*(vu32*)(a))
#define REG8(a)  (*(vu8*)(a))
#define NDMA_SAD(x)   REG32(0x04004104 + (x) * 0x1C)
#define NDMA_DAD(x)   REG32(0x04004108 + (x) * 0x1C)
#define NDMA_TCNT(x)  REG32(0x0400410C + (x) * 0x1C)
#define NDMA_WCNT(x)  REG32(0x04004110 + (x) * 0x1C)
#define NDMA_BCNT(x)  REG32(0x04004114 + (x) * 0x1C)
#define NDMA_FDATA(x) REG32(0x04004118 + (x) * 0x1C)
#define NDMA_CNT(x)   REG32(0x0400411C + (x) * 0x1C)
#define SCFG_EXT9     REG32(0x04004008)
#define DISPCNT_A     REG32(0x04000000)
#define DISPCNT_MODE  REG8(0x04000002)   /* bits 16-23: display mode in bits 0-1 */
#define MMEM_FIFO     0x04000068

#define NDMA_ON       BIT(31)
#define NDMA_IRQ      BIT(30)
#define NDMA_FOREVER  BIT(29)
#define NDMA_START(m) ((u32)(m) << 24)
#define NDMA_FILL     (3u << 13)
#define NDMA_DFIX     (2u << 10)
#define NDMA_PHYS(n)  ((u32)(n) << 16)  /* 1 << n words */
#define IRQ_NDMA(x)   BIT(28 + (x))

/* ---------------------------------------------------------------- log */

static char logbuf[16384];
static int loglen;

static void say(const char* fmt, ...) {
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	iprintf("%s\n", b);
	const int n = strlen(b);
	if (loglen + n + 2 < (int)sizeof logbuf) {
		memcpy(logbuf + loglen, b, n);
		loglen += n;
		logbuf[loglen++] = '\n';
	}
	/* libnds's nocashMessage copies into a ~120-byte buffer inside its own
	 * code: longer strings overwrite the code after it. Send chunks. */
	char m[100];
	for (int at = 0; at == 0 || at < n; at += 90) {
		snprintf(m, sizeof m, "@@%.90s%s", b + at, at + 90 >= n ? "\n" : "");
		nocashMessage(m);
	}
}

static bool sd_ok;
static void flush_log(void) {
	if (!sd_ok || !loglen) return;
	FILE* f = fopen("sd:/dscheevos-lab/log.txt", "a");
	if (!f) f = fopen("fat:/dscheevos-lab/log.txt", "a");
	if (f) {
		fwrite(logbuf, 1, loglen, f);
		fclose(f);
	}
	loglen = 0;
}

/* ---------------------------------------------------------------- the strip */

static u16 banner[256 * (N + M)] __attribute__((aligned(32)));

static const char* lab_text(void* ctx, const dsd_item_t* it, int which) {
	(void)ctx; (void)it;
	return which == DSD_TEXT_TITLE ? "Display lab: test strip" : "If you can read this, the FIFO feed works";
}

static void make_banner(void) {
	static dsd_t d;
	static dscm_notices_t n;
	const dsd_config_t cfg = { .font = &dsd_font_latin, .text = lab_text, .hardcore = 1 };
	dsd_init(&d, &cfg);
	memset(&n, 0, sizeof n);
	n.magic = DSCM_NOTICES_MAGIC;
	n.head = 1;
	n.ring[1].seq = 1;
	n.ring[1].kind = DSCM_NOTICE_UNLOCK;
	n.ring[1].id = 1;
	n.ring[1].value = 10;
	dsd_ingest(&d, &n);
	const dsd_caps_t caps = { N, 0 };
	dsd_tick(&d, 1, &caps);
	dsd_surface_t s = { banner, 256, N, 256 };
	while (!dsd_render(&d, &s, N)) {}
	for (int y = N; y < N + M; y++)                 /* margin: the strip's background */
		for (int x = 0; x < 256; x++) banner[y * 256 + x] = banner[(N - 2) * 256 + x];
	DC_FlushRange(banner, sizeof banner);
}

static inline void mode_set(int m) { DISPCNT_MODE = (u8)((DISPCNT_MODE & ~3) | m); }
static inline int mode_get(void) { return DISPCNT_MODE & 3; }

static void arm_fifo(void) {
	NDMA_CNT(0) = 0;
	NDMA_SAD(0) = (u32)banner;
	NDMA_DAD(0) = MMEM_FIFO;
	NDMA_TCNT(0) = (N + M) * 128;
	NDMA_WCNT(0) = 4;
	NDMA_BCNT(0) = 0;
	NDMA_CNT(0) = NDMA_ON | NDMA_START(0x09) | NDMA_PHYS(2) | NDMA_DFIX;
}

/* ---------------------------------------------------------------- stand-in game */

static int scroll;

static void setup_game(void) {
	videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE);
	vramSetBankA(VRAM_A_MAIN_BG);
	REG_BG0CNT = BG_32x32 | BG_COLOR_16 | BG_MAP_BASE(0) | BG_TILE_BASE(1) | BG_PRIORITY(1);
	u8 tiles[32];
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x += 2) {
			const int p0 = (((x + y) >> 1) & 1) ? 1 : 2, p1 = (((x + 1 + y) >> 1) & 1) ? 1 : 2;
			tiles[y * 4 + x / 2] = p0 | (p1 << 4);
		}
	memcpy((void*)0x06004000, tiles, sizeof tiles);
	for (int b = 0; b < 16; b++) {
		BG_PALETTE[b * 16 + 1] = RGB15((b * 2) & 31, 31 - b * 2, 10 + b);
		BG_PALETTE[b * 16 + 2] = RGB15(4, 4 + b, 4);
	}
	BG_PALETTE[0] = RGB15(0, 0, 8);
	u16* map = (u16*)0x06000000;
	for (int r = 0; r < 32; r++)
		for (int c = 0; c < 32; c++) map[r * 32 + c] = (u16)((r % 16) << 12);
	lcdMainOnTop();
}

/* ---------------------------------------------------------------- raw IRQ phase */

enum { T_NONE, T_H1, T_H2A, T_H2B, T_NEW, T_STRESS_NDMA, T_STRESS_DMA3, T_BOTTOM, T_T1, T_COUNTER, T_D1, T_P };
static volatile int test;
static volatile u32 frame;

/* H1 / H5 / H6: the old arming (mode 3 at VBlank), switch back from the main loop */
static volatile u32 fifo_done, armed_old;

/* H2 */
static volatile u32 h2_ticks;               /* BCNT interval at prescaler 1 */
static volatile u32 h2_irqs, h2_irq_frames, h2_last_irq_frame, h2_hist[263];
static volatile u32 h2_t33[4], h2_t33n;     /* a few precise delays (33.5 MHz ticks) */
static u32 dummy __attribute__((aligned(32)));

/* new timing (H3, H6b, H8) */
static volatile int new_phase;              /* 0 idle, 1 strip on (waiting exit), 2 guard */
static volatile u32 entries, late_entries, exits, guard_fixes, byte_clobber;
static volatile u32 exit_late_hist[16];
static volatile int use_vblank_start;       /* H2b worked: NDMA2 re-fires by itself */

/* H10: bottom edge, lines 192-N..191, entered from an H-Blank count from line 0 */
static volatile int bottom_arm_at_vblank;   /* 1: FIFO armed at VBlank, 0: at entry */
static volatile int bottom_phase;
static volatile u32 bottom_entries, bottom_exits, bottom_entry_hist[4], bottom_exit_hist[4];
static volatile int bottom_every_other;     /* H11a: strip on even frames only */

/* T1: the slack probe. Strip lines S..S+N-1 end K lines before VBlank. */
#define T1_K 2
#define T1_S (192 - N - T1_K)
static volatile int t1_strip;               /* strip allowed this run */
static volatile int t1_in_wait;             /* the stand-in game is waiting for VBlank */
static volatile int t1_phase;
static volatile u32 t1_shown, t1_skipped, t1_skip_wait, t1_skip_line, t1_skip_pending, t1_last_vc, t1_last_pending;

static void timer_start(void) {
	TIMER0_CR = 0; TIMER1_CR = 0;
	TIMER0_DATA = 0; TIMER1_DATA = 0;
	TIMER1_CR = TIMER_ENABLE | TIMER_CASCADE;
	TIMER0_CR = TIMER_ENABLE | TIMER_DIV_1;
}
static u32 timer_read(void) { return TIMER0_DATA | (u32)TIMER1_DATA << 16; }

static void ndma2_oneshot(u32 ticks) {      /* H2a: start now, IRQ after one interval */
	NDMA_CNT(2) = 0;
	NDMA_DAD(2) = (u32)&dummy;
	NDMA_FDATA(2) = 0;
	NDMA_WCNT(2) = 2;
	NDMA_TCNT(2) = 2;
	NDMA_BCNT(2) = ticks | (1u << 16);
	NDMA_CNT(2) = NDMA_ON | NDMA_IRQ | NDMA_START(0x10) | NDMA_FILL | NDMA_DFIX | NDMA_PHYS(0);
}

static void ndma2_vblank(u32 ticks) {       /* H2b: V-Blank startup, repeat forever */
	NDMA_CNT(2) = 0;
	NDMA_DAD(2) = (u32)&dummy;
	NDMA_FDATA(2) = 0;
	NDMA_WCNT(2) = 2;
	NDMA_TCNT(2) = 2;
	NDMA_BCNT(2) = ticks | (1u << 16);
	NDMA_CNT(2) = NDMA_ON | NDMA_IRQ | NDMA_FOREVER | NDMA_START(0x06) | NDMA_FILL | NDMA_DFIX | NDMA_PHYS(0);
}

static volatile int ndma3_to_io;            /* H11b/T1: fill writes go to an I/O register, not RAM */
static void ndma3_hblanks(u32 count) {      /* IRQ at the H-Blank ending the count-th visible line */
	NDMA_CNT(3) = 0;
	NDMA_DAD(3) = ndma3_to_io ? (u32)&NDMA_FDATA(3) : (u32)&dummy;
	NDMA_FDATA(3) = 0;
	NDMA_WCNT(3) = 1;
	NDMA_TCNT(3) = count;
	NDMA_BCNT(3) = 0;
	NDMA_CNT(3) = NDMA_ON | NDMA_IRQ | NDMA_START(0x07) | NDMA_FILL | NDMA_DFIX | NDMA_PHYS(0);
}

/* Line ~261: the new entry. */
static void strip_entry(void) {
	const u32 vc = REG_VCOUNT;
	entries++;
	if (vc < 259 || vc > 262) { late_entries++; return; }
	const u32 before = DISPCNT_A;
	arm_fifo();
	mode_set(3);
	if ((DISPCNT_A & 0xFF00FFFF) != (before & 0xFF00FFFF)) byte_clobber++;
	new_phase = 1;
	ndma3_hblanks(N);
}

static void strip_exit_or_guard(void) {
	const u32 vc = REG_VCOUNT;
	if (new_phase == 1) {
		if (mode_get() == 3) mode_set(1);
		exits++;
		int late = (int)vc - (N - 1);
		if (late < 0) late = 0;
		if (late > 15) late = 15;
		exit_late_hist[late]++;
		new_phase = 2;
		ndma3_hblanks(M);
	} else if (new_phase == 2) {
		if (mode_get() == 3) { mode_set(1); guard_fixes++; }
		new_phase = 0;
	}
}

static VoidFn saved_handler;
static u32 saved_ie;
#define RAW_BITS (IRQ_VBLANK | IRQ_NDMA(2) | IRQ_NDMA(3) | IRQ_VCOUNT | IRQ_TIMER2)

/* P1 (the production setup, dsi-popup-plan.md and its two reviews): the
 * "game" IRQ handler's view. It must never run with mode 3 on, and its own
 * DISPCNT writes (bit 22, invisible here: no bitmap sprites) must stick. */
static volatile u32 p_irqs, p_saw3, p_lost, p_bad_vblank, p_frames;
static volatile u32 p_written = 2;          /* the bit-22 value the game wrote this frame; 2: none */
static volatile u32 p_marker;

/* D1 (round2-synthesis.md): the all-NDMA strip. While the "game" waits,
 * up to three channels are armed and then nothing of ours runs. Channels
 * follow the design's plan, clear of nds-bootstrap's card engine (NDMA0):
 *   D1_F (NDMA2): the FIFO feed (startup 09h);
 *   D1_A (NDMA3): on each H-Blank, writes a whole DISPCNT word from a table
 *          (mode 1, then mode 3 for lines E..190, then mode 1 at 191);
 *   D1_B (NDMA1): on each H-Blank, writes D1_F's control word from a table
 *          (0, then the enable value at line E-1), so the FIFO starts with no
 *          CPU. Variant "no starter": D1_F is enabled at arming instead, which
 *          works only if the FIFO isn't drained while the mode is 1 (H10a).
 * A game IRQ while armed runs the abort first: stop all three, mode 1. */
#define D1_F 2
#define D1_A 3
#define D1_B 1
#define D1_E (191 - N)                      /* first strip line; the strip is E..190 */
#define D1_ARM 100                          /* line the stand-in game goes idle */
static u32 d1_dtab[192] __attribute__((aligned(32)));
static u32 d1_btab[192] __attribute__((aligned(32)));
static volatile int d1_armed, d1_abort_every;
static volatile u32 d1_frames, d1_shown, d1_bad_vblank, d1_aborts;
static const u16* d1_src;                   /* FIFO source: main RAM or NWRAM */
static volatile int d1_no_starter;          /* enable the FIFO at arming, no D1_B */

static void d1_abort(void) {
	NDMA_CNT(D1_A) = 0;
	NDMA_CNT(D1_B) = 0;
	NDMA_CNT(D1_F) = 0;
	while (NDMA_CNT(D1_A) & NDMA_ON) {}     /* no in-flight write may land after the restore */
	if (mode_get() == 3) mode_set(1);
	d1_armed = 0;
}

static u32 d1_fifo_cnt(void) { return NDMA_ON | NDMA_START(0x09) | NDMA_PHYS(2) | NDMA_DFIX; }

/* Arm at line D1_ARM, with IRQs off (atomic arming). */
static void d1_arm(void) {
	const int ime = REG_IME;
	REG_IME = 0;
	const u32 vc = REG_VCOUNT;
	const u32 d = (DISPCNT_A & ~0x30000u) | 0x10000u;    /* captured now: the game is idle */
	const u32 n = 191 - vc;                  /* H-Blanks of lines vc..190 */
	for (u32 i = 0; i < n; i++) {
		const u32 line = vc + i + 1;         /* the line this H-Blank's write affects */
		d1_dtab[i] = (line >= D1_E && line <= 190) ? (d | 0x30000u) : d;
		d1_btab[i] = (line == D1_E) ? d1_fifo_cnt() : 0;
	}
	DC_FlushRange(d1_dtab, sizeof d1_dtab);
	DC_FlushRange(d1_btab, sizeof d1_btab);
	NDMA_CNT(D1_F) = 0;
	NDMA_SAD(D1_F) = (u32)d1_src;
	NDMA_DAD(D1_F) = MMEM_FIFO;
	NDMA_TCNT(D1_F) = (N + M) * 128;
	NDMA_WCNT(D1_F) = 4;
	NDMA_BCNT(D1_F) = 0;
	NDMA_CNT(D1_A) = 0;
	NDMA_SAD(D1_A) = (u32)d1_dtab;
	NDMA_DAD(D1_A) = 0x04000000;
	NDMA_TCNT(D1_A) = n;
	NDMA_WCNT(D1_A) = 1;
	NDMA_BCNT(D1_A) = 0;
	NDMA_CNT(D1_A) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX | NDMA_PHYS(0);
	if (d1_no_starter) {
		NDMA_CNT(D1_F) = d1_fifo_cnt();
	} else {
		NDMA_CNT(D1_B) = 0;
		NDMA_SAD(D1_B) = (u32)d1_btab;
		NDMA_DAD(D1_B) = (u32)&NDMA_CNT(D1_F);
		NDMA_TCNT(D1_B) = D1_E - vc;         /* stops after starting the FIFO */
		NDMA_WCNT(D1_B) = 1;
		NDMA_BCNT(D1_B) = 0;
		NDMA_CNT(D1_B) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX | NDMA_PHYS(0);
	}
	d1_armed = 1;
	d1_shown++;
	REG_IME = ime;
}

__attribute__((target("arm"), noinline)) static void raw_isr(void) {
	const u32 f = REG_IE & REG_IF & RAW_BITS;
	if (f & IRQ_NDMA(2)) {
		REG_IF = IRQ_NDMA(2);
		if (test == T_H2A || test == T_H2B) {
			const u32 vc = REG_VCOUNT;
			if (h2_t33n < 4 && test == T_H2A) h2_t33[h2_t33n++] = timer_read();
			h2_irqs++;
			if (h2_last_irq_frame != frame) { h2_irq_frames++; h2_last_irq_frame = frame; }
			h2_hist[vc < 263 ? vc : 262]++;
		} else if (test == T_NEW) {
			strip_entry();
		}
	}
	if (f & IRQ_NDMA(3)) {
		REG_IF = IRQ_NDMA(3);
		if (test == T_T1) {
			if (t1_phase == 1) {
				const u32 vc = REG_VCOUNT;
				/* Show only if the game is waiting and nothing else is pending. */
				if (t1_in_wait && (vc == T1_S - 1 || vc == T1_S) && !(REG_IE & REG_IF & ~RAW_BITS)) {
					arm_fifo();
					mode_set(3);
					t1_phase = 2;
					ndma3_hblanks(N);
					t1_shown++;
				} else {
					t1_phase = 0;
					t1_skipped++;
					if (!t1_in_wait) t1_skip_wait++;
					else if (vc != T1_S - 1 && vc != T1_S) { t1_skip_line++; t1_last_vc = vc; }
					else { t1_skip_pending++; t1_last_pending = REG_IE & REG_IF & ~RAW_BITS; }
				}
			} else if (t1_phase == 2) {
				if (mode_get() == 3) mode_set(1);
				t1_phase = 0;
			}
		} else if (test == T_NEW) strip_exit_or_guard();
		else if (test == T_BOTTOM) {
			const u32 vc = REG_VCOUNT;
			if (bottom_phase == 1) {                 /* end of line 191-N: switch in */
				const int d = (int)vc - (191 - N);
				bottom_entry_hist[d < 0 ? 0 : d > 2 ? 3 : d + 1]++;
				if (!bottom_arm_at_vblank) arm_fifo();
				mode_set(3);
				bottom_entries++;
				bottom_phase = 2;
				ndma3_hblanks(N);
			} else if (bottom_phase == 2) {          /* end of line 191: switch back */
				if (mode_get() == 3) mode_set(1);
				const int d = (int)vc - 191;
				bottom_exit_hist[d < 0 ? 0 : d > 2 ? 3 : d + 1]++;
				bottom_exits++;
				bottom_phase = 0;
			}
		}
	}
	if (f & IRQ_TIMER2) {                    /* P1: the game's own IRQ, landing anywhere */
		REG_IF = IRQ_TIMER2;
		TIMER2_CR = 0;
		if (test == T_P) {
			p_irqs++;
			if (mode_get() == 3) p_saw3++;
			p_marker ^= 1;
			DISPCNT_A = (DISPCNT_A & ~BIT(22)) | (p_marker << 22);
			p_written = p_marker;
		}
	}
	if (f & IRQ_VCOUNT) {                    /* D1: a "game" IRQ in the strip: abort first */
		REG_IF = IRQ_VCOUNT;
		if (test == T_D1 && d1_armed) { d1_abort(); d1_aborts++; }
	}
	if (f & IRQ_VBLANK) {
		REG_IF = IRQ_VBLANK;
		if (test == T_D1) {
			if (mode_get() == 3) { d1_bad_vblank++; mode_set(1); }
			d1_armed = 0;
			d1_frames++;
		}
		if (test == T_P) {
			if (mode_get() == 3) { p_bad_vblank++; mode_set(1); }
			if (p_written != 2 && ((DISPCNT_A >> 22) & 1) != p_written) p_lost++;
			p_written = 2;
			p_frames++;
		}
		frame++;
		scroll++;
		REG_BG0HOFS = scroll;
		REG_BG0VOFS = scroll / 2;
		if (mode_get() == 3) mode_set(1);       /* never carry mode 3 into a frame we didn't arm */
		switch (test) {
		case T_H1: case T_STRESS_NDMA: case T_STRESS_DMA3:
			if (armed_old && !(NDMA_CNT(0) & NDMA_ON)) fifo_done++;
			arm_fifo();
			mode_set(3);
			armed_old++;
			break;
		case T_H2A:
			timer_start();
			ndma2_oneshot(h2_ticks);
			break;
		case T_NEW:
			if (new_phase) new_phase = 0;           /* a frame whose exit never came */
			if (!use_vblank_start) ndma2_oneshot(h2_ticks);
			break;
		case T_BOTTOM:
			if (bottom_every_other && (frame & 1)) { bottom_phase = 0; break; }
			if (bottom_arm_at_vblank) arm_fifo();
			bottom_phase = 1;
			ndma3_hblanks(192 - N);                  /* H-Blanks from line 0 */
			break;
		case T_T1:
			timer_start();                          /* the game's frame clock: ticks since VBlank */
			t1_phase = 0;
			break;
		case T_COUNTER:
			ndma3_hblanks(150);                      /* runs over lines 0..149, IRQ ignored */
			break;
		}
	}
	INTR_WAIT_FLAGS |= f;
	/* Everything else (the ARM7's FIFO messages: SD, keys) stays libnds's. */
	if (REG_IE & REG_IF & ~RAW_BITS) saved_handler();
}

static void raw_begin(int t) {
	REG_IME = 0;
	saved_handler = IRQ_HANDLER;
	saved_ie = REG_IE;
	test = t;
	IRQ_HANDLER = raw_isr;
	REG_IF = RAW_BITS;
	REG_IE = (saved_ie & ~IRQ_VCOUNT & ~IRQ_HBLANK) | RAW_BITS;
	REG_DISPSTAT = DISP_VBLANK_IRQ;
	REG_IME = 1;
}

static void raw_end(void) {
	REG_IME = 0;
	for (int i = 0; i < 4; i++) NDMA_CNT(i) = 0;
	mode_set(1);
	test = T_NONE;
	IRQ_HANDLER = saved_handler;
	REG_IF = RAW_BITS;
	REG_IE = saved_ie;
	REG_IME = 1;
}

static void wait_frames(u32 n) {
	const u32 end = frame + n;
	while ((s32)(frame - end) < 0) {}
}

/* ---------------------------------------------------------------- tests */

static void title(const char* s) {
	consoleClear();
	say("== %s", s);
}

/* H4: VRAMCNT readback. Runs before anything is set up. */
static void test_h4(void) {
	title("H4 VRAMCNT readback");
	static const u32 regs[9] = { 0x04000240, 0x04000241, 0x04000242, 0x04000243, 0x04000244,
	                             0x04000245, 0x04000246, 0x04000248, 0x04000249 };
	int readable = 0;
	for (int i = 0; i < 9; i++) {
		const u8 orig = REG8(regs[i]);
		REG8(regs[i]) = 0x80;                  /* enabled, LCDC */
		const u8 a = REG8(regs[i]);
		REG8(regs[i]) = 0x00;
		const u8 b = REG8(regs[i]);
		REG8(regs[i]) = orig;
		if (a == 0x80 && b == 0x00) readable++;
		say("H4 bank %c orig %02x read %02x/%02x", "ABCDEFGHI"[i], orig, a, b);
	}
	say("H4 vramcnt readable=%d/9", readable);
}

/* R5: SCFG_EXT9 bit 8 and IE bits 28-31. */
static void test_r5(void) {
	title("R5 SCFG_EXT9 bit 8");
	const u32 ext = SCFG_EXT9;
	say("R5 SCFG_EXT9=%08lx bit8=%d scfg_access=%d", ext, (int)(ext >> 8 & 1), (int)(ext >> 31));
	const int ime = REG_IME;
	REG_IME = 0;
	const u32 ie = REG_IE;
	REG_IE = ie | 0xF0000000;
	say("R5 IE 28-31 with bit8 as is: %lx", (REG_IE >> 28) & 0xF);
	if (ext >> 31) {
		SCFG_EXT9 = ext & ~BIT(8);
		REG_IE = ie | 0xF0000000;
		say("R5 bit8 clear -> IE 28-31 %lx", (REG_IE >> 28) & 0xF);
		SCFG_EXT9 = ext | BIT(8);
		REG_IE = ie | 0xF0000000;
		say("R5 bit8 set   -> IE 28-31 %lx", (REG_IE >> 28) & 0xF);
		SCFG_EXT9 = ext;
	} else {
		say("R5 SCFG locked: can't toggle bit 8 here");
	}
	REG_IE = ie;
	REG_IME = ime;
}

/* H1: does 09h feed the FIFO (the old arming, main-loop switch back). */
static void strip_main_loop_frames(u32 frames, int stress) {
	static u32 copybuf[2][1024] __attribute__((aligned(32)));
	const u32 end = frame + frames;
	while ((s32)(frame - end) < 0) {
		if (mode_get() == 3 && REG_VCOUNT >= N - 1 && REG_VCOUNT < 192) mode_set(1);
		if (stress == 1 && !(NDMA_CNT(1) & NDMA_ON)) {      /* card-like: 128-word blocks */
			NDMA_SAD(1) = (u32)copybuf[0];
			NDMA_DAD(1) = (u32)copybuf[1];
			NDMA_WCNT(1) = 1024;
			NDMA_BCNT(1) = 0;
			NDMA_CNT(1) = NDMA_ON | NDMA_START(0x10) | NDMA_PHYS(7);
		}
		if (stress == 2 && !(DMA_CR(3) & DMA_BUSY)) {
			DMA_SRC(3) = (u32)copybuf[0];
			DMA_DEST(3) = (u32)copybuf[1];
			DMA_CR(3) = DMA_ENABLE | DMA_32_BIT | 1024;
		}
	}
	NDMA_CNT(1) = 0;
}

static void test_h1(void) {
	title("H1 NDMA 09h feeds the FIFO (10 s)");
	say("H1 look: banner on lines 0-%d, game below", N - 1);
	fifo_done = armed_old = 0;
	raw_begin(T_H1);
	strip_main_loop_frames(600, 0);
	raw_end();
	say("H1 fifo_ndma_done=%lu/%lu", fifo_done, armed_old ? armed_old - 1 : 0);
}

/* H2a / H2b: NDMA as a private timer. */
static void h2_reset(void) {
	h2_irqs = h2_irq_frames = 0;
	h2_last_irq_frame = ~0u;
	h2_t33n = 0;
	memset((void*)h2_hist, 0, sizeof h2_hist);
}

static void h2_report(const char* name, u32 frames, u32 target_line) {
	u32 lo = 999, hi = 0, within = 0;
	for (int l = 0; l < 263; l++) {
		if (!h2_hist[l]) continue;
		if ((u32)l < lo) lo = l;
		if ((u32)l > hi) hi = l;
		const int d = l - (int)target_line;
		if (d >= -1 && d <= 1) within += h2_hist[l];
	}
	say("%s irqs=%lu frames_with_irq=%lu/%lu line %lu..%lu target %lu within1=%lu",
	    name, h2_irqs, h2_irq_frames, frames, lo == 999 ? 0 : lo, hi, target_line, within);
}

static void test_h2(void) {
	title("H2 NDMA as a private timer");
	if (!((SCFG_EXT9 >> 8) & 1)) say("H2 warning: SCFG_EXT9 bit 8 clear, NDMA IRQs may not fire");
	static const u32 lines[3] = { 20, 40, 69 };
	for (int i = 0; i < 3; i++) {
		h2_reset();
		h2_ticks = lines[i] * LINE_TICKS_8M;
		const u32 f0 = frame;
		raw_begin(T_H2A);
		wait_frames(1000);
		raw_end();
		char name[32];
		snprintf(name, sizeof name, "H2a delay %lu lines", lines[i]);
		/* VBlank IRQ enters at line 192 (+ its own latency); target = 192 + lines */
		h2_report(name, frame - f0, 192 + lines[i]);
		for (u32 k = 0; k < h2_t33n; k++)
			say("H2a   precise %lu: %lu ticks@33M, expected %lu", k, h2_t33[k], h2_ticks * 4);
	}
	h2_reset();
	h2_ticks = 69 * LINE_TICKS_8M;
	const u32 f0 = frame;
	raw_begin(T_H2B);
	ndma2_vblank(h2_ticks);
	wait_frames(1000);
	raw_end();
	h2_report("H2b vblank-start forever", frame - f0, 261);
}

/* H3 + H8 + H6b: the new timing. */
static void new_reset(void) {
	entries = late_entries = exits = guard_fixes = byte_clobber = 0;
	memset((void*)exit_late_hist, 0, sizeof exit_late_hist);
	new_phase = 0;
}

static void new_report(const char* name) {
	say("%s entries=%lu late_entry=%lu exits=%lu guard_fixes=%lu byte_clobber=%lu",
	    name, entries, late_entries, exits, guard_fixes, byte_clobber);
	char b[160];
	int o = snprintf(b, sizeof b, "%s exit lateness (lines 0..15+):", name);
	for (int i = 0; i < 16 && o < (int)sizeof b - 8; i++) o += snprintf(b + o, sizeof b - o, " %lu", exit_late_hist[i]);
	say("%s", b);
}

static void test_h3(void) {
	title("H3/H8 byte switch, entry at 261 (10 s)");
	say("H3 look: clean edge at line %d, banner from line 0", N);
	new_reset();
	h2_ticks = 69 * LINE_TICKS_8M;
	raw_begin(T_NEW);
	if (use_vblank_start) ndma2_vblank(h2_ticks);
	wait_frames(600);
	raw_end();
	new_report("H3");
}

static void test_h6b(void) {
	title("H6b exit latency, game masks IRQs (167 s)");
	new_reset();
	h2_ticks = 69 * LINE_TICKS_8M;
	raw_begin(T_NEW);
	if (use_vblank_start) ndma2_vblank(h2_ticks);
	u32 seed = 12345;
	const u32 end = frame + 10000;
	while ((s32)(frame - end) < 0) {
		/* the "game": every so often, IRQs off for 0-2 lines */
		seed = seed * 1103515245u + 12345u;
		const u32 r = seed >> 16;
		if ((r & 7) == 0) {
			const u32 lines = (r >> 3) % 3;
			REG_IME = 0;
			u32 vc = REG_VCOUNT, seen = 0;
			while (seen < lines) { const u32 v = REG_VCOUNT; if (v != vc) { vc = v; seen++; } }
			REG_IME = 1;
		}
		for (volatile int i = (int)((r >> 6) & 1023); i > 0; i--) {}
	}
	raw_end();
	new_report("H6b");
}

/* H10: a bottom-edge strip. The zero-lag design shows notices only while
 * the game is already waiting for VBlank, which is the end of the frame. */
static void test_h10(void) {
	for (int v = 1; v >= 0; v--) {
		title(v ? "H10a bottom edge, FIFO armed at VBlank (10 s)" : "H10b bottom edge, FIFO armed at entry (10 s)");
		say("H10 look: banner on lines %d-191, game above", 192 - N);
		bottom_arm_at_vblank = v;
		bottom_phase = 0;
		bottom_entries = bottom_exits = 0;
		memset((void*)bottom_entry_hist, 0, sizeof bottom_entry_hist);
		memset((void*)bottom_exit_hist, 0, sizeof bottom_exit_hist);
		raw_begin(T_BOTTOM);
		wait_frames(600);
		raw_end();
		say("H10%c entries=%lu exits=%lu entry line early/ok/+1/late %lu/%lu/%lu/%lu exit %lu/%lu/%lu/%lu",
		    v ? 'a' : 'b', bottom_entries, bottom_exits,
		    bottom_entry_hist[0], bottom_entry_hist[1], bottom_entry_hist[2], bottom_entry_hist[3],
		    bottom_exit_hist[0], bottom_exit_hist[1], bottom_exit_hist[2], bottom_exit_hist[3]);
	}
}

/* ---- T1: the slack probe (docs/display-library.md 11.4) ----
 * A stand-in game alternates a light frame (done early, so the strip may
 * show at the bottom while it waits) and a heavy frame whose fixed amount of
 * work is sized to end k ticks (33.5 MHz) before VBlank. Any strip cost that
 * leaks into working time (cache pollution from the strip's IRQs, the
 * dispatcher, the bus) makes heavy frames overrun. X = the smallest k with
 * no more overruns with the strip than without. */
#define FRAME_TICKS 560190u                 /* 263 lines x 2130 */
static volatile u32 workbuf[4096] __attribute__((aligned(32)));   /* 16 KB: more than the 4 KB D-cache */

static u32 work(u32 iters) {
	u32 acc = 0, i = 0;
	while (iters--) {
		acc += workbuf[i] ^ (acc >> 3);
		i = (i + 8) & 4095;                     /* a new cache line every iteration */
	}
	return acc;
}

static volatile u32 sink;

__attribute__((target("arm"), noinline)) static void t1_wait_vblank(void) {
	const u32 f0 = frame;
	t1_in_wait = 1;
	const u32 vc = REG_VCOUNT;
	if (t1_strip && vc < T1_S - 2) {            /* arm at wait entry, from idle */
		t1_phase = 1;
		ndma3_hblanks(T1_S - vc);
	}
	while (frame == f0) asm volatile("mcr p15, 0, %0, c7, c0, 4" :: "r"(0));   /* halt, like OS_Halt */
	t1_in_wait = 0;
}

static void test_t1(void) {
	title("T1 slack probe (about 6 min)");
	ndma3_to_io = 1;
	raw_begin(T_T1);
	for (int i = 0; i < 4096; i++) workbuf[i] = (u32)i * 2654435761u;
	/* Calibrate: iterations per tick, strip off, in the same light/heavy rhythm. */
	t1_strip = 0;
	u32 rate_x1000 = 0;
	for (int i = 0; i < 60; i++) {
		t1_wait_vblank();
		const u32 t0 = timer_read();
		sink = work(20000);
		const u32 dt = timer_read() - t0;
		if (i >= 30) rate_x1000 += 20000u * 1000u / (dt ? dt : 1) / 30;
		t1_wait_vblank();
	}
	say("T1 rate %lu.%03lu iterations/tick", rate_x1000 / 1000, rate_x1000 % 1000);
	static const u32 ks[] = { 20000, 8000, 3000, 1500, 800, 400, 200, 100 };
	for (unsigned ki = 0; ki < sizeof ks / sizeof ks[0]; ki++) {
		u32 lag[2] = { 0, 0 }, shown = 0;
		for (int on = 0; on < 2; on++) {
			t1_strip = on;
			t1_shown = t1_skipped = 0;
			for (int i = 0; i < 600; i++) {
				t1_wait_vblank();                     /* light frame: a little work, then wait */
				sink = work(2000);
				t1_wait_vblank();                     /* heavy frame starts here */
				const u32 f0 = frame;
				const u32 start = timer_read();
				const u32 budget = FRAME_TICKS > start + ks[ki] ? FRAME_TICKS - start - ks[ki] : 0;
				sink = work((u32)((u64)budget * rate_x1000 / 1000));
				if (frame != f0) lag[on]++;           /* VBlank came before the work was done */
			}
			if (on) {
				shown = t1_shown;
				if (ki == 0) say("T1 skips: not waiting=%lu line=%lu (last vc %lu) pending=%lu (last %08lx)",
				                 t1_skip_wait, t1_skip_line, t1_last_vc, t1_skip_pending, t1_last_pending);
			}
		}
		say("T1 k=%lu ticks (%lu cycles): overruns off=%lu on=%lu strip shown=%lu/600",
		    ks[ki], ks[ki] * 2, lag[0], lag[1], shown);
	}
	raw_end();
	ndma3_to_io = 0;
	say("T1 X = smallest k where on == off (owner decides if it counts as zero)");
}

/* ---- D1 and NWRAM ---- */
static u16* nwram_banner(void) {
	/* A WRAM-A/B/C window mapped to the ARM9 (MBK6-8), if this boot left one. */
	const u32 mbk6 = REG32(0x04004054), mbk7 = REG32(0x04004058), mbk8 = REG32(0x0400405C);
	say("NW MBK1-5 %08lx %08lx %08lx %08lx %08lx MBK6-8 %08lx %08lx %08lx",
	    REG32(0x04004040), REG32(0x04004044), REG32(0x04004048), REG32(0x0400404C), REG32(0x04004050), mbk6, mbk7, mbk8);
	const u32 c_start = 0x03000000 + ((mbk8 >> 3) & 0x1FF) * 0x8000;
	const u32 c_end = 0x03000000 + ((mbk8 >> 19) & 0x3FF) * 0x8000;
	const u32 b_start = 0x03000000 + ((mbk7 >> 3) & 0x1FF) * 0x8000;
	const u32 b_end = 0x03000000 + ((mbk7 >> 19) & 0x3FF) * 0x8000;
	const u32 need = sizeof banner;
	u32 at = 0;
	/* a slot of WRAM-C, then B, mastered by the ARM9 (master bits 0-1 = 0) and enabled */
	for (int k = 0; k < 8 && !at; k++) {
		const u8 slot = *(vu8*)(0x0400404C + k);
		if ((slot & 0x80) && (slot & 3) == 0 && c_end > c_start + need) at = c_start;
	}
	for (int k = 0; k < 8 && !at; k++) {
		const u8 slot = *(vu8*)(0x04004044 + k);
		if ((slot & 0x80) && (slot & 3) == 0 && b_end > b_start + need) at = b_start;
	}
	if (!at) { say("NW no ARM9-mapped WRAM-B/C window: NWRAM test skipped"); return 0; }
	u16* dst = (u16*)at;
	for (u32 i = 0; i < 256 * (N + M); i++) dst[i] = banner[i];
	if (dst[1000] != banner[1000]) { say("NW window at %08lx not writable: skipped", at); return 0; }
	say("NW banner copied to %08lx", at);
	return dst;
}

static void d1_run(const char* name, const u16* src, int abort_every, int frames, int no_starter) {
	title(name);
	say("%s look: banner on lines %d-190, game above%s", name, D1_E,
	    abort_every ? "; every 4th frame cut short at line 169" : "");
	d1_src = src;
	d1_no_starter = no_starter;
	d1_abort_every = abort_every;
	d1_frames = d1_shown = d1_bad_vblank = d1_aborts = 0;
	raw_begin(T_D1);
	REG_DISPSTAT = DISP_VBLANK_IRQ | ((169 & 0xFF) << 8) | ((169 & 0x100) >> 1);
	{ const u32 f0 = frame; while (frame == f0) {} }   /* start at a frame boundary */
	for (int i = 0; i < frames; i++) {
		const int cut = abort_every && (frame % 4) == 0;
		REG_DISPSTAT = cut ? (REG_DISPSTAT | DISP_YTRIGGER_IRQ) : (REG_DISPSTAT & ~DISP_YTRIGGER_IRQ);
		while (REG_VCOUNT != D1_ARM) {}          /* its frame work ends at line 100 */
		d1_arm();
		const u32 f1 = frame;
		while (frame == f1) asm volatile("" ::: "memory");   /* waits for VBlank: the next frame starts */
	}
	REG_DISPSTAT = DISP_VBLANK_IRQ;
	raw_end();
	say("%s frames=%lu armed=%lu bad-at-vblank=%lu aborts=%lu (pass: bad 0)", name, d1_frames, d1_shown, d1_bad_vblank, d1_aborts);
}

static void test_d1(void) {
	d1_run("D1 all-NDMA strip, main RAM (10 s)", banner, 0, 600, 0);
	d1_run("D1b all-NDMA strip, abort on a game IRQ (10 s)", banner, 1, 600, 0);
	d1_run("D1c all-NDMA strip, no starter channel (10 s)", banner, 0, 600, 1);
	u16* nw = nwram_banner();
	if (nw) d1_run("D1n all-NDMA strip, FIFO from NWRAM (10 s)", nw, 0, 600, 0);
}

/* ---- P1: the production setup ----
 * As the module would run it in a game booted by nds-bootstrap's NTR path:
 *  - SCFG_EXT9 and MBK2-8 as that boot leaves them (main.arm9.c:576-583,
 *    initMBKARM9), SCFG then locked;
 *  - strip and DISPCNT table in NWRAM at 0x03760000, no starter channel (D1c);
 *  - the abort in a wrapper put on the BIOS IRQ vector (DTCM+0x3FFC) at
 *    arming, which puts the original back on its first entry, stops only our
 *    own channels (recognised by destination), with a bounded wait;
 *  - arming skipped when an IRQ is pending, or undone when a line passed.
 * A "game" timer IRQ lands at swept points across the strip's lines and their
 * H-Blank edges on 3 frames of 4. Runs last: the lock stays until power-off. */
#define P_NW  0x03760000u
#define P_TAB (P_NW + 256u * (N + M) * 2u)  /* the table right after the banner */
static VoidFn p_orig;
static volatile u32 p_aborts, p_armed, p_skipped_pending, p_undone;

static inline bool p_ours(void) {
	return ((NDMA_CNT(D1_A) & NDMA_ON) && NDMA_DAD(D1_A) == 0x04000000u)
	    || ((NDMA_CNT(D1_F) & NDMA_ON) && NDMA_DAD(D1_F) == MMEM_FIFO);
}

static inline void p_stop(void) {
	NDMA_CNT(D1_A) = 0;                      /* the DISPCNT writer first */
	NDMA_CNT(D1_F) = 0;
	for (int i = 0; i < 64 && (NDMA_CNT(D1_A) & NDMA_ON); i++) {}
	if (mode_get() == 3) mode_set(1);
}

__attribute__((target("arm"), noinline)) static void p_wrapper(void) {
	IRQ_HANDLER = p_orig;                    /* off the vector again before anything else */
	if (p_ours()) {
		p_stop();
		p_aborts++;
	}
	p_orig();
}

static void p_arm(int fire) {
	const int ime = REG_IME;
	REG_IME = 0;
	if (REG_IE & REG_IF) {                   /* an IRQ is due: it would cut the strip at once */
		p_skipped_pending++;
		REG_IME = ime;
		return;
	}
	const u32 vc = REG_VCOUNT;
	const u32 d = (DISPCNT_A & ~0x30000u) | 0x10000u;
	const u32 n = 191 - vc;
	u32* tab = (u32*)P_TAB;
	for (u32 i = 0; i < n; i++) {
		const u32 line = vc + i + 1;
		tab[i] = (line >= D1_E && line <= 190) ? (d | 0x30000u) : d;
	}
	DC_FlushRange(tab, n * 4);
	NDMA_CNT(D1_F) = 0;
	NDMA_SAD(D1_F) = P_NW;
	NDMA_DAD(D1_F) = MMEM_FIFO;
	NDMA_TCNT(D1_F) = (N + M) * 128;
	NDMA_WCNT(D1_F) = 4;
	NDMA_BCNT(D1_F) = 0;
	NDMA_CNT(D1_A) = 0;
	NDMA_SAD(D1_A) = (u32)tab;
	NDMA_DAD(D1_A) = 0x04000000;
	NDMA_TCNT(D1_A) = n;
	NDMA_WCNT(D1_A) = 1;
	NDMA_BCNT(D1_A) = 0;
	NDMA_CNT(D1_A) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX | NDMA_PHYS(0);
	NDMA_CNT(D1_F) = d1_fifo_cnt();
	if (REG_VCOUNT != vc) {                  /* a line went by: the table is off by one */
		p_stop();
		p_undone++;
		REG_IME = ime;
		return;
	}
	p_orig = IRQ_HANDLER;
	IRQ_HANDLER = p_wrapper;
	if (fire) {
		/* from line E-1 to the start of VBlank, in 397-cycle steps, so the IRQ
		 * meets every point of a line (2130 cycles) over the run */
		const u32 span = (191 - D1_E + 2) * 2130;
		const u32 cycles = (D1_E - 1 - vc) * 2130 + (p_frames * 397) % span;
		TIMER2_CR = 0;
		TIMER2_DATA = (u16)(65536 - (cycles >> 6));
		TIMER2_CR = TIMER_ENABLE | TIMER_IRQ_REQ | TIMER_DIV_64;
	}
	p_armed++;
	REG_IME = ime;
}

static void p_run(const char* name, int fire_every4, int frames) {
	title(name);
	say("%s look: banner on lines %d-190, game above", name, D1_E);
	p_irqs = p_saw3 = p_lost = p_bad_vblank = p_frames = 0;
	p_aborts = p_armed = p_skipped_pending = p_undone = 0;
	p_written = 2;
	raw_begin(T_P);
	{ const u32 f0 = frame; while (frame == f0) {} }
	for (int i = 0; i < frames; i++) {
		while (REG_VCOUNT != D1_ARM) {}
		p_arm(fire_every4 && (frame % 4) != 0);
		const u32 f1 = frame;
		while (frame == f1) asm volatile("" ::: "memory");
	}
	TIMER2_CR = 0;
	raw_end();
	say("%s frames=%lu armed=%lu skipped(pending)=%lu undone=%lu game-irqs=%lu aborts=%lu",
	    name, p_frames, p_armed, p_skipped_pending, p_undone, p_irqs, p_aborts);
	say("%s game saw mode 3=%lu lost DISPCNT writes=%lu mode 3 at VBlank=%lu (pass: all 0)",
	    name, p_saw3, p_lost, p_bad_vblank);
}

static void test_p(void) {
	title("P1 production setup (about 30 s)");
	say("P1 before: SCFG_EXT9=%08lx MBK1-5 %08lx %08lx %08lx %08lx %08lx MBK6-8 %08lx %08lx %08lx", SCFG_EXT9,
	    REG32(0x04004040), REG32(0x04004044), REG32(0x04004048), REG32(0x0400404C), REG32(0x04004050),
	    REG32(0x04004054), REG32(0x04004058), REG32(0x0400405C));
	if (!(SCFG_EXT9 & BIT(31))) {
		say("P1 SCFG already locked (run P1 right after power-on): skipped");
		return;
	}
	/* nds-bootstrap's NTR game boot: initMBKARM9, every B/C slot to the ARM9, then SCFG */
	REG32(0x04004044) = 0x9094989C;
	REG32(0x04004048) = 0x8084888C;
	REG32(0x0400404C) = 0x9094989C;
	REG32(0x04004050) = 0x8084888C;
	REG32(0x04004054) = 0x00000000;
	REG32(0x04004058) = 0x07C03740;
	REG32(0x0400405C) = 0x07403700;
	SCFG_EXT9 = 0x8300C000u | BIT(16);
	SCFG_EXT9 &= ~BIT(31);
	say("P1 after lock: SCFG_EXT9=%08lx MBK1-5 %08lx %08lx %08lx %08lx %08lx MBK6-8 %08lx %08lx %08lx", SCFG_EXT9,
	    REG32(0x04004040), REG32(0x04004044), REG32(0x04004048), REG32(0x0400404C), REG32(0x04004050),
	    REG32(0x04004054), REG32(0x04004058), REG32(0x0400405C));
	/* separate memory at each address the module would use, or aliases? */
	vu32* const w0 = (vu32*)0x03700000;
	vu32* const w1 = (vu32*)0x03740000;
	vu32* const w2 = (vu32*)0x03748000;
	vu32* const w3 = (vu32*)P_NW;
	*w0 = 0x11111111; *w1 = 0x22222222; *w2 = 0x33333333; *w3 = 0x44444444;
	DC_FlushAll();
	DC_InvalidateAll();
	say("P1 alias 03700000=%08lx 03740000=%08lx 03748000=%08lx 03760000=%08lx (pass: 1..., 2..., 3..., 4...)",
	    *w0, *w1, *w2, *w3);
	u16* const dst = (u16*)P_NW;
	for (u32 i = 0; i < 256 * (N + M); i++) dst[i] = banner[i];
	DC_FlushRange(dst, 256 * (N + M) * 2);
	if (dst[1000] != banner[1000]) {
		say("P1 NWRAM at %08lx not writable: skipped", P_NW);
		return;
	}
	flush_log();
	p_run("P1a strip only, no game IRQ (10 s)", 0, 600);
	flush_log();
	p_run("P1b game IRQs swept across the strip (20 s)", 1, 1200);
}

static void hold(int frames);

/* ---- C1: D-cache clean by address (rnd's ARM7-route question) ---- */
static u32 c1buf[4096] __attribute__((aligned(32)));    /* 16 KB */

__attribute__((target("arm"), noinline)) static u32 c1_clean(u32 start, u32 lines) {
	timer_start();
	for (u32 i = 0; i < lines; i++) asm volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(start + i * 32));
	asm volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0));   /* drain the write buffer */
	return timer_read();
}

static void test_c1(void) {
	title("C1 D-cache clean by address: cost");
	const int ime = REG_IME;
	REG_IME = 0;
	/* (1) 4 KB, all lines dirty in the cache */
	for (int i = 0; i < 1024; i++) c1buf[i] = (u32)i;
	const u32 dirty = c1_clean((u32)c1buf, 128);
	/* (2) the same 4 KB again: present but now clean */
	const u32 clean = c1_clean((u32)c1buf, 128);
	/* (3) 4 KB not in the cache: write 16 KB, then clean the first 4 KB */
	for (int i = 0; i < 4096; i++) c1buf[i] = (u32)i ^ 0x5A5A5A5A;
	const u32 absent = c1_clean((u32)c1buf, 128);
	REG_IME = ime;
	/* timer ticks are 33.5 MHz; ARM9 cycles are twice that */
	say("C1 per 4KB (128 lines), ARM9 cycles: dirty=%lu clean=%lu not-cached=%lu", dirty * 2, clean * 2, absent * 2);
	say("C1 per line: dirty=%lu.%02lu clean=%lu.%02lu not-cached=%lu.%02lu",
	    dirty * 2 / 128, dirty * 200 / 128 % 100, clean * 2 / 128, clean * 200 / 128 % 100, absent * 2 / 128, absent * 200 / 128 % 100);
}

/* ---- A7: ARM7 latency and sound while the ARM9 runs NDMA ---- */
static void a7_start(void) { fifoSendValue32(FIFO_USER_02, 1); }
static void a7_stop(const char* phase) {
	fifoSendValue32(FIFO_USER_02, 2);
	u32 v[3];
	for (int i = 0; i < 3; i++) {
		while (!fifoCheckValue32(FIFO_USER_02)) swiWaitForVBlank();
		v[i] = fifoGetValue32(FIFO_USER_02);
	}
	/* timer ticks at 33.514 MHz -> tenths of a microsecond */
	say("A7 %s: IRQ lateness max %lu.%lu us avg %lu.%lu us over %lu ticks", phase,
	    v[0] * 10 / 335 / 10, v[0] * 10 / 335 % 10, v[1] * 10 / 335 / 10, v[1] * 10 / 335 % 10, v[2]);
}

static void test_a7(void) {
	title("A7 ARM7 latency + tone under ARM9 NDMA (20 s)");
	say("A7 listen: a steady 440 Hz tone; any click or dropout is a fail");
	a7_start();
	hold(300);
	a7_stop("idle");
	a7_start();
	raw_begin(T_STRESS_NDMA);
	strip_main_loop_frames(300, 1);
	raw_end();
	a7_stop("NDMA1 128-word copies + FIFO strip");
	a7_start();
	d1_run("A7/D1 strip while timing", banner, 0, 300, 0);
	a7_stop("D1 strip");
	say("A7 pass: max lateness under load <= idle max + 10 us, and no audible glitch");
}

/* ---- H11 ---- */
static void test_h11(void) {
	static u32 buf[4096] __attribute__((aligned(32)));
	vu32* unc = (vu32*)memUncached(buf);

	/* (a) Stop the FIFO mid-strip: is the bus back at once? Bottom strip on even
	 * frames, stopped at line 168; reads counted on lines 169-180, strip frames
	 * vs plain frames. */
	title("H11a stop the FIFO mid-strip (10 s)");
	bottom_arm_at_vblank = 0;
	bottom_every_other = 1;
	bottom_phase = 0;
	raw_begin(T_BOTTOM);
	u64 sum[2] = { 0, 0 };
	u32 n[2] = { 0, 0 };
	for (int i = 0; i < 600; i++) {
		while (REG_VCOUNT != 192 - N + 8) {}
		const int strip = mode_get() == 3;
		if (strip) { NDMA_CNT(0) = 0; mode_set(1); }
		u32 count = 0, k = 0;
		while (REG_VCOUNT < 180) { (void)unc[k]; k = (k + 9) & 4095; count++; }
		sum[strip] += count;
		n[strip]++;
		while (REG_VCOUNT >= 180 || REG_VCOUNT < 100) {}
	}
	raw_end();
	bottom_every_other = 0;
	say("H11a reads/frame after stop: stopped-strip=%lu plain=%lu (frames %lu/%lu; pass: within 1 pct)",
	    n[1] ? (u32)(sum[1] / n[1]) : 0, n[0] ? (u32)(sum[0] / n[0]) : 0, n[1], n[0]);

	/* (b) The H-Blank counter's fill writes: RAM vs I/O vs no counter. */
	title("H11b counter writes: RAM vs I/O (15 s)");
	u32 rate[3];
	for (int v = 0; v < 3; v++) {
		ndma3_to_io = v == 2;
		raw_begin(v ? T_COUNTER : T_NONE);
		u64 total = 0;
		for (int i = 0; i < 300; i++) {
			while (REG_VCOUNT != 1) {}
			u32 count = 0, k = 0;
			while (REG_VCOUNT < 140) { (void)unc[k]; k = (k + 9) & 4095; count++; }
			total += count;
			while (REG_VCOUNT < 150) {}
		}
		raw_end();
		rate[v] = (u32)(total / 300);
	}
	ndma3_to_io = 0;
	say("H11b reads lines 1-139: no counter=%lu RAM dest=%lu I/O dest=%lu (pass: I/O == none)", rate[0], rate[1], rate[2]);
}

/* H5: bus cost, main-RAM reads during the strip lines, strip off vs on. */
static u32 bus_loop_frames(u32 frames, bool strip) {
	static u32 buf[4096] __attribute__((aligned(32)));
	vu32* unc = (vu32*)memUncached(buf);          /* uncached mirror of main RAM */
	u64 total = 0;
	fifo_done = armed_old = 0;
	raw_begin(strip ? T_H1 : T_NONE);
	for (u32 f = 0; f < frames; f++) {
		while (REG_VCOUNT != 0) {}
		u32 count = 0, i = 0;
		while (REG_VCOUNT < N + M) {
			if (strip && mode_get() == 3 && REG_VCOUNT >= N - 1) mode_set(1);
			(void)unc[i];
			i = (i + 9) & 4095;
			count++;
		}
		total += count;
		while (REG_VCOUNT < 100) {}
	}
	raw_end();
	return (u32)(total / frames);
}

static void test_h5(void) {
	title("H5 bus cost of the strip (10 s)");
	const u32 off = bus_loop_frames(300, false);
	const u32 on = bus_loop_frames(300, true);
	/* loss in the strip lines, scaled to the whole frame (263 lines) */
	const u32 strip_pct_x100 = off ? (u32)((u64)(off - (on < off ? on : off)) * 10000 / off) : 0;
	const u32 frame_pct_x100 = strip_pct_x100 * (N + M) / 263;
	say("H5 reads/strip off=%lu on=%lu", off, on);
	say("H5 loss: strip lines %lu.%02lu pct, whole frame %lu.%02lu pct (pass <= 3.50)",
	    strip_pct_x100 / 100, strip_pct_x100 % 100, frame_pct_x100 / 100, frame_pct_x100 % 100);
}

static void test_h6(void) {
	title("H6a strip + NDMA1 128-word blocks (10 s)");
	say("H6a look: corruption expected (record it)");
	fifo_done = armed_old = 0;
	raw_begin(T_STRESS_NDMA);
	strip_main_loop_frames(600, 1);
	raw_end();
	say("H6a fifo_ndma_done=%lu/%lu", fifo_done, armed_old ? armed_old - 1 : 0);

	title("H6b' strip + legacy DMA3 bursts (10 s)");
	say("H6 look: banner must stay clean");
	fifo_done = armed_old = 0;
	raw_begin(T_STRESS_DMA3);
	strip_main_loop_frames(600, 2);
	raw_end();
	say("H6-dma3 fifo_ndma_done=%lu/%lu", fifo_done, armed_old ? armed_old - 1 : 0);
}

/* H9: camera LED via the ARM7. */
static u32 led(u32 v) {
	fifoSendValue32(FIFO_USER_01, v);
	while (!fifoCheckValue32(FIFO_USER_01)) swiWaitForVBlank();
	return fifoGetValue32(FIFO_USER_01);
}

static void hold(int frames) { for (int i = 0; i < frames; i++) swiWaitForVBlank(); }

static void test_h9(void) {
	title("H9 camera LED: on 3 s, blink 5 s, off");
	const u32 t1 = led(0x3101);
	hold(180);
	const u32 t2 = led(0x3102);
	hold(300);
	const u32 t0 = led(0x3100);
	/* 33.514 MHz ticks -> microseconds */
	say("H9 i2c write us: on=%lu blink=%lu off=%lu (pass < 500)",
	    t1 * 1000 / 33514, t2 * 1000 / 33514, t0 * 1000 / 33514);

	/* L1: the power LED's colour (BPTWL 63h), the one LED that can change
	 * colour. The owner notes on video which colours are visible with the
	 * lid open, then it goes back to automatic. */
	title("L1 power LED: purple 3 s, red 3 s, blue 3 s, auto");
	const u32 p3 = led(0x6303);
	hold(180);
	const u32 p1 = led(0x6301);
	hold(180);
	const u32 p2 = led(0x6302);
	hold(180);
	const u32 p0 = led(0x6300);
	say("L1 i2c write us: purple=%lu red=%lu blue=%lu auto=%lu",
	    p3 * 1000 / 33514, p1 * 1000 / 33514, p2 * 1000 / 33514, p0 * 1000 / 33514);

	/* L2: the Wi-Fi LED (BPTWL 30h, low 2 bits only): on 3 s, off 3 s, back
	 * to blink-on-traffic. */
	title("L2 Wi-Fi LED: on 3 s, off 3 s, traffic");
	const u32 w1 = led(0x3001);
	hold(180);
	const u32 w0 = led(0x3000);
	hold(180);
	const u32 w3 = led(0x3003);
	say("L2 i2c write us: on=%lu off=%lu traffic=%lu", w1 * 1000 / 33514, w0 * 1000 / 33514, w3 * 1000 / 33514);
}

int main(void) {
	consoleDemoInit();
	if (!isDSiMode()) {
		iprintf("display-lab needs DSi mode.\nStart it from TWiLight Menu++\nin DSi mode.\n");
		for (;;) swiWaitForVBlank();
	}
	sd_ok = fatInitDefault();
	if (sd_ok) mkdir("sd:/dscheevos-lab", 0777);

	say("display-lab N=%d M=%d SCFG_EXT9=%08lx sd=%d", N, M, SCFG_EXT9, sd_ok);
	test_h4();
	test_r5();
	flush_log();

	setup_game();
	make_banner();
	iprintf("\nStart filming both screens.\nA: all tests (about 13 min)\nX: production test P1 only\n   (about 40 s)\n");
	for (;;) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_A) break;
		if (keysDown() & KEY_X) {
			test_p();
			consoleClear();
			say("DONE, log %s", sd_ok ? "written to sd:/dscheevos-lab/log.txt" : "NOT written (no SD)");
			flush_log();
			for (;;) swiWaitForVBlank();
		}
	}

	test_h1();      flush_log();
	test_h2();      flush_log();
	use_vblank_start = h2_irq_frames >= 990;   /* H2b re-fired by itself every frame */
	say("new timing uses %s", use_vblank_start ? "V-Blank startup (H2b)" : "one-shot from VBlank (H2a)");
	test_h3();      flush_log();
	test_h10();     flush_log();
	test_d1();      flush_log();
	test_h5();      flush_log();
	test_h6();      flush_log();
	test_h6b();     flush_log();
	test_h9();      flush_log();
	test_h11();     flush_log();
	test_c1();      flush_log();
	test_a7();      flush_log();

	consoleClear();
	say("Main tests done. A: run T1 slack probe (about 6 min), B: finish");
	for (;;) {
		swiWaitForVBlank();
		scanKeys();
		if (keysDown() & KEY_A) { test_t1(); flush_log(); break; }
		if (keysDown() & KEY_B) break;
	}
	test_p();       /* last: it locks SCFG until power-off */
	flush_log();

	consoleClear();
	say("DONE, log %s", sd_ok ? "written to sd:/dscheevos-lab/log.txt" : "NOT written (no SD)");
	flush_log();
	for (;;) swiWaitForVBlank();
}
