/*
 * DScheevos ARM9 module: glue between the nds-bootstrap hooks and the core.
 * See dscheevos_module.h for the ABI and memory layout.
 */
#include "dscheevos_module.h"
#include "dscheevos.h"
#include "net/net.h"
#include "heap.h"
#include "util/rc_version.h"

#include <stddef.h>

#ifndef DSCM_HEAP_UNCACHED_MAX
/* Bytes of the set reply up to which evaluation runs uncached (see the
 * policy in module_init). 160 KB: the biggest real sets measured to keep
 * every frame evaluated uncached (Pokemon Black 155 KB, Mario Kart DS 114 KB;
 * research/optimize/opt-heavy.md). Bigger sets (Platinum 304 KB, SoulSilver
 * 598 KB, TWEWY 1 MB) are not measured uncached yet: they stay cached. */
#define DSCM_HEAP_UNCACHED_MAX (160u * 1024u)
#endif
#include <string.h>

#ifdef DSCM_STREAM
#include "ds_stream_protocol.h"
/* Cheevotron streamer (src/stream.c): no evaluation, just send the watched
 * words to the phone. These replace the rcheevos paths in module_init and
 * module_idle below. */
void dscm_stream_start(const char* hash, const char* game_code, const char* title, int dtcm_known);
void dscm_stream_set_phone(uint32_t ip);
void dscm_stream_record(void* ring, uint32_t size, uint32_t seed);
int dscm_stream_net_init(void);
void dscm_stream_service(uint32_t now_ms);
void dscm_stream_frame(uint32_t frame);
void dscm_stream_status(uint32_t frame, uint32_t net_state, uint32_t idle_lines_max);
/* src/popup.c: pop-ups on the console's screen, armed in idle time */
void dscm_popup_init(void);
void dscm_popup_idle(uint32_t frame);
void dscm_popup_rearm(void);
void dscm_popup_reset(void);
extern uint8_t dscm_popup_active, dscm_popup_watch;
#endif

#define REG_VCOUNT (*(volatile uint16_t*)0x04000006)
#define LINES_PER_FRAME 263

/* Code every idle wake runs: ARM code (the rest of the module is Thumb), so
 * the CPSR and CP15 work below is inline instead of a call through a
 * Thumb-to-ARM veneer each, and right after halt_entry (.text.wake, after .text.entries in module.ld; the runner counts code between the VBlank and halt entries as the VBlank call): a wake
 * fetches a few contiguous code lines, not lines scattered over 300 KB. */
#define WAKE_CODE __attribute__((target("arm"), section(".text.wake")))
#define WAKE_INLINE static inline __attribute__((always_inline, target("arm")))
WAKE_INLINE uint32_t wake_irq_off(void)
{
  uint32_t cpsr, t;
  __asm__ volatile("mrs %0, cpsr\n\torr %1, %0, #0x80\n\tmsr cpsr_c, %1" : "=r"(cpsr), "=r"(t) : : "memory");
  return cpsr;
}
WAKE_INLINE void wake_irq_restore(uint32_t cpsr)
{
  __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory");
}
WAKE_INLINE void wake_wfi(void)
{
  __asm__ volatile("mcr p15, 0, %0, c7, c0, 4" : : "r"(0) : "memory");
}
WAKE_INLINE uint32_t wake_swap(volatile uint32_t* p, uint32_t v)
{
  uint32_t old;
  __asm__ volatile("swp %0, %2, [%1]" : "=&r"(old) : "r"(p), "r"(v) : "memory");
  return old;
}

dscm_status_t dscm_g_status;   /* referenced by the header in start.s */
dscm_net_slot_t dscm_g_net[DSCM_NET_SLOTS];
dscm_notices_t dscm_g_notices;
/* Written through the cache and cleaned at each step (journal_sync_clean),
 * so the ARM7 can copy it from RAM. It has cache lines of its own: a cached
 * write to a neighbour can never write stale journal bytes back. */
union {
  dscm_journal_t j;
  uint8_t lines[(sizeof(dscm_journal_t) + 31) & ~31u];
} dscm_g_journal __attribute__((aligned(32)));
static void* g_net_handle[DSCM_NET_SLOTS];
static uint32_t g_net_sequence;
static dscm_boot_t g_boot;
static const char* g_session;        /* session file in memory (its header) */
static const char* g_replies;        /* its replies and CA: preload_offset[i] from here (heap_load_begin) */
static int g_initialized;

/* What every idle wake reads or writes, in one cache line: the game's idle
 * thread wakes 20-40 times a frame, and each line we touch there is a line
 * of the game's own evicted from the data cache. Most wakes need only the
 * snapshot and the halt (see module_idle). */
typedef struct wake_h_t {
  uint32_t pass_frame;           /* g_frames at the last full pass */
  uint32_t halts_idle, halts_other;  /* copied to the status once a frame */
  uint16_t requests;             /* requests queued or in flight (http_send .. poll_network) */
  volatile uint8_t reset_pending;
  uint8_t progress_stale;
  uint8_t net_busy;              /* net_busy() at the end of the last net_service() */
  uint8_t more;                  /* dsc_work_pending() at the end of the last full pass */
  uint8_t net_started;
  uint8_t dtcm_fixed;            /* the session gave the DTCM base */
  uint32_t dtcm;                 /* the game's DTCM base, once known */
  uint32_t capture_cpsr;         /* CPSR with IRQs on, from the caller of capture() */
  volatile uint32_t busy;        /* idle work running (maybe parked): dscm_lock */
} wake_h_t;
_Static_assert(sizeof(wake_h_t) == 32, "H: one cache line");
_Static_assert(offsetof(wake_h_t, reset_pending) == 14 && offsetof(wake_h_t, busy) == 28, "H: start.s H_RESET_PENDING, H_BUSY (reset_entry writes them)");
#define g_dtcm (H.dtcm)

extern uint32_t dscm_vblank_count_read(void);  /* start.s */
extern uint32_t dscm_dtcm_base(void);
extern void dscm_publish_notify(uint32_t count);
extern void dscm_publish_cues(uint32_t cues);
extern void dscm_publish_list(void* list);
extern void dscm_dc_clean(const void* addr, uint32_t len);
extern void dscm_dc_invalidate(const void* addr, uint32_t len);
extern void dscm_mpu_read(uint32_t out[10]);   /* start.s */
extern void dscm_dc_drain(void);               /* start.s */

/* Whether the game's protection unit makes addr data-cacheable. Regions
 * 0-7, the highest number wins; outside every region nothing is cached. */
__attribute__((section(".text_cold"), noinline)) static int mpu_dcacheable(uint32_t addr)
{
  uint32_t r[10];
  int i;
  dscm_mpu_read(r);
  for (i = 7; i >= 0; i--) {
    uint32_t reg = r[i], size, base;
    if (!(reg & 1))
      continue;
    size = 2u << ((reg >> 1) & 31);
    base = reg & ~(size - 1) & 0xFFFFF000u;
    if (addr - base < size)
      return (int)((r[8] >> i) & 1);
  }
  return 0;
}

/* Once, at the first idle wake (out of line: the wake reads no pool for it). */
__attribute__((section(".text_cold"), noinline)) static void alias_check(void);

/* journal_sync for a game whose mirror is cached: write the lines to RAM. */
static void journal_sync_clean(const void* addr, uint32_t len)
{
  dscm_dc_clean(addr, len);
  dscm_dc_drain();
}
extern uint32_t dscm_take(volatile uint32_t* p);
extern uint32_t dscm_lock(volatile uint32_t* p);
static uint32_t g_notices_seen;
static uint32_t g_cues_head;         /* last notice counted into the cues */
static uint32_t g_cues;
static void update_progress(void);

/* Self-timing. REG_VCOUNT is read-only and runs whatever the game does; the
 * timers are the game's, so we never touch them. Whole lines; the caller
 * keeps the work under a frame (a longer one wraps and reads short). */
static inline uint32_t lines_between(uint32_t start, uint32_t end)
{
  int32_t d = (int32_t)end - (int32_t)start;
  return (uint32_t)(d < 0 ? d + LINES_PER_FRAME : d);
}
#define lines_since(start) lines_between((start), REG_VCOUNT)

/* Frame counting and the wake's flags: two cache lines in the module header,
 * next to its code. The ARM7 bumps dscm_vblank_count at each VBlank; the idle
 * thread reads it (vblank_poll) before every snapshot, so nothing of ours runs
 * in the game's interrupts. publish_stats() copies the counters to the status.
 * idle_open says an idle pass is being timed: whoever clears it (the pass at
 * its end, or the poll that finds a VBlank came meanwhile) counts its lines,
 * so none are counted twice. */
typedef struct dscm_irq_t {
  uint32_t last_vblank;          /* the ARM7's count at the last call */
  volatile uint32_t frames;      /* VBlanks since init */
  uint8_t on;                    /* initialized and active */
  uint8_t data_region;           /* the game's MPU region holding the module's data, 0xFF until known (start.s, data_region_scan) */
  volatile uint8_t in_halt;      /* unused (was the re-entry guard; start.s GUARD_* now) */
  uint8_t alias_cached;          /* diagnostics: the game's MPU caches the 0x0C000000 mirror (SDK5) */
  volatile uint32_t idle_open;
  volatile uint32_t idle_start;
  uint32_t timed_frames;
  uint32_t irq_lines_total;
  uint16_t irq_lines_last;
  uint16_t irq_lines_max;
  /* second line: only when a pass is cut, or VBlanks were missed */
  volatile uint32_t idle_cut_lines;
  uint32_t idle_cut;
  uint32_t frames_missed;
  uint32_t reserved2[5];
} dscm_irq_t;
/* Idle lines per frame, from the idle thread, with IRQs masked: at each
 * pass's start and end it closes the frames that ended since (VBlanks
 * counted by the IRQ). Lines a pass added belong to the frame it ran in,
 * and so does a cut (the IRQ counts it at that frame's end); frames since
 * then had no pass. "Ran": a pass started or ended in the frame. */
typedef struct wake_idle_t {
  uint32_t frame;                /* the frame being added up (dscm_irq.frames then) */
  uint32_t lines;                /* its lines so far */
  uint32_t cut_seen;             /* dscm_irq.idle_cut_lines already counted */
  uint32_t ran;
  uint32_t last, max, total, none;
} wake_idle_t;
_Static_assert(sizeof(wake_idle_t) == 32, "g_idle: one cache line");

/* The wake page: what every idle wake reads and writes (dscm_irq, g_idle, H)
 * and start.s's re-entry guard, at a fixed address, the module region's last
 * 160 bytes, through the uncached 0x0C000000 mirror (the idle stack starts
 * below it, module.ld). Code reaches it from a base built from immediates
 * (wake_page()), so a wake reads no literal pool for it: a pool is a cache
 * line of ours, read again after the game evicted it, evicting one of the
 * game's, at every wake. (On SDK5 games the mirror is cached: as before, the
 * lines are then cached ones.) Zeroed by init_entry (start.s). */
typedef struct wake_page_t {
  dscm_irq_t irq;                /* 64 */
  wake_idle_t idle;              /* 32 */
  wake_h_t h;                    /* 32 */
  volatile uint32_t guard[8];    /* start.s GUARD_*: the re-entry guard */
} wake_page_t;
_Static_assert(sizeof(wake_page_t) == 160 && offsetof(wake_page_t, h) == 96 && offsetof(wake_page_t, guard) == 128, "wake page: start.s WAKE_PAGE, module.ld __stack_top");
/* 0x0CF80000 (the region's end, mirror view) in four instructions that are
 * the same in ARM and Thumb code (unified syntax: GCC switches inline asm to
 * divided and back), no literal; not volatile, so the compiler
 * computes it once per function. */
static inline __attribute__((always_inline)) uintptr_t wake_end(void)
{
  uintptr_t b;
  __asm__(".syntax unified\n\tmovs %0, #0xCF\n\tlsls %0, %0, #4\n\tadds %0, %0, #8\n\tlsls %0, %0, #16" : "=l"(b) : : "cc");
  return b;
}
_Static_assert(DSCM_BASE == 0x02D80000u && DSCM_SIZE == 0x00200000u, "wake_end(), wake_page_arm() and vblank_count() build these addresses from immediates");
#define WAKE_PAGE ((wake_page_t*)(wake_end() - sizeof(wake_page_t)))
/* Wake code (ARM) builds the base once per call in two instructions and hands
 * the page to its inline helpers (wp); everything else uses WAKE_PAGE. WP is
 * what the names below go through: WAKE_PAGE, or wp inside wake code. */
WAKE_INLINE wake_page_t* wake_page_arm(void)
{
  uintptr_t b;
  __asm__(".syntax unified\n\tmov %0, #0x0C000000\n\torr %0, %0, #0x00F80000" : "=r"(b));
  return (wake_page_t*)(b - sizeof(wake_page_t));
}
#define WP WAKE_PAGE
#define dscm_irq (WP->irq)
#define g_idle (WP->idle)
#define H (WP->h)

/* the offsets start.s uses */
_Static_assert(sizeof(dscm_irq_t) == 64, "dscm_irq: two cache lines");
_Static_assert(offsetof(dscm_irq_t, on) == 8 && offsetof(dscm_irq_t, data_region) == 9 && offsetof(dscm_irq_t, in_halt) == 10 && offsetof(dscm_irq_t, alias_cached) == 11 && offsetof(dscm_irq_t, idle_open) == 12, "dscm_irq");
_Static_assert(offsetof(dscm_irq_t, idle_start) == 16 && offsetof(dscm_irq_t, timed_frames) == 20 && offsetof(dscm_irq_t, irq_lines_total) == 24, "dscm_irq");
_Static_assert(offsetof(dscm_irq_t, irq_lines_last) == 28 && offsetof(dscm_irq_t, irq_lines_max) == 30, "dscm_irq");
_Static_assert(offsetof(dscm_irq_t, idle_cut_lines) == 32 && offsetof(dscm_irq_t, idle_cut) == 36 && offsetof(dscm_irq_t, frames_missed) == 40, "dscm_irq");
#define g_frames (dscm_irq.frames)

/* The ARM7's VBlank count, written behind the data cache. Through the
 * 0x0C000000 mirror when the game's MPU leaves it uncached (no line filled),
 * else drop the header line and read it through the cache (start.s). */
extern uint32_t dscm_vblank_count_read(void);
#undef WP
#define WP wp
WAKE_INLINE uint32_t vblank_count(wake_page_t* wp)
{
  if (dscm_irq.alias_cached == 0)   /* 0xFF until the game's MPU is read */
  {
    /* the header's mirror view, 0x0CD80000, from immediates (ARM code only:
     * vblank_count is wake code) */
    uintptr_t h;
    __asm__(".syntax unified\n\tmov %0, #0x0C000000\n\torr %0, %0, #0x00D80000" : "=r"(h));
    return *(volatile uint32_t*)(h + offsetof(dscm_header_t, vblank_count));
  }
  return dscm_vblank_count_read();
}
#undef WP
#define WP WAKE_PAGE



#undef WP
#define WP wp
WAKE_INLINE void idle_frames(wake_page_t* wp, uint32_t lines)
{
  uint32_t passed = dscm_irq.frames - g_idle.frame;
  if (passed) {
    uint32_t cut = dscm_irq.idle_cut_lines;
    uint32_t count = g_idle.lines + (cut - g_idle.cut_seen);
    g_idle.cut_seen = cut;
    g_idle.total += count;
    if (count > g_idle.max)
      g_idle.max = count;
    g_idle.last = passed > 1 ? 0 : count;
    g_idle.none += passed - 1 + !g_idle.ran;
    g_idle.frame = dscm_irq.frames;
    g_idle.lines = 0;
    g_idle.ran = 0;
  }
  g_idle.lines += lines;
  g_idle.ran = 1;
}
#undef WP
#define WP WAKE_PAGE

void module_vblank_copy(void);

/* The VBlanks since the last call, queued for the core, oldest first: the
 * first seals the snapshot taken at the last halt before it, the others are
 * frames the game never halted in (repeats). Called with IRQs masked, right
 * before anything that would replace or read that snapshot, so no capture
 * can come between a VBlank and its queueing. */
#undef WP
#define WP wp
WAKE_INLINE void vblank_poll(wake_page_t* wp)
{
  uint32_t c = vblank_count(WP), n = c - dscm_irq.last_vblank;
  if (!n)
    return;
  dscm_irq.last_vblank = c;
  dscm_irq.frames += n;
  dscm_irq.timed_frames += n;        /* frames counted (the IRQ lines stay 0: no IRQ work) */
  if (dscm_irq.idle_open) {          /* the pass was cut at the VBlank (line 192) */
    uint32_t start = dscm_irq.idle_start;
    dscm_irq.idle_open = 0;
    dscm_irq.idle_cut_lines += start < 192 ? 192 - start : 0;
    dscm_irq.idle_cut++;
  }
  do {
    if (dsc_vblank_queue())
      module_vblank_copy();          /* nothing captured yet (session start, reset) */
  } while (--n);
}
#undef WP
#define WP WAKE_PAGE

/* used by the stubs in syscalls.c */
uint32_t dscm_unix_time(void) { return g_boot.unix_time + g_frames / 60; }

/* The same clock frames_ago VBlanks ago: a journal entry gets the time its
 * frame was captured (the core counts VBlanks as g_frames does). */
static uint32_t unix_time_at(uint32_t frames_ago)
{
  uint32_t f = g_frames;
  return g_boot.unix_time + (f - (frames_ago < f ? frames_ago : f)) / 60;
}

/* ---- platform for the core ---- */

static uint32_t read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes)
{
  if (address < 0x400000u && address + num_bytes <= 0x400000u) {
    memcpy(buffer, (const void*)(0x02000000u + address), num_bytes);
    return num_bytes;
  }
  if (address >= 0x1000000u && address + num_bytes <= 0x1004000u && g_dtcm) {
    memcpy(buffer, (const void*)(g_dtcm + (address - 0x1000000u)), num_bytes);
    return num_bytes;
  }
  return 0;
}

#ifdef DSCM_STREAM
/* The streamer's read for stream.c: the same regions as read_memory. Returns
 * the byte count, or 0 if the range is unreadable (then the word is zero). */
int dscm_stream_read_bytes(uint32_t address, void* out, uint32_t len)
{
  return read_memory(address, (uint8_t*)out, len) ? (int)len : 0;
}
#endif

/* The same regions as read_memory, for the core's direct snapshot copy
 * (dsc_capture, or a copy in the VBlank IRQ), after g_dtcm is refreshed. */
/* Wake code: every capture calls it (it sat in the hot region, a code line
 * away from the rest of the wake). */
#undef WP
#define WP wp
WAKE_CODE static void memory_map(const uint8_t** main_ram, const uint8_t** dtcm)
{
  wake_page_t* const wp = wake_page_arm();
  *main_ram = (const uint8_t*)0x02000000u;
  *dtcm = g_dtcm ? (const uint8_t*)g_dtcm : NULL;
}
#undef WP
#define WP WAKE_PAGE

extern uint32_t dscm_irq_off(void);
extern void dscm_irq_restore(uint32_t cpsr);
extern void dscm_wfi(void);

/* dsc_capture copies with IRQs masked and calls this every few words. IF
 * latches requests while CPSR.I masks them: a waiting IRQ runs here at
 * once, so the game sees at most a few words' delay, and the copy goes on.
 * If two lines or more went by, a game thread probably ran (the IRQ woke
 * it): start over. The core also starts over if the IRQ was a VBlank. */
#define REG_IE (*(volatile uint32_t*)0x04000210)
#define REG_IF (*(volatile uint32_t*)0x04000214)
#define g_capture_cpsr (H.capture_cpsr)
#undef WP
#define WP wp
WAKE_CODE static int interrupt_pending(void)
{
  wake_page_t* const wp = wake_page_arm();
  uint32_t before;
  if (vblank_count(WP) != dscm_irq.last_vblank)
    return 1;          /* a VBlank: the copy would be of two frames (vblank_poll queues it next) */
  if (!(REG_IE & REG_IF))
    return 0;
  before = REG_VCOUNT;
  wake_irq_restore(g_capture_cpsr);
  wake_irq_off();
  /* a VBlank: the copy would be of two frames (vblank_poll queues it next) */
  return lines_between(before, REG_VCOUNT) >= 2 || vblank_count(WP) != dscm_irq.last_vblank;
}
#undef WP
#define WP WAKE_PAGE

/* A snapshot when the game halts. Returns with IRQs still masked, and the
 * CPSR to put back: the caller may halt first. */
/* The game sets its DTCM up before its first frame; the module starts before that. */
#undef WP
#define WP wp
WAKE_INLINE void refresh_dtcm(wake_page_t* wp)
{
  if (!H.dtcm_fixed)          /* the session gave no DTCM base: read the game's */
    g_dtcm = dscm_dtcm_base();
}
#undef WP
#define WP WAKE_PAGE

#undef WP
#define WP wp
WAKE_INLINE uint32_t capture(wake_page_t* wp)
{
  refresh_dtcm(WP);
  /* VBlanks so far, and their commits, with IRQs on: after a long stretch
   * without idle time (a load) that is many frames. The masked poll below
   * then finds at most the VBlank that came meanwhile. */
  vblank_poll(WP);
  dsc_drain();
  g_capture_cpsr = wake_irq_off();
  vblank_poll(WP);
  dsc_capture();
  return g_capture_cpsr;
}
#undef WP
#define WP WAKE_PAGE

/* g_frames * 1000 / 60, the same value mod 2^32 as the 64-bit product, in
 * 32-bit arithmetic: a 64-bit division costs ~340 instructions in libgcc,
 * and this runs several times every idle pass. */
static uint32_t millis(void)
{
  uint32_t f = g_frames, q = f / 3u;
  return q * 50u + (f - q * 3u) * 50u / 3u;
}
uint32_t dscm_millis(void) { return millis(); }

static uint32_t frame_count(void) { return g_frames; }

extern void dscm_publish_net_ring(void* ring);
extern void https_add_ca(const char* pem, uint32_t len);

static const char* request_kind(const char* post)
{
  /* "r=<kind>&..." */
  return (post && post[0] == 'r' && post[1] == '=') ? post + 2 : "";
}

static int starts_with(const char* s, const char* prefix)
{
  return strncmp(s, prefix, strlen(prefix)) == 0 && (s[strlen(prefix)] == '&' || s[strlen(prefix)] == '\0');
}

static void http_send(const dsc_http_request_t* request)
{
  const char* kind = request_kind(request->post_data);
  int preload = -1;
  int i;

  /* Session-start requests were answered by the loader before the game booted. */
  if (starts_with(kind, "login2")) preload = DSCM_PRELOAD_LOGIN;
  else if (starts_with(kind, "achievementsets")) preload = DSCM_PRELOAD_SET;
  else if (starts_with(kind, "startsession")) preload = DSCM_PRELOAD_SESSION;

  if (preload >= 0) {
    if (g_boot.preload_length[preload]) {
      uint32_t offset = g_boot.preload_offset[preload], length = g_boot.preload_length[preload];
      const char* body = g_replies + offset;
      /* With a spare NUL after it in the file, the core's copy of the reply
       * is the reply itself (heap.h): no heap for it. */
      if (offset + length < g_boot.file_size && body[length] == '\0')
        heap_lend(body, length);
      dsc_http_done(request->handle, 200, body, length);
      heap_lend(NULL, 0);
    }
    else {
      static const char missing[] = "{\"Success\":false,\"Error\":\"Not fetched at session start\"}";
      dsc_http_done(request->handle, 200, missing, sizeof(missing) - 1);
    }
    return;
  }

  /* Everything else (unlocks, leaderboards, pings) goes to the network layer. */
  for (i = 0; i < DSCM_NET_SLOTS; i++) {
    if (dscm_g_net[i].state == DSCM_NET_FREE) {
      dscm_net_slot_t* s = &dscm_g_net[i];
      if (strlen(request->url) >= DSCM_NET_URL_MAX || (request->post_data && strlen(request->post_data) >= DSCM_NET_POST_MAX))
        break;
      strcpy(s->url, request->url);
      strcpy(s->post, request->post_data ? request->post_data : "");
      s->sequence = ++g_net_sequence;
      s->http_status = 0;
      s->reply_length = 0;
      g_net_handle[i] = request->handle;
      dscm_g_status.pending_requests++;
      H.requests++;
      s->state = DSCM_NET_REQUEST;   /* publish last */
      return;
    }
  }
  /* queue full: rcheevos retries retryable errors later */
  dsc_http_done(request->handle, -2, "", 0);
}

static void poll_network(void)
{
  int i;
  for (i = 0; i < DSCM_NET_SLOTS; i++) {
    dscm_net_slot_t* s = &dscm_g_net[i];
    if (s->state == DSCM_NET_REPLIED) {
      uint32_t len = s->reply_length < DSCM_NET_REPLY_MAX ? s->reply_length : DSCM_NET_REPLY_MAX;
      if (s->http_status == 200 && !strncmp(s->post, "r=awardachievement&", 19))
        dscm_g_status.unlocks_sent++;
      dsc_http_done(g_net_handle[i], s->http_status, s->reply, len);
      g_net_handle[i] = NULL;
      dscm_g_status.pending_requests--;
      H.requests--;
      s->state = DSCM_NET_FREE;
    }
  }
}

static void copy_title(char* dst, const char* src, size_t n)
{
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

static void on_event(const dsc_event_t* e)
{
  switch (e->type) {
    case DSC_EVENT_UNLOCK: {
      dscm_unlock_t* u = &dscm_g_status.recent[dscm_g_status.unlock_count % DSCM_RECENT_MAX];
      u->id = e->id;
      u->points = e->points;
      u->frame = g_frames;
      copy_title(u->title, e->title, sizeof(u->title));
      dscm_g_status.unlock_count++;
      dscm_publish_notify(dscm_g_status.unlock_count);
      H.progress_stale = 1;
      break;
    }
    case DSC_EVENT_GAME_LOADED:
      copy_title(dscm_g_status.game_title, e->title, sizeof(dscm_g_status.game_title));
      H.progress_stale = 1;
      break;
    case DSC_EVENT_LOGIN_FAILED:
      dscm_g_status.error = DSCM_ERROR_LOGIN;
      break;
    case DSC_EVENT_GAME_LOAD_FAILED:
      dscm_g_status.error = DSCM_ERROR_GAME_LOAD;
      break;
    default:
      break;
  }
}

/* The list is complete: write it to RAM, then swap header->list. */
static void publish_list(dscm_list_t* list)
{
  if (list)
    dscm_dc_clean(list, sizeof(dscm_list_t) + list->count * sizeof(dscm_list_entry_t));
  dscm_publish_list(list);   /* the module's view: only the ARM9 menu reads it */
}

/* Keep RAM current for readers past the cache, after notices change. */
static void clean_notices(void)
{
  uint32_t seen = dscm_g_notices.head + dscm_g_notices.indicators_generation;
  if (seen != g_notices_seen) {
    g_notices_seen = seen;
    dscm_dc_clean(&dscm_g_notices, sizeof(dscm_g_notices));
  }
}

/* Turn new notices into header->cues for the ARM7's LED: a counter per
 * progress or leaderboard notice, and a bit while a challenge shows. */
static void update_cues(void)
{
  uint32_t head = dscm_g_notices.head, cues = g_cues, seq;
  int i;
  if (head - g_cues_head > DSCM_NOTICE_RING)
    g_cues_head = head - DSCM_NOTICE_RING;   /* older ones were overwritten */
  for (seq = g_cues_head + 1; seq - 1 != head; seq++) {
    const dscm_notice_t* x = &dscm_g_notices.ring[seq % DSCM_NOTICE_RING];
    if (x->seq != seq)
      continue;
    if (x->kind == DSCM_NOTICE_PROGRESS)
      cues = (cues & ~DSCM_CUE_PROGRESS_MASK) | ((cues + 1) & DSCM_CUE_PROGRESS_MASK);
    else if (x->kind == DSCM_NOTICE_LEADERBOARD_STARTED || x->kind == DSCM_NOTICE_LEADERBOARD_FAILED
             || x->kind == DSCM_NOTICE_LEADERBOARD_SUBMITTED)
      cues = (cues & ~DSCM_CUE_LEADERBOARD_MASK) | ((cues + (1u << DSCM_CUE_LEADERBOARD_SHIFT)) & DSCM_CUE_LEADERBOARD_MASK);
  }
  g_cues_head = head;
  cues &= ~DSCM_CUE_CHALLENGE;
  for (i = 0; i < DSCM_INDICATOR_MAX; i++)
    if (dscm_g_notices.challenges[i].id)
      cues |= DSCM_CUE_CHALLENGE;
  if (cues != g_cues) {
    g_cues = cues;
    dscm_publish_cues(cues);
  }
}

/* ---- entry points ---- */

extern char __bss_start[], __bss_end[];

int module_init(const dscm_boot_t* boot)
{
  dsc_platform_t platform;
  dsc_config_t config;
  int i;

  memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));
  /* from here on the journal is only written uncached: drop its cached lines */
  dscm_dc_clean(&dscm_g_journal, sizeof(dscm_g_journal));
  dscm_dc_invalidate(&dscm_g_journal, sizeof(dscm_g_journal));
  if (!boot || boot->magic != DSCM_SESSION_MAGIC || boot->size != sizeof(dscm_boot_t)
      || boot->file_size > DSCM_SESSION_MAX) {
    dscm_g_status.error = DSCM_ERROR_BAD_BOOT;
    return -1;
  }
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) {
    if (boot->preload_offset[i] > boot->file_size || boot->preload_length[i] > boot->file_size - boot->preload_offset[i]) {
      dscm_g_status.error = DSCM_ERROR_BAD_BOOT;
      return -1;
    }
  }
  g_boot = *boot;
  /* rc_client and the journal run on UTC, as the server does, when the
   * launcher could measure the console clock's offset */
  if (g_boot.flags & DSCM_BOOT_UTC_KNOWN)
    g_boot.unix_time = dscm_console_to_utc(g_boot.unix_time, g_boot.utc_offset_q);
  g_session = (const char*)DSCM_DATA_ADDR(boot);   /* the module's view of the session */
#ifdef DSCM_DATA_MIRROR
  /* Evaluation reads the set's data, its snapshots and the core's state
   * uncached in the mirror view (slower: Kirby's rcheevos +23% cycles): only
   * for sets small enough to afford it (the set reply's size as the cost
   * proxy); bigger sets keep them cached and pollute the game's D-cache
   * (cached, a heavy set's evaluation touches several times the 4 KB
   * D-cache each frame: Black lost ~45k game cycles a frame to it). */
  {
    int uncached = boot->preload_length[DSCM_PRELOAD_SET] <= DSCM_HEAP_UNCACHED_MAX;
    heap_view(uncached);
    /* the core's state the same way (zeroed above through the mirror;
     * nothing of the module was ever cached at its 0x02 address) */
    if (!uncached)
      dsc_state_view(0x02000000u);
  }
#endif
#ifdef DSCM_STREAM
  g_replies = NULL;                               /* the streamer loads no achievement set */
#else
  g_replies = heap_load_begin(boot->file_size);   /* before the first malloc */
#endif
  g_boot.host[sizeof(g_boot.host) - 1] = '\0';
  g_boot.username[sizeof(g_boot.username) - 1] = '\0';
  g_boot.token[sizeof(g_boot.token) - 1] = '\0';
  g_boot.game_hash[sizeof(g_boot.game_hash) - 1] = '\0';
  g_dtcm = g_boot.dtcm_base;
  H.dtcm_fixed = g_dtcm != 0;   /* fixed: refresh_dtcm leaves it */
  dscm_irq.last_vblank = dscm_vblank_count_read();
  dscm_irq.alias_cached = 0xFF;   /* known once the game idles (module_idle) */
  dscm_irq.data_region = 0xFF;    /* the game's MPU region for it too (start.s) */
  dscm_g_status.hardcore = g_boot.hardcore;

#ifdef DSCM_STREAM
  (void)platform;
  (void)config;
  dscm_g_status.active = 1;   /* the streamer is active whenever the boot is valid */
#else
  memset(&platform, 0, sizeof(platform));
  platform.read_memory = read_memory;
  platform.memory_map = memory_map;
  platform.interrupt_pending = interrupt_pending;
  platform.millis = millis;
  platform.http_send = http_send;
  platform.event = on_event;
  platform.unix_time = dscm_unix_time;
  platform.unix_time_at = unix_time_at;
  platform.clock_is_utc = (g_boot.flags & DSCM_BOOT_UTC_KNOWN) != 0;
  platform.clock = g_boot.clock;
  dscm_g_status.clock = g_boot.clock;
  /* Cached, cleaned at each ordering point: the 0x0C000000 mirror is cached
   * write-back on SDK5 games, so it can't be trusted to reach RAM. */
  platform.journal = &dscm_g_journal.j;
  platform.journal_sync = journal_sync_clean;
  platform.notices = &dscm_g_notices;
  platform.publish_list = publish_list;
  platform.frame = frame_count;

  memset(&config, 0, sizeof(config));
  config.host = g_boot.host;
  config.username = g_boot.username;
  config.token = g_boot.token;
  config.hardcore = g_boot.hardcore;
  config.spectator = (g_boot.flags & DSCM_BOOT_SPECTATOR) ? 1 : 0;
  config.max_addresses = 4096;
  config.snapshot_slots = 1024;          /* ~17 s of frames with no idle time (loading, save writes) */
  config.snapshot_bytes = HEAP_ARENA_BYTES;  /* shared by queued frames: a few hundred bytes each, typically */
  config.capture_at_halt = 1;            /* snapshot when the game halts, not in its VBlank interrupt */
  config.idle_max_frames = 4;            /* then the network and a fresh snapshot get their turn */

  heap_arena_window(1);                  /* the arena goes where the session's replies are */
  i = dsc_init(&platform, &config);
  heap_arena_window(0);
  if (i != 0) {
    dscm_g_status.error = DSCM_ERROR_NO_MEMORY;
    return -1;
  }
  if (dsc_begin_session(g_boot.game_hash) != 0) {
    dscm_g_status.error = DSCM_ERROR_BAD_BOOT;
    return -1;
  }
  /* session-start replies are preloaded, so this settles in a few passes */
  for (i = 0; i < 8 && !dsc_is_active() && !dscm_g_status.error; i++)
    dsc_idle();
  /* The replies and rc_api's parse of the set are in the arena's place, which
   * the first frame overwrites: the load must be over, and nothing may
   * answer from the session again (later requests get "Not fetched"). An
   * unfinished load never resumes: module_idle runs only when active. */
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++)
    g_boot.preload_length[i] = 0;
  if (heap_load_end() != 0) {   /* load-time blocks left in the arena, or an allocation failed (the set didn't fit) */
    dsc_shutdown();
    dscm_g_status.error = DSCM_ERROR_NO_MEMORY;
  }

  dscm_g_status.active = dsc_is_active();
  if (dscm_g_status.active)
    update_progress();
  clean_notices();
  update_cues();
#endif /* !DSCM_STREAM */

  /* Keep using the launcher's Wi-Fi connection during play. */
  if (dscm_g_status.active && g_boot.net_valid) {
    static char user_agent[80];
    net_config_t nc;
    void* ring;
    memset(&nc, 0, sizeof(nc));
    memcpy(nc.mac, g_boot.mac, 6);
    nc.ip = g_boot.ip;
    nc.netmask = g_boot.netmask;
    nc.gateway = g_boot.gateway;
    nc.dns = g_boot.dns;
    memcpy(nc.ap_ssid, g_boot.ap_ssid, sizeof(nc.ap_ssid));
    nc.ap_ssid_len = g_boot.ap_ssid_len;
    memcpy(nc.ap_bssid, g_boot.ap_bssid, 6);
    nc.ap_channel = g_boot.ap_channel;
    nc.ap_security = g_boot.ap_security;
    nc.ap_pair_cipher = g_boot.ap_pair_cipher;
    nc.ap_group_cipher = g_boot.ap_group_cipher;
    nc.pmk_valid = g_boot.pmk_valid;
    memcpy(nc.pmk, g_boot.pmk, 32);
    nc.host = g_boot.host;
    strcpy(user_agent, "DScheevos/" DSC_VERSION " (Nintendo DSi) rcheevos/" RCHEEVOS_VERSION_STRING);
    nc.user_agent = user_agent;
    entropy_stir(&g_boot.unix_time, sizeof(g_boot.unix_time));
    entropy_stir(g_boot.token, sizeof(g_boot.token));   /* the pool's secret part */
    ring = net_init(&nc);
    /* the link keeps the one copy of the PMK it needs */
    memset(nc.pmk, 0, sizeof(nc.pmk));
    memset(g_boot.pmk, 0, sizeof(g_boot.pmk));
    memset(((dscm_boot_t*)g_session)->pmk, 0, sizeof(g_boot.pmk));
    dscm_dc_clean(((dscm_boot_t*)g_session)->pmk, sizeof(g_boot.pmk));
    /* still in the session: nothing writes the arena before the first frame */
    if (g_replies && g_boot.ca_length && g_boot.ca_offset <= g_boot.file_size && g_boot.ca_length <= g_boot.file_size - g_boot.ca_offset)
      https_add_ca(g_replies + g_boot.ca_offset, g_boot.ca_length);
    dscm_publish_net_ring((void*)DSCM_PHYS(ring));
    dscm_g_status.net_state = DSCM_NET_STATE_CONNECTING;
    H.net_started = 1;
    H.net_busy = 1;           /* the first pass services it */
  }
#ifdef DSCM_STREAM
  /* Open the UDP stream once the link is up. The phone loads the game from the
   * hash, but also shows its name in the "Switch to ...?" prompt, so pass the
   * title and game code from the NDS header the game left in main RAM (NTR mode
   * header at 0x027FFE00: title at +0x00, 12 bytes; game code at +0x0C, 4). */
  {
    const char* ndsh = (const char*)0x027FFE00;
    dscm_stream_start(g_boot.game_hash, ndsh + 0x0C, ndsh + 0x00, H.dtcm_fixed);
    dscm_stream_set_phone(g_boot.phone_ip);
  }
  if (dscm_stream_net_init() != 0)
    dscm_g_status.error = DSCM_ERROR_NO_MEMORY;
  dscm_popup_init();
  {
    /* The recording's ring: the snapshot arena's place, which the streamer
     * doesn't use (it loads no set), after the session file. */
    const uint32_t used = g_boot.file_size > HEAP_SESSION_HEAD ? g_boot.file_size : HEAP_SESSION_HEAD;
    const uint32_t start = (DSCM_SESSION + used + DSS_REC_SECTOR - 1) & ~(DSS_REC_SECTOR - 1);
    if (start < HEAP_ZONE_END)
      dscm_stream_record((void*)DSCM_DATA_ADDR(start), HEAP_ZONE_END - start,
                         g_boot.unix_time ^ dscm_vblank_count_read() ^ (uint32_t)g_boot.mac[5] << 8);
  }
#else
  /* nothing reads the session's replies or CA from here on */
  heap_release_session();
#endif
  g_initialized = 1;
  g_idle.frame = dscm_irq.frames;
  H.pass_frame = dscm_irq.frames - 1;   /* the first pass is a full one */
  dscm_irq.on = dscm_g_status.active ? 1 : 0;
  return dscm_g_status.active ? 0 : -1;
}

/* A VBlank with nothing captured yet: copy now (vblank_poll, idle time). */
void module_vblank_copy(void)
{
  refresh_dtcm(WP);
  dsc_vblank_copy();
}

/* Frames queued and not evaluated yet (dropped ones were never queued). */
/* Out of line: its dsc_stats_t would be in module_idle's frame, on every wake. */
static __attribute__((noinline)) uint32_t backlog(void)
{
  dsc_stats_t s;
  dsc_get_stats(&s);
  return s.frames_snapshotted - s.frames_evaluated - s.frames_discarded;
}

/* The core's counters for the status (and the in-game menu). */
static void publish_stats(void)
{
  /* diagnostics: is the 0x0C000000 mirror cached under the game's MPU (SDK5)?
   * Checked once the game's own MPU setup is in effect. The module doesn't
   * rely on the mirror either way. */
  dscm_g_status.alias_cached = dscm_irq.alias_cached;
  dsc_stats_t s;
  dsc_get_stats(&s);
  dscm_g_status.frames_snapshotted = s.frames_snapshotted;
  dscm_g_status.frames_dropped = s.frames_dropped;
  dscm_g_status.frames_evaluated = s.frames_evaluated;
  dscm_g_status.captures = s.captures;
  dscm_g_status.captures_stopped = s.captures_stopped;
  dscm_g_status.frames_stale = s.frames_stale;
  /* the IRQ's own counters */
  dscm_g_status.frames_missed = dscm_irq.frames_missed;
  dscm_g_status.timed_frames = dscm_irq.timed_frames;
  dscm_g_status.irq_lines_last = dscm_irq.irq_lines_last;
  dscm_g_status.irq_lines_max = dscm_irq.irq_lines_max;
  dscm_g_status.irq_lines_total = dscm_irq.irq_lines_total;
  dscm_g_status.idle_lines_last = g_idle.last;
  dscm_g_status.idle_lines_max = g_idle.max;
  dscm_g_status.idle_lines_total = g_idle.total;
  dscm_g_status.idle_none = g_idle.none;
  dscm_g_status.idle_cut = dscm_irq.idle_cut;
  dscm_g_status.halts_idle = H.halts_idle;
  dscm_g_status.halts_other = H.halts_other;
}

static void update_progress(void)
{
  dsc_progress_t p;
  dsc_get_progress(&p);
  dscm_g_status.achievements = p.achievements;
  dscm_g_status.achievements_unlocked = p.unlocked;
  dscm_g_status.points = p.points;
  dscm_g_status.points_unlocked = p.points_unlocked;
  if (p.hardcore_refused)
    dscm_g_status.hardcore = DSCM_HARDCORE_REFUSED;
  H.progress_stale = 0;
}

/* From flush_entry (start.s), on the module stack, with the game stopped
 * (cold: linked after the hot code, see module.ld):
 * evaluate what is queued (the in-game menu or a reset is about to have the
 * ARM7 copy the journal). Skipped if idle work holds the lock. */
#ifdef DSCM_STREAM
void module_flush(void) { }   /* no journal to flush when streaming */
#else
__attribute__((section(".text_cold"), noinline)) void module_flush(void)
{
  if (!dscm_irq.on || dscm_lock(&H.busy))
    return;
  dsc_flush();         /* the journal is cleaned to RAM at each step (journal_sync) */
  H.busy = 0;          /* notices, cues and stats: the next idle pass */
}
#endif

/* A reset (nds-bootstrap's reset path, IRQs off) sets H.reset_pending and
 * clears H.busy, a pass the game's threads were running never resumes:
 * start.s, reset_entry, writes them itself (the data view may be read-only
 * under the game's MPU then). */

/* Work only once a frame or when something is waiting (see module_idle):
 * all of it read from H, one cache line. */
#undef WP
#define WP wp
WAKE_INLINE int work_wanted(wake_page_t* wp)
{
  return g_frames != H.pass_frame || H.more || H.requests || H.net_busy || H.reset_pending || H.progress_stale;
}
#undef WP
#define WP WAKE_PAGE

/* One full pass of idle work, the lock held (H.busy). Evaluation, answers,
 * the network, notices; no snapshot, no halt. It may be parked at any
 * instruction (a game thread preempts the idle thread; idle-coverage's
 * BIOS-wait gate parks its coroutine) and resumed later: the lock keeps
 * every other caller out of the core meanwhile. */
static int idle_step_locked(void)
{
  uint32_t start, end, used;
  int new_frame, evaluated;

  if (!work_wanted(WP))
    return 0;
  new_frame = g_frames != H.pass_frame;
  H.pass_frame = g_frames;

  if (H.reset_pending) {
    H.reset_pending = 0;
    dsc_reset();
  }
  if (H.requests)
    poll_network();   /* answers that came in since the last pass */
  if (H.progress_stale)
    update_progress();

  start = REG_VCOUNT;
  evaluated = dsc_idle();   /* queued frames; answers, rcheevos' own timers: retries, pings */
  if (evaluated) {
    end = REG_VCOUNT;
    used = lines_between(start, end);
    dscm_g_status.eval_lines = (dscm_g_status.eval_lines * 7 + used * 16) / 8;
  }
  if (new_frame || evaluated) {
    clean_notices();
    update_cues();
  }

  /* Then the network: achievements come first. Every pass while something
   * is in flight or waiting (so an unlock goes out at once), else once a
   * frame for lwIP's timers and frames from the access point. */
  if (H.net_started && (new_frame || H.net_busy || H.requests)) {
    net_service(millis());
    H.net_busy = net_busy() != 0;
    dscm_g_status.net_state = net_state();
    dscm_g_status.net_detail = net_detail();
    dscm_g_status.tls_handshake_ms = net_handshake_ms();
    dscm_g_status.net_rejoins = link_rejoins();
    poll_network();
  }
  /* Work appears only through a VBlank (a new frame) or a pass like this
   * one (answers, events): what is left now is all the next wakes need. */
  H.more = dsc_work_pending() != 0;
  if (new_frame || evaluated)
    publish_stats();
  return 1;
}

/* One unit of idle work for a caller other than the OS_Halt path (see
 * idle_step_locked). Returns 0 when there was nothing to do, or the work is
 * already running elsewhere (parked). */
int dscm_idle_step(void)
{
  int r;
  if (!dscm_irq.on || dscm_lock(&H.busy))
    return 0;
  r = idle_step_locked();
  H.busy = 0;
  return r;
}

/* Called on the module's own stack from halt_entry, in the game's idle thread.
 * NitroSDK switches threads when an IRQ returns, so any game thread that wakes
 * (the main loop at VBlank, a loader thread at a card IRQ) preempts this work
 * at once; it resumes the next time the game is idle. eval_lines is wall time
 * between start and end, so it includes any time the game took meanwhile.
 * The idle lines in the status stop at the VBlank that preempts a pass (see
 * vblank_entry), so they leave out the game's frame but also the rest of
 * that pass; idle_cut counts such passes.
 * Returns nonzero when it halted (the caller returns to the game's loop),
 * 0 for the stock halt. */
#ifndef DSCM_STREAM
#undef WP
#define WP wp
WAKE_CODE int module_idle(int idle_loop)
{
  wake_page_t* const wp = wake_page_arm();
  uint32_t start;

  start = REG_VCOUNT;
  if (!dscm_irq.on)   /* initialized and active */
    return 0;
  /* A game thread that halts until its IRQ comes (while (!flag) OS_Halt())
   * is waiting for its own frame: any work here would delay the game. Only
   * the idle thread's loop is free time. */
  if (!idle_loop) {
    H.halts_other++;
    return 0;
  }
  /* idle work parked elsewhere (dscm_idle_step): the stock halt */
  if (wake_swap(&H.busy, 1))
    return 0;
  H.halts_idle++;
  /* IRQs stay on: only the idle thread touches these (no IRQ work of ours) */
  /* the game's MPU is set up once it idles: is the 0x0C000000 mirror
   * cached (SDK5)? vblank_count() reads past the cache either way */
  if (dscm_irq.alias_cached == 0xFF)
    alias_check();
  vblank_poll(WP);
  idle_frames(WP, 0);
  dscm_irq.idle_start = start;
  dscm_irq.idle_open = 1;

  /* The idle thread wakes at every IRQ, often many times a frame (20-40 in
   * Kirby Super Star Ultra). A full pass runs on the first wake after a
   * VBlank, and on any wake with work waiting: frames to evaluate, answers,
   * a reset, the network in the middle of something (work_wanted, from one
   * cache line). Every other wake only takes the snapshot and halts,
   * touching as few lines as it can (H, g_idle, dscm_irq). Nothing a full
   * pass would do on such a wake is skipped: dsc_idle() would find nothing
   * (dsc_work_pending), and the network gets a pass only when busy or once
   * a frame. */
  if (work_wanted(WP)) {
    /* Evaluation may take the rest of the frame: copy the memory the game
     * halted with first, so the next VBlank has it even if the copy before
     * the halt below comes too late. */
    if (backlog())
      wake_irq_restore(capture(WP));
    idle_step_locked();
  }
  /* Last, the snapshot and the halt, with IRQs masked from one to the
   * other: the idle thread runs only when every game thread is blocked, so
   * this is the game's memory when it stopped, and no game code can run
   * between the copy and the halt. The CPU wakes from the halt when an IRQ
   * is requested (IE & IF), masked or not; the IRQ is taken when the mask
   * is lifted. The copy stops at once if an IRQ is already waiting, and the
   * halt then returns at once too.
   * It halts even with frames still waiting: the next IRQ brings the next
   * pass. In melonDS, an idle loop that never halted stalled Kirby Super
   * Star Ultra's streaming for a minute (tests/perf/AB.md). */
  {
    uint32_t cpsr = capture(WP);
    start = dscm_irq.idle_start;
    /* else a VBlank came meanwhile and counted the pass */
    idle_frames(WP, wake_swap(&dscm_irq.idle_open, 0) ? lines_since(start) : 0);
    H.busy = 0;       /* IRQs masked: nothing runs before the halt */
    wake_wfi();
    wake_irq_restore(cpsr);
    return 1;   /* halted here */
  }
}
#undef WP
#define WP WAKE_PAGE
#else
/* Cheevotron streamer idle: once per new frame, service the Wi-Fi link and
 * send the watched words to the phone. No evaluation and no snapshot arena;
 * the only timing care is masking IRQs while the words are read (stream.c).
 * A pop-up's band is armed first (it has a deadline in the frame), and again
 * after the network work if an IRQ ended it meanwhile (popup.c).
 * Always returns 0, so the game's own OS_Halt runs after. */
static uint32_t g_status_frame;   /* frame of the last STATUS */

/* Native cart mode: the game reads its own cartridge, so no card read reaches
 * nds-bootstrap's ARM9 engine, which is where it puts its IPC sync IRQ back
 * into IE when a game cleared it (the in-game menu and resets ride on that
 * IRQ). Do it here once a frame instead. IPC_SYNC bit 14 set means the engine
 * asked for that IRQ. */
#define REG_IPC_SYNC (*(volatile uint16_t*)0x04000180)
static void ipc_sync_keep(void)
{
  uint32_t cpsr;
  if (!(g_boot.flags & DSCM_BOOT_CART_NATIVE) || !(REG_IPC_SYNC & 0x4000) || (REG_IE & 0x10000))
    return;
  cpsr = dscm_irq_off();
  REG_IE |= 0x10000;
  dscm_irq_restore(cpsr);
}

int module_idle(int idle_loop)
{
  uint32_t c;
  if (!dscm_irq.on || !idle_loop)
    return 0;
  if (dscm_lock(&H.busy))
    return 0;
  c = dscm_vblank_count_read();
  if (H.reset_pending) {
    H.reset_pending = 0;
    dscm_popup_reset();   /* the game starts over: the pop-ups' state goes, no register touched */
  }
  if (dscm_popup_active || dscm_popup_watch)
    dscm_popup_idle(c);
  if (c != dscm_irq.last_vblank) {
    dscm_irq.last_vblank = c;
    dscm_irq.frames = c;
    ipc_sync_keep();
    if (H.net_started) {
      net_service(dscm_millis());
      dscm_g_status.net_state = net_state();
      dscm_g_status.net_detail = net_detail();
      dscm_g_status.net_rejoins = link_rejoins();
    }
    dscm_stream_service(dscm_millis());
    dscm_stream_frame(c);
    if (c - g_status_frame >= DSS_FULL_EVERY) {
      /* by distance, not c % 60: idle passes skip frames (a 30 fps scene
       * idles on every other one), so an exact multiple may never come */
      g_status_frame = c;
      dscm_stream_status(c, net_state(), 0);
    }
  } else if (H.net_started && net_busy()) {
    net_service(dscm_millis());   /* let an in-flight exchange progress within a frame */
    dscm_stream_service(dscm_millis());
  } else {
    dscm_stream_service(dscm_millis());   /* HELLO cadence and liveness while unpaired */
  }
  if (dscm_popup_active)
    dscm_popup_rearm();
  H.busy = 0;
  return 0;
}
#endif /* DSCM_STREAM */

static void alias_check(void)
{
  dscm_irq.alias_cached = (uint8_t)mpu_dcacheable((uint32_t)DSCM_UNCACHED(&dscm_g_journal));
}

void module_shutdown(void)
{
  if (g_initialized)
    dsc_shutdown();
  g_initialized = 0;
  dscm_irq.on = 0;
  dscm_g_status.active = 0;
}

dscm_status_t* module_status(void) { return &dscm_g_status; }
dscm_net_slot_t* module_net(void) { return dscm_g_net; }
