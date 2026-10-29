/*
 * Cheevotron pop-ups on the DSi's own screen (DSCM_STREAM build).
 *
 * The phone evaluates the set; when something unlocks it sends a POPUP (and
 * the badge's pixels in BADGE chunks), and this draws it as a full-width band
 * at the bottom edge of the screen the game's main display engine (engine A)
 * drives. Wire contract: ds_stream_protocol.h.
 *
 * How the band gets on screen, without touching the game's VRAM or code (the
 * display lab's D1c strip, proven on a DSi in the game boot's own SCFG and
 * NWRAM setup by its test P1):
 *  - NDMA3 writes DISPCNT once per H-Blank from a per-line table: the game's
 *    own value above the band, the same with display mode 3 (main memory
 *    display) on the band's lines, the game's value again on line 191;
 *  - NDMA2 feeds the band's pixels from main RAM into the display FIFO, which
 *    mode 3 shows instead of the game's picture on those lines.
 * Both are armed from the game's idle thread (module_idle, at OS_Halt): every
 * game thread is blocked, so no game code runs until an IRQ. The first IRQ
 * goes through dscm_irq_wrapper (start.s), put on the BIOS IRQ vector at
 * arming: it stops both channels, puts mode 1 back, takes itself off the
 * vector, and only then runs the game's handler. The IPC receive-FIFO
 * interrupt alone (the ARM7's messages, about one a frame) is let through
 * instead, in games whose NitroSDK dispatcher we recognise: the wrapper
 * calls its handler with the band up, and the band goes on if the handler
 * woke no game thread (NitroSDK then wakes OS_WaitIrq's waiters, which look
 * and sleep again; collect() checks they did). So no game thread ever sees
 * mode 3 or loses a DISPCNT write.
 *
 * Everything here runs in the module's idle time; nothing runs in the game's
 * interrupts except the wrapper. Packets from the network are checked against
 * every bound before use: they land in a game's address space.
 */
#include "dscheevos_module.h"
#include "ds_stream_protocol.h"
#include "dsd.h"

#include <stddef.h>
#include <string.h>

/* gen/font_latin.c: nds-bootstrap's in-game menu font (display/tools/fontgen.py) */
extern const dsd_font_t dsd_font_latin;

/* stream.c: one packet to the paired phone (dropped when unpaired) */
void dscm_stream_send(uint8_t type, const void* body, uint16_t len);

/* start.s */
extern void dscm_irq_wrapper(void);
extern volatile uint32_t dscm_wrap_orig, dscm_wrap_hits, dscm_wrap_line, dscm_wrap_irqs;
extern volatile uint32_t dscm_wrap_pass, dscm_wrap_passes, dscm_wrap_woke, dscm_wrap_open;
extern volatile uint32_t dscm_wrap_table, dscm_wrap_switch, dscm_wrap_info, dscm_wrap_wake;
extern uint32_t dscm_cp15_control(void);
extern uint32_t dscm_dtcm_base(void);
extern uint32_t dscm_irq_off(void);
extern void dscm_irq_restore(uint32_t cpsr);
extern void dscm_dc_clean(const void* addr, uint32_t len);
extern void dscm_dc_invalidate(const void* addr, uint32_t len);

/* The host test (tests/popup) puts the registers and the IRQ vector in plain
 * memory; on the console these are the addresses themselves. */
#ifndef POPUP_IO
#define POPUP_IO(a) (a)
#endif
#ifndef POPUP_NDS_HEADER
#define POPUP_NDS_HEADER 0x027FFE00u           /* the game's header, where its boot left it */
#endif
#ifndef POPUP_CODE
#define POPUP_CODE(a) (a)                      /* the game's code and data, read where they are */
#endif
#ifndef POPUP_LINE_ANY_SOURCE
#define POPUP_LINE_ANY_SOURCE 0                /* the host test has no NWRAM to copy to */
#endif
#define REG8(a)  (*(volatile uint8_t*)POPUP_IO(a))
#define REG16(a) (*(volatile uint16_t*)POPUP_IO(a))
#define REG32(a) (*(volatile uint32_t*)POPUP_IO(a))
#define WRAPPER  ((uint32_t)(uintptr_t)dscm_irq_wrapper)
#define DISPCNT        REG32(0x04000000)
#define DISPCNT_MODE   REG8(0x04000002)        /* DISPCNT bits 16-23: the display mode in bits 0-1 */
#define DISPSTAT       REG16(0x04000004)
#define VCOUNT         REG16(0x04000006)
#define DISPCAPCNT     REG32(0x04000064)
#define MASTER_BRIGHT  REG16(0x0400006C)
#define REG_IE         REG32(0x04000210)
#define REG_IF         REG32(0x04000214)
#define POWCNT1        REG16(0x04000304)
#define DMA_CNT(x)     REG32(0x040000B8 + (x) * 12)
#define NDMA_SAD(x)    REG32(0x04004104 + (x) * 0x1C)
#define NDMA_DAD(x)    REG32(0x04004108 + (x) * 0x1C)
#define NDMA_TCNT(x)   REG32(0x0400410C + (x) * 0x1C)
#define NDMA_WCNT(x)   REG32(0x04004110 + (x) * 0x1C)
#define NDMA_BCNT(x)   REG32(0x04004114 + (x) * 0x1C)
#define NDMA_CNT(x)    REG32(0x0400411C + (x) * 0x1C)
#define NDMA_ON        0x80000000u
#define NDMA_START(m)  ((uint32_t)(m) << 24)
#define NDMA_DFIX      (2u << 10)
#define NDMA_PHYS(n)   ((uint32_t)(n) << 16)   /* blocks of 1 << n words */
#define MMEM_FIFO      0x04000068u
#define CH_FIFO        2                       /* NDMA2: display FIFO feeder */
#define CH_DISPCNT     3                       /* NDMA3: DISPCNT writer */
#define MODE_MASK      0x30000u
#define MODE_1         0x10000u
#define MODE_3         0x30000u
#define LAST_LINE      190                     /* the band ends here; line 191 is the game's */
#define POWCNT1_TOP    0x8000u                 /* engine A on the top screen */
#define ARM_LEAD       6                       /* lines before the band's first line the band is armed */
#define SCREEN_STABLE  8                      /* frames engine A stays on one screen before a band (a class-E game swaps every frame) */

/* ---- the band ---- */

#define TOAST_ROWS 32
#define SMALL_ROWS 16
#define MARGIN     4                           /* rows fed past the band: the FIFO reads ahead */
#define SLIDE      4                           /* frames to slide in, and out */
#define HOLD_DEFAULT 180
#define WAIT_MAX   600                         /* frames with no chance to show: dropped */
#define BADGE_WAIT 30                          /* frames a pop-up waits for its badge's chunks */

static uint16_t g_band[256 * (TOAST_ROWS + MARGIN)] __attribute__((aligned(32)));
#define BAND_WORDS ((TOAST_ROWS + MARGIN) * 128)
static uint32_t g_table[192] __attribute__((aligned(32)));   /* DISPCNT per line */

/* Where the display FIFO is fed from. Main RAM was starved in a game (its
 * sound and card DMA share that bus: rows came out shifted against each
 * other on hardware), so the band is copied by DMA into shared WRAM (NWRAM,
 * proven as a FIFO source in the display lab's D1n and P1), mapped to the
 * ARM9 by the game boot at 0x03700000-0x0377FFFF: the window at 0x03760000,
 * above nds-bootstrap's copy of the game's FNT/FAT at 0x03700000 and below
 * its colour LUT's tables (that option must stay off). DMA only: the game's
 * protection unit may not let the CPU at it. Main RAM if the window is taken. */
#define NW_BASE    0x03760000u
#define NW_FNTFAT  0x03700000u
#define NW_TABLE   (NW_BASE + 0x5000u)         /* the DISPCNT table, after the band */
static uint32_t g_table_src;                   /* where NDMA3 reads the table: NW_TABLE or g_table */
static uint32_t g_src;                         /* the band's first word for the FIFO */
static uint8_t  g_nwram;                       /* g_src is the NWRAM window */

static uint32_t g_table_base;                 /* what g_table holds: the game's DISPCNT, */
static uint16_t g_table_first;                 /* the band's first line, */
static uint8_t  g_table_mode;                  /* and the display mode (0 or 0xFF: not built) */

/* ---- pop-ups ---- */

typedef struct popup_t {
  dss_popup_state_t st;                        /* what the phone was last told, and the counts */
  uint8_t  kind, style, position, flags;
  uint16_t hold, badge_id;
  uint8_t  phase;                              /* P_* */
  uint16_t step;                               /* frames shown in this phase */
  uint16_t waited_run;                         /* frames since it last showed */
  char label[DSS_POPUP_LABEL_MAX + 1];
  char title[DSS_POPUP_TITLE_MAX + 1];
  char detail[DSS_POPUP_DETAIL_MAX + 1];
  char value[DSS_POPUP_VALUE_MAX + 1];
} popup_t;

enum { P_FREE = 0, P_QUEUED, P_IN, P_HOLD, P_OUT };

#define QUEUE_MAX 3
#define HISTORY   8
static popup_t g_cur;                          /* phase P_FREE: none */
static popup_t g_queue[QUEUE_MAX];
static uint8_t g_queue_count;
static dss_popup_state_t g_history[HISTORY];   /* finished ones, for repeated POPUPs */
static uint8_t g_history_head;

/* ---- badges: two slots, the phone sends the badge right before its pop-up ---- */

typedef struct badge_slot_t {
  uint16_t id;
  uint8_t  side;
  uint8_t  chunks;                             /* bit per DSS_BADGE_CHUNK_MAX chunk received */
  uint16_t total;
  uint16_t age;
  uint16_t px[32 * 32];
} badge_slot_t;
static badge_slot_t g_badges[2];
static uint16_t g_badge_age;

/* ---- the strip's state ---- */

/* A pop-up is waiting, up or armed: module_idle calls in only then, so the
 * game's idle wakes cost nothing more the rest of the time. */
uint8_t dscm_popup_active;
static uint8_t  g_mode;                        /* DSS_DISPLAY_*, from the phone (off until it says) */
static uint8_t  g_standdown;                   /* DSS_POPUP_WHY_*: off for the rest of the game */
static uint8_t  g_armed;                       /* the wrapper's entry for the last arming is due */
static uint8_t  g_pass_off;                    /* passing stopped for the game (pass_lost) */
static uint32_t g_armed_hits;                  /* dscm_wrap_hits at that arming */
static uint16_t g_armed_first;                 /* its band's first line */
static uint32_t g_shown_seen, g_cut_seen;      /* counts at the last frame tick */
static uint32_t g_frame;                       /* the VBlank count at the last tick */
static uint16_t g_screen_frames;               /* frames engine A stayed on the same screen */
static uint16_t g_screen_last;
static uint32_t g_fifo_frame;                  /* the frame the FIFO channel was last started in */
static uint8_t  g_skips_seen;                  /* SKIP(): reasons counted in this frame */
static uint8_t  g_wakes;                       /* W_*: where this frame's idle wakes fell, while a band was wanted */
#define W_EARLY  1                             /* in time to arm the band */
#define W_LATE   2                             /* too late: at or in the band's lines */
#define W_VBLANK 4                             /* in the VBlank before the frame's lines */
#define VECTOR_LOST_MAX 3                      /* the vector changed under an armed band this often: off */
static uint8_t  g_rendered;                    /* g_band holds g_cur */
static uint16_t g_rows;                        /* g_cur's band height */
static dss_display_status_t g_stats;

static uint8_t g_widths[256];

/* ---- colours (BGR555) ---- */

#define RGB(r, g, b) ((uint16_t)((r) | (g) << 5 | (b) << 10))
#define C_BG     DSS_POPUP_BACKGROUND
#define C_EDGE   RGB(9, 10, 15)
#define C_TEXT   RGB(31, 31, 31)
#define C_DIM    RGB(19, 20, 23)
#define C_GOLD   RGB(31, 25, 6)
#define C_SKY    RGB(13, 24, 31)
#define C_GREEN  RGB(11, 27, 12)
#define C_BLUE   RGB(11, 20, 31)
#define C_RED    RGB(31, 9, 8)

/* 16x16 icons for pop-ups without a badge (the display library's, bit 0 = left) */
static const uint16_t icon_trophy[16] = {0x0000, 0x3ffc, 0x6ff6, 0x4ff2, 0x4ff2, 0x6ff6, 0x3ffc, 0x1ff8, 0x07e0, 0x0180, 0x0180, 0x03c0, 0x07e0, 0x0ff0, 0x0ff0, 0x0000};
static const uint16_t icon_crown[16]  = {0x0000, 0x0000, 0x4182, 0x63c6, 0x77ee, 0x7ffe, 0x7ffe, 0x7ffe, 0x6ff6, 0x7ffe, 0x7ffe, 0x3ffc, 0x0000, 0x3ffc, 0x0000, 0x0000};
static const uint16_t icon_flag[16]   = {0x0000, 0x0004, 0x0ffc, 0x1ffc, 0x3ffc, 0x1ffc, 0x0ffc, 0x07fc, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x000e, 0x001f, 0x0000};
static const uint16_t icon_bars[16]   = {0x0000, 0x0000, 0x6000, 0x6000, 0x6600, 0x6600, 0x6660, 0x6660, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x7ffe, 0x0000, 0x0000};
static const uint16_t icon_shield[16] = {0x0000, 0x0180, 0x0ff0, 0x3ffc, 0x3ffc, 0x3c3c, 0x381c, 0x381c, 0x3c3c, 0x1ff8, 0x1ff8, 0x0ff0, 0x07e0, 0x03c0, 0x0180, 0x0000};

static const char* default_label(uint8_t kind)
{
  switch (kind) {
    case DSS_POPUP_MASTERY:     return "Set mastered";
    case DSS_POPUP_LEADERBOARD: return "Leaderboard";
    case DSS_POPUP_PROGRESS:    return "Progress";
    case DSS_POPUP_STATUS:      return "Cheevotron";
    case DSS_POPUP_TEST:        return "Pop-up test";
    default:                    return "Achievement unlocked";
  }
}

static uint16_t accent_of(const popup_t* p)
{
  switch (p->kind) {
    case DSS_POPUP_LEADERBOARD: return C_SKY;
    case DSS_POPUP_PROGRESS:    return C_GREEN;
    case DSS_POPUP_STATUS:      return (p->flags & DSS_POPUP_FLAG_HARDCORE) ? C_RED : C_BLUE;
    default:                    return C_GOLD;
  }
}

static const uint16_t* icon_of(uint8_t kind)
{
  switch (kind) {
    case DSS_POPUP_MASTERY:     return icon_crown;
    case DSS_POPUP_LEADERBOARD: return icon_flag;
    case DSS_POPUP_PROGRESS:    return icon_bars;
    case DSS_POPUP_STATUS:      return icon_shield;
    default:                    return icon_trophy;
  }
}

/* ---- telling the phone ---- */

static void send_state(popup_t* p)
{
  dscm_stream_send(DSS_T_POPUP_STATE, &p->st, sizeof(p->st));
}

static void set_state(popup_t* p, uint8_t state, uint8_t reason)
{
  p->st.state = state;
  if (reason)
    p->st.reason = reason;
  p->st.screen = (POWCNT1 & POWCNT1_TOP) ? 1 : 0;
  send_state(p);
}

static void remember(const dss_popup_state_t* st)
{
  g_history_head = (uint8_t)((g_history_head + 1) % HISTORY);
  g_history[g_history_head] = *st;
}

/* A finished pop-up: its last state to the phone and the history. */
static void finish(popup_t* p, uint8_t state, uint8_t reason)
{
  set_state(p, state, reason);
  remember(&p->st);
  p->phase = P_FREE;
}

static void refuse(uint16_t id, uint8_t reason)
{
  dss_popup_state_t st;
  memset(&st, 0, sizeof(st));
  st.popup_id = id;
  st.state = DSS_POPUP_REFUSED;
  st.reason = reason;
  st.screen = (POWCNT1 & POWCNT1_TOP) ? 1 : 0;
  dscm_stream_send(DSS_T_POPUP_STATE, &st, sizeof(st));
  remember(&st);
}

/* ---- packets ---- */

/* A string field: a length byte, then that many bytes, copied with a NUL.
 * Returns the bytes used, or 0 if it runs past the packet or the field. */
static uint16_t take_string(const uint8_t* p, uint16_t left, char* out, uint16_t max)
{
  uint16_t n;
  if (left < 1)
    return 0;
  n = p[0];
  if (n > max || 1 + n > left)
    return 0;
  memcpy(out, p + 1, n);
  out[n] = '\0';
  return (uint16_t)(1 + n);
}

static int known(uint16_t id)
{
  int i;
  if (g_cur.phase != P_FREE && g_cur.st.popup_id == id) {
    send_state(&g_cur);
    return 1;
  }
  for (i = 0; i < g_queue_count; i++)
    if (g_queue[i].st.popup_id == id) {
      send_state(&g_queue[i]);
      return 1;
    }
  for (i = 0; i < HISTORY; i++)
    if (g_history[i].popup_id == id) {
      dscm_stream_send(DSS_T_POPUP_STATE, &g_history[i], sizeof(g_history[i]));
      return 1;
    }
  return 0;
}

/* Why a new pop-up can't be queued now (DSS_POPUP_WHY_*), 0 if it can. */
static uint8_t queue_refusal(void)
{
  if (g_standdown)
    return g_standdown;
  if (g_mode == DSS_DISPLAY_OFF)
    return DSS_POPUP_WHY_OFF;
  if (g_queue_count >= QUEUE_MAX)
    return DSS_POPUP_WHY_BUSY;
  return 0;
}

/* Into the queue, the phone told it is queued. */
static void enqueue(const popup_t* p)
{
  popup_t* q = &g_queue[g_queue_count];
  if (!dscm_popup_active) {
    /* the screen is watched from now on (ticks run only while active) */
    g_screen_last = POWCNT1 & POWCNT1_TOP;
    g_screen_frames = 0;
    dscm_popup_active = 1;
  }
  *q = *p;
  q->phase = P_QUEUED;
  q->st.edge = 0;       /* the bottom edge only, for now */
  set_state(q, DSS_POPUP_QUEUED, DSS_POPUP_WHY_NONE);
  g_queue_count++;
}

void dscm_popup_on_popup(const uint8_t* body, uint16_t len)
{
  static popup_t p;     /* a few hundred bytes: off the module's small stack */
  uint16_t at, n;
  if (len < sizeof(dss_popup_t))
    return;
  memset(&p, 0, sizeof(p));
  p.st.popup_id = (uint16_t)(body[0] | body[1] << 8);
  if (!p.st.popup_id || known(p.st.popup_id))
    return;
  p.kind = body[2];
  p.style = body[3] == DSS_POPUP_SMALL ? DSS_POPUP_SMALL : DSS_POPUP_TOAST;
  p.position = body[4];
  p.flags = body[5];
  p.hold = (uint16_t)(body[6] | body[7] << 8);
  if (!p.hold || p.hold > 1200)
    p.hold = HOLD_DEFAULT;
  p.badge_id = (uint16_t)(body[8] | body[9] << 8);
  at = sizeof(dss_popup_t);
  if (!(n = take_string(body + at, (uint16_t)(len - at), p.label, DSS_POPUP_LABEL_MAX))) goto bad;
  at = (uint16_t)(at + n);
  if (!(n = take_string(body + at, (uint16_t)(len - at), p.title, DSS_POPUP_TITLE_MAX))) goto bad;
  at = (uint16_t)(at + n);
  if (!(n = take_string(body + at, (uint16_t)(len - at), p.detail, DSS_POPUP_DETAIL_MAX))) goto bad;
  at = (uint16_t)(at + n);
  if (!(n = take_string(body + at, (uint16_t)(len - at), p.value, DSS_POPUP_VALUE_MAX))) goto bad;

  if ((n = queue_refusal())) {
    refuse(p.st.popup_id, (uint8_t)n);
    return;
  }
  enqueue(&p);
  return;
bad:
  refuse(p.st.popup_id, DSS_POPUP_WHY_BAD);
}

void dscm_popup_set_mode(uint8_t mode);

/* A pop-up of the console's own, for when no phone can send one (the link to
 * it lost): shown like the phone's, its states going nowhere. A dry run the
 * phone asked for turns on: with the phone gone, nobody reads the counters. */
void dscm_popup_local(const char* title, const char* detail, const char* value)
{
  static popup_t p;
  static uint16_t id = 0xFF00;                 /* above the phone's ids */
  if (g_mode == DSS_DISPLAY_DRY || g_mode == DSS_DISPLAY_NOOP)
    dscm_popup_set_mode(DSS_DISPLAY_ON);
  if (queue_refusal())
    return;
  memset(&p, 0, sizeof(p));
  p.st.popup_id = id;
  id = (uint16_t)(id == 0xFFFF ? 0xFF00 : id + 1);
  p.style = DSS_POPUP_TOAST;
  p.hold = 600;                                /* 10 s: time to read it */
  strncpy(p.label, "Cheevotron", DSS_POPUP_LABEL_MAX);
  strncpy(p.title, title, DSS_POPUP_TITLE_MAX);
  strncpy(p.detail, detail, DSS_POPUP_DETAIL_MAX);
  strncpy(p.value, value, DSS_POPUP_VALUE_MAX);
  enqueue(&p);
}

static badge_slot_t* badge_find(uint16_t id)
{
  int i;
  for (i = 0; i < 2; i++)
    if (g_badges[i].id == id)
      return &g_badges[i];
  return NULL;
}

static int badge_ready(const badge_slot_t* b)
{
  uint8_t need;
  if (!b || !b->total)
    return 0;
  need = (uint8_t)((1u << ((b->total + DSS_BADGE_CHUNK_MAX - 1) / DSS_BADGE_CHUNK_MAX)) - 1);
  return (b->chunks & need) == need;
}

void dscm_popup_on_badge(const uint8_t* body, uint16_t len)
{
  uint16_t id, offset, total, n;
  uint8_t side;
  badge_slot_t* b;
  if (len < sizeof(dss_badge_t))
    return;
  id = (uint16_t)(body[0] | body[1] << 8);
  side = body[2];
  offset = (uint16_t)(body[4] | body[5] << 8);
  total = (uint16_t)(body[6] | body[7] << 8);
  n = (uint16_t)(len - sizeof(dss_badge_t));
  if (!id || (side != 32 && side != 16) || body[3] != DSS_BADGE_BGR555 || total != side * side * 2
      || n == 0 || n > DSS_BADGE_CHUNK_MAX || offset % DSS_BADGE_CHUNK_MAX || offset >= total || n > total - offset)
    return;
  b = badge_find(id);
  if (!b || b->side != side) {
    /* the older slot, never the current pop-up's */
    b = g_badges[0].age <= g_badges[1].age ? &g_badges[0] : &g_badges[1];
    if (g_cur.phase != P_FREE && b->id == g_cur.badge_id)
      b = b == &g_badges[0] ? &g_badges[1] : &g_badges[0];
    b->id = id;
    b->side = side;
    b->total = total;
    b->chunks = 0;
  }
  b->age = ++g_badge_age;
  memcpy((uint8_t*)b->px + offset, body + sizeof(dss_badge_t), n);
  b->chunks |= (uint8_t)(1u << (offset / DSS_BADGE_CHUNK_MAX));
}

/* ---- drawing ---- */

static int text_width(const uint8_t* g, int n)
{
  int w = 0, i;
  for (i = 0; i < n; i++)
    w += g_widths[g[i]];
  return w;
}

/* Glyphs that fit px pixels, ending in "..." when cut (as the display library's fit()). */
static int fit(uint8_t* g, int n, int px)
{
  int w = 0, i = 0, k;
  const int dots = 3 * g_widths['.'];
  if (text_width(g, n) <= px)
    return n;
  while (i < n && w + g_widths[g[i]] + dots <= px)
    w += g_widths[g[i++]];
  while (i > 0 && g[i - 1] == ' ')
    i--;
  for (k = 0; k < 3 && i < DSD_LINE_MAX; k++)
    g[i++] = '.';
  return i;
}

static void draw_text(uint16_t* band, int rows, int x, int y, const uint8_t* g, int n, uint16_t c)
{
  int i, r, j;
  for (i = 0; i < n; i++) {
    const uint8_t* glyph = dsd_font_latin.glyphs + g[i] * 8;
    for (r = 0; r < 8; r++) {
      uint8_t bits = glyph[r];
      if (y + r < 0 || y + r >= rows)
        continue;
      for (j = 0; bits; j++, bits >>= 1)
        if ((bits & 1) && x + j >= 0 && x + j < 256)
          band[(y + r) * 256 + x + j] = c;
    }
    x += g_widths[g[i]];
  }
}

/* One line of text at x (left side) or ending at 256 - x (right side). */
static void draw_line(uint16_t* band, int rows, int right, int x, int y, const char* s, int px, uint16_t c)
{
  uint8_t g[DSD_LINE_MAX];
  int n = fit(g, dsd_text_to_glyphs(&dsd_font_latin, s, g, DSD_LINE_MAX - 3), px);
  draw_text(band, rows, right ? 256 - x - text_width(g, n) : x, y, g, n, c);
}

/* The value ("10 pts") at the far end of the line; returns its width. */
static int draw_value(uint16_t* band, int rows, int right, int y, const char* s, uint16_t c)
{
  uint8_t g[DSD_LINE_MAX];
  int n, w;
  if (!s[0])
    return 0;
  n = dsd_text_to_glyphs(&dsd_font_latin, s, g, DSS_POPUP_VALUE_MAX);
  w = text_width(g, n);
  draw_text(band, rows, right ? 6 : 256 - 6 - w, y, g, n, c);
  return w;
}

/* g_cur into g_band, rows lines plus the margin; then to RAM for the DMA. */
static void render(void)
{
  const popup_t* p = &g_cur;
  const int rows = p->style == DSS_POPUP_SMALL ? SMALL_ROWS : TOAST_ROWS;
  const int right = (p->position & DSS_POPUP_POS_RIGHT) != 0;
  const uint16_t accent = accent_of(p);
  const badge_slot_t* b = p->badge_id ? badge_find(p->badge_id) : NULL;
  const int side = rows == TOAST_ROWS ? 32 : 16;
  int x, y, text_x, vw;
  uint32_t* w32 = (uint32_t*)g_band;
  const uint32_t bg2 = (uint32_t)C_BG | (uint32_t)C_BG << 16;

  for (x = 0; x < 256 * (rows + MARGIN) / 2; x++)
    w32[x] = bg2;
  for (x = 0; x < 256; x++)                    /* the edge against the game's picture */
    g_band[x] = C_EDGE;
  for (y = 1; y < rows; y++)                   /* accent bar on the badge's side */
    for (x = 0; x < 3; x++)
      g_band[y * 256 + (right ? 255 - x : x)] = accent;

  if (b && badge_ready(b) && b->side == side) {
    const int bx = right ? 256 - 4 - side : 4, by = (rows - side) / 2;
    for (y = 0; y < side; y++)
      for (x = 0; x < side; x++)
        g_band[(by + y) * 256 + bx + x] = b->px[y * side + x] & 0x7FFF;
    text_x = 4 + side + 4;
  } else {
    const uint16_t* icon = icon_of(p->kind);
    const int ix = right ? 256 - 22 : 6, iy = (rows - 16) / 2;
    for (y = 0; y < 16; y++) {
      uint16_t bits = icon[y];
      for (x = 0; bits; x++, bits >>= 1)
        if (bits & 1)
          g_band[(iy + y) * 256 + ix + x] = accent;
    }
    text_x = 26;
    if (p->badge_id)
      g_cur.st.reason = DSS_POPUP_WHY_NO_BADGE;
  }

  if (rows == TOAST_ROWS) {
    /* label in the accent colour with the value at the far end, title, detail */
    const int avail = 256 - text_x - 6;
    vw = draw_value(g_band, rows, right, 3, p->value, C_TEXT);
    draw_line(g_band, rows, right, text_x, 3, p->label[0] ? p->label : default_label(p->kind),
              avail - (vw ? vw + 8 : 0), accent);
    draw_line(g_band, rows, right, text_x, 13, p->title, avail, C_TEXT);
    draw_line(g_band, rows, right, text_x, 22, p->detail, avail, C_DIM);
  } else {
    /* one line: the title, the value at the far end in the accent colour */
    const int avail = 256 - text_x - 6;
    vw = draw_value(g_band, rows, right, 4, p->value, accent);
    draw_line(g_band, rows, right, text_x, 4, p->title[0] ? p->title : default_label(p->kind),
              avail - (vw ? vw + 8 : 0), C_TEXT);
  }

  g_rows = (uint16_t)rows;
  g_rendered = 1;
  dscm_dc_clean(g_band, sizeof(g_band));   /* the DMA reads RAM, not the cache */
}

/* ---- the strip ---- */

/* words from src to dst (physical addresses) by DMA at once, NDMA3 while no
 * band is armed (the line path's VCOUNT IRQ may start NDMA2, the feed, at any
 * time; NDMA3 is free on that path); 1 when done. Bounded. */
static int dma_copy(uint32_t src, uint32_t dst, uint32_t words)
{
  int i;
  if (NDMA_CNT(CH_DISPCNT) & NDMA_ON)
    return 0;
  NDMA_SAD(CH_DISPCNT) = src;
  NDMA_DAD(CH_DISPCNT) = dst;
  NDMA_WCNT(CH_DISPCNT) = words;
  NDMA_BCNT(CH_DISPCNT) = 0;
  NDMA_CNT(CH_DISPCNT) = NDMA_ON | NDMA_START(0x10) | NDMA_PHYS(3);
  for (i = 0; i < 200000 && (NDMA_CNT(CH_DISPCNT) & NDMA_ON); i++) {}
  if (NDMA_CNT(CH_DISPCNT) & NDMA_ON) {
    NDMA_CNT(CH_DISPCNT) = 0;
    return 0;
  }
  return 1;
}

/* g_band (cleaned) into the NWRAM window by DMA while no band is up; 1 if a
 * few words read back by DMA match. */
static int copy_to_nwram(void)
{
  static uint32_t check[8] __attribute__((aligned(32)));
  const uint32_t* words = (const uint32_t*)g_band;
  const uint32_t at = 2 * 128;                 /* row 2: the label's line */
  int i;
  if (!dma_copy(DSCM_PHYS(g_band), NW_BASE, BAND_WORDS))
    return 0;
  dscm_dc_invalidate(check, sizeof(check));
  if (!dma_copy(NW_BASE + at * 4, DSCM_PHYS(check), 8))
    return 0;
  dscm_dc_invalidate(check, sizeof(check));
  for (i = 0; i < 8; i++)
    if (check[i] != words[at + i])
      return 0;
  return 1;
}

/* After render(): the band goes into the NWRAM window when it's free and the
 * copy reads back right, else the FIFO is fed from main RAM. */
static void choose_source(void)
{
  g_src = DSCM_PHYS(g_band);
  if (g_nwram && copy_to_nwram())
    g_src = NW_BASE;
  g_stats.nwram = g_src == NW_BASE;
}

/* Our channels off and mode 1 back, as the wrapper does: for when the wrapper
 * can't run (the vector was changed under us) or a band is undone at once. */
static void stop_channels(void)
{
  int i;
  if ((NDMA_CNT(CH_DISPCNT) & NDMA_ON) && NDMA_DAD(CH_DISPCNT) == 0x04000000u)
    NDMA_CNT(CH_DISPCNT) = 0;
  for (i = 0; i < 64 && (NDMA_CNT(CH_DISPCNT) & NDMA_ON) && NDMA_DAD(CH_DISPCNT) == 0x04000000u; i++) {}
  if ((NDMA_CNT(CH_FIFO) & NDMA_ON) && NDMA_DAD(CH_FIFO) == MMEM_FIFO)
    NDMA_CNT(CH_FIFO) = 0;
  for (i = 0; i < 2; i++)                      /* again, should a last table word have landed */
    if ((DISPCNT & MODE_MASK) == MODE_3)
      DISPCNT_MODE = (uint8_t)((DISPCNT_MODE & ~3) | 1);
}

static volatile uint32_t* irq_vector(void)
{
#ifdef POPUP_HOST_VECTOR
  return POPUP_HOST_VECTOR;
#else
  return (volatile uint32_t*)(dscm_dtcm_base() + 0x3FFC);
#endif
}

/* ---- the game's IRQ dispatcher, for letting the IPC interrupt pass ---- */

/* NitroSDK's OS_IrqHandler as the display census found it in 43 of 45 games
 * (docs/display-research/survey.md section 5), the IRQ table and
 * OS_IrqHandler_ThreadSwitch in the two literal words after it: V1 (release
 * builds, conditional returns) and V2 (2007-2009 titles, branch pairs). Both
 * do the same: IME and IE & IF checks, the lowest pending bit acknowledged,
 * its table entry called with lr = ThreadSwitch, their own return pushed. */
static const uint32_t k_disp_v1[20] = {
  0xE92D4000, 0xE3A0C301, 0xE28CCE21, 0xE51C1008, 0xE3510000, 0x08BD8000, 0xE89C0006, 0xE0111002,
  0x08BD8000, 0xE3A03102, 0xE16F0F11, 0xE1D11033, 0x1AFFFFFC, 0xE1A01033, 0xE58C1004, 0xE270001F,
  0xE59F1008, 0xE7910100, 0xE59FE004, 0xE12FFF10,
};
static const uint32_t k_disp_v2[24] = {
  0xE92D4000, 0xE3A0C301, 0xE28CCE21, 0xE51C1008, 0xE3510000, 0x0A000000, 0xEA000000, 0xE8BD8000,
  0xE89C0006, 0xE0111002, 0x0A000000, 0xEA000000, 0xE8BD8000, 0xE3A03102, 0xE16F0F11, 0xE1D11033,
  0x1AFFFFFC, 0xE1A01033, 0xE58C1004, 0xE270001F, 0xE59F1008, 0xE7910100, 0xE59FE004, 0xE12FFF10,
};

static struct {
  uint32_t seen;                               /* the vector the code dump was queued for */
  uint8_t  variant;                            /* DISP_*: what dispatcher_known() found last */
  uint8_t  dump_left;                          /* DSS_T_CODE chunks still to send */
  uint32_t table, sw, info, wake;
} g_disp;
#define DISP_V1      0x01                      /* the dispatcher, either variant */
#define DISP_V2      0x02
#define DISP_SDK3    0x10                      /* its ThreadSwitch: NitroSDK 3.x/4.x */
#define DISP_SDK2    0x20                      /* NitroSDK 2.0 */

#define CODE32(a) (*(const volatile uint32_t*)POPUP_CODE(a))

static int in_main_ram(uint32_t a) { return a - 0x02000000u < 0x01000000u; }
static int in_itcm(uint32_t a) { return a - 0x01000000u < 0x01000000u; }
static int in_dtcm(uint32_t a, uint32_t room) { return a - dscm_dtcm_base() < 0x4000u - room; }

/* OS_IrqHandler_ThreadSwitch's start up to its isNeedRescheduling test, word
 * for word, the masked words being ldr ip,=X (literal offsets) and OSThread
 * field offsets.
 * NitroSDK 3.x (the pokediamond decomp) and 4.x (Pokemon HG, Platinum,
 * Black): OSi_IrqThreadQueue's threads woken (state = READY, queue and link
 * cleared), the queue emptied, isNeedRescheduling set; then the test. */
static const uint32_t k_switch3[23] = {
  0xE59FC000, 0xE3A03000, 0xE59CC000, 0xE3A02001, 0xE35C0000, 0x0A00000D, 0xE58C2000, 0xE58C3000,
  0xE58C3000, 0xE59C0000, 0xE58C3000, 0xE1A0C000, 0xE35C0000, 0x1AFFFFF7, 0xE59FC000, 0xE58C3000,
  0xE58C3004, 0xE59FC000, 0xE3A01001, 0xE1CC10B0, 0xE59FC000, 0xE1DC10B0, 0xE3510000,
};
#define K_SWITCH3_MASKED ((1u << 0) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 14) | (1u << 17) | (1u << 20))
/* NitroSDK 2.0 (Super Mario 64 DS): a halfword of IRQ wake bits read and
 * cleared; if any, isNeedRescheduling set and every thread waiting on one of
 * those IRQs made READY; then the test. */
static const uint32_t k_switch2[21] = {
  0xE59FC000, 0xE1DC30B0, 0xE3A00000, 0xE3530000, 0x0A00000C, 0xE1CC00B0, 0xE59FC000, 0xE3A01001,
  0xE1CC10B0, 0xE59CC00C, 0xE3A02001, 0xE35C0000, 0x0A000004, 0xE59C006C, 0xE1130011, 0x158C2064,
  0xE59CC068, 0xEAFFFFF8, 0xE59FC000, 0xE1DC10B0, 0xE3510000,
};
#define K_SWITCH2_MASKED ((1u << 0) | (1u << 6) | (1u << 18))

/* The ldr ip,=X literal of the word i at sw. */
static uint32_t switch_literal(uint32_t sw, uint32_t i)
{
  return CODE32(sw + i * 4 + 8 + (CODE32(sw + i * 4) & 0xFFF));
}

static int switch_matches(uint32_t sw, const uint32_t* sig, uint32_t len, uint32_t masked)
{
  uint32_t i;
  for (i = 0; i < len; i++)
    if ((CODE32(sw + i * 4) & ((masked >> i) & 1 ? 0xFFFFF000u : 0xFFFFFFFFu)) != sig[i])
      return 0;
  return 1;
}

/* ThreadSwitch at sw: DISP_SDK3 or DISP_SDK2 with g_disp's info (and, SDK 2.0,
 * wake) filled in, every literal loaded twice agreeing; 0 if unknown. */
static uint32_t find_thread_switch(uint32_t sw)
{
  uint32_t q;
  if (!in_itcm(sw) || (sw & 3))
    return 0;
  if (switch_matches(sw, k_switch3, 23, K_SWITCH3_MASKED)) {
    const uint32_t next = CODE32(sw + 9 * 4) & 0xFFF;
    q = switch_literal(sw, 0);
    g_disp.info = switch_literal(sw, 17);
    g_disp.wake = 0;
    return q == switch_literal(sw, 14) && g_disp.info == switch_literal(sw, 20) && q != g_disp.info
           && (CODE32(sw + 10 * 4) & 0xFFF) == next && next >= 8
           && (CODE32(sw + 7 * 4) & 0xFFF) == next - 8 && (CODE32(sw + 8 * 4) & 0xFFF) == next - 4
           && in_main_ram(g_disp.info) && !(g_disp.info & 3)
           ? DISP_SDK3 : 0;
  }
  if (switch_matches(sw, k_switch2, 21, K_SWITCH2_MASKED)) {
    g_disp.info = switch_literal(sw, 6);
    g_disp.wake = switch_literal(sw, 0);
    return g_disp.info == switch_literal(sw, 18) && in_main_ram(g_disp.info) && !(g_disp.info & 3)
           && (in_dtcm(g_disp.wake, 2) || in_main_ram(g_disp.wake)) && !(g_disp.wake & 1)
           && g_disp.wake != g_disp.info
           ? DISP_SDK2 : 0;
  }
  return 0;
}

/* Whether the IRQ vector holds a dispatcher the wrapper can stand in for,
 * checked word for word at every arming (g_disp.variant says what matched);
 * the code dump to the phone queued once per vector value. */
static int dispatcher_known(uint32_t cur)
{
  uint32_t n, i;
  if (cur != g_disp.seen) {
    g_disp.seen = cur;
    g_disp.dump_left = 6;
  }
  g_disp.variant = 0;
  g_disp.sw = 0;
  if (!in_itcm(cur) || (cur & 3))
    return 0;
  for (n = 1; n <= 2; n++) {
    const uint32_t* sig = n == 1 ? k_disp_v1 : k_disp_v2;
    const uint32_t len = n == 1 ? 20 : 24;
    for (i = 0; i < len && CODE32(cur + i * 4) == sig[i]; i++) {}
    if (i < len)
      continue;
    g_disp.variant = (uint8_t)n;
    g_disp.table = CODE32(cur + len * 4);
    g_disp.sw = CODE32(cur + len * 4 + 4);
    if ((!in_dtcm(g_disp.table, 22 * 4) && !in_main_ram(g_disp.table)) || (g_disp.table & 3))
      return 0;
    g_disp.variant |= (uint8_t)find_thread_switch(g_disp.sw);
    return (g_disp.variant & (DISP_SDK3 | DISP_SDK2)) != 0;
  }
  return 0;
}

/* With the dispatcher known: the IPC receive-FIFO handler where code lives
 * (ARM or Thumb: the wrapper calls it with bx). */
static int pass_ready(void)
{
  const uint32_t handler = CODE32(g_disp.table + 18 * 4);
  return in_itcm(handler) || in_main_ram(handler);
}

/* One DSS_T_CODE chunk a call while some are due: the dispatcher's first 96
 * bytes, then ThreadSwitch's first 192 (or more of the dispatcher, when it
 * wasn't recognised). */
static void send_code(void)
{
  dss_code_t c;
  uint32_t k, base;
  if (!g_disp.dump_left || !g_disp.seen || !in_itcm(g_disp.seen))
    return;
  k = 6u - g_disp.dump_left;
  base = k < 2 || !g_disp.sw || !in_itcm(g_disp.sw) ? g_disp.seen + k * 48 : g_disp.sw + (k - 2) * 48;
  c.address = base;
  for (uint32_t i = 0; i < 48; i += 4) {
    const uint32_t w = CODE32(base + i);
    memcpy(c.bytes + i, &w, 4);
  }
  dscm_stream_send(DSS_T_CODE, &c, sizeof(c));
  g_disp.dump_left--;
}

/* The result of the last arming, once the wrapper has run for it. */
static void collect(void)
{
  uint32_t line, cpsr;
  if (!g_armed)
    return;
  /* IRQs masked: the wrapper can't run between the two reads */
  cpsr = dscm_irq_off();
  if (dscm_wrap_hits == g_armed_hits) {
    if (*irq_vector() == WRAPPER) {
      dscm_irq_restore(cpsr);
      return;                                  /* no IRQ yet, or only IPC ones let through */
    }
    if (dscm_wrap_open) {
      /* The wrapper called the game's handler for an IPC interrupt and it
       * never came back (it returned to the BIOS some other way): the band
       * ran on unguarded. Stop it, and let no interrupt through again. */
      stop_channels();
      dscm_wrap_open = 0;
      dscm_irq_restore(cpsr);
      g_armed = 0;
      g_pass_off = 1;
      if (g_stats.pass_lost < 0xFF)
        g_stats.pass_lost++;
      return;
    }
    /* The vector was changed while we were on it (nds-bootstrap's soft reset
     * clears it, or the game set another handler): nothing will stop the
     * band, so stop it here. A reset happens now and then; a game that keeps
     * replacing the vector gets no more pop-ups. */
    stop_channels();
    dscm_irq_restore(cpsr);
    g_armed = 0;
    if (++g_stats.vector_lost >= VECTOR_LOST_MAX)
      g_standdown = DSS_POPUP_WHY_VECTOR;
    return;
  }
  dscm_irq_restore(cpsr);
  g_armed = 0;
  line = dscm_wrap_line;
  if (line > LAST_LINE)
    g_stats.frames_shown++;                    /* the band drawn whole (or would have been) */
  else if (line >= g_armed_first) {
    g_stats.frames_cut++;
    g_stats.last_cut_line = (uint16_t)line;
    g_stats.last_cut_irqs = dscm_wrap_irqs;
  } else
    g_stats.early++;
}

/* Rows of the band to show this frame: it slides in and out. */
static int visible_rows(void)
{
  switch (g_cur.phase) {
    case P_IN:   return g_rows * (g_cur.step + 1) / SLIDE;
    case P_HOLD: return g_rows;
    case P_OUT:  return g_rows * (SLIDE - g_cur.step - 1) / SLIDE;
    default:     return 0;
  }
}

static int dma_busy(void)
{
  int i;
  for (i = 0; i < 4; i++) {
    const uint32_t c = DMA_CNT(i);
    if ((c & 0x80000000u) && ((c >> 27) & 7) != 1)   /* only VBlank-timed DMA can wait */
      return 1;
  }
  for (i = 0; i < 4; i++)
    if (NDMA_CNT(i) & NDMA_ON)                 /* card reads (NDMA0/1), anyone's NDMA2/3 */
      return 1;
  return 0;
}

/* The IPC interrupt may pass the wrapper: with the DISPCNT writer running (its
 * check after the handler), unless the phone asked for strict or passing
 * went wrong once in this game. */
static uint32_t pass_allowed(void)
{
  return !g_pass_off && g_mode >= DSS_DISPLAY_NOOP && g_mode != DSS_DISPLAY_ON_STRICT;
}

/* A reason a frame's band was skipped, counted once per frame however many
 * idle wakes it held up (some games wake 20-40 times a frame). */
#define SKIP(field, bit) do { if (!(g_skips_seen & (bit))) { g_skips_seen |= (bit); g_stats.field++; } } while (0)

/* Arm the band for this frame, from the idle thread: every game thread is
 * blocked, so only an IRQ can run game code before the halt. IRQs are masked
 * from the checks to the wrapper going on the vector, last. */
static void arm(void)
{
  const int rows = visible_rows();
  const uint32_t first = (uint32_t)(LAST_LINE + 1 - rows);
  volatile uint32_t* vec;
  uint32_t cpsr, vc, d, cur, l, ie, ds, ctl, pass;
  int i;

  if (rows <= 0 || g_armed || g_standdown || g_mode == DSS_DISPLAY_OFF)
    return;
  /* An IRQ ended this frame's band before its first line: the display FIFO
   * may still hold words the first arming fed it, which a second one in the
   * same frame would show first (proven on hardware only from a new frame). */
  if (g_mode != DSS_DISPLAY_ON_REARM && g_fifo_frame == g_frame)   /* DSS_DISPLAY_ON_REARM tries it */
    return;
  /* The per-line DISPCNT table, with IRQs still on (most of a line through
   * the uncached mirror): rebuilt only when the game's value, the band's
   * first line or the mode changed. DISPCNT is read again below. */
  d = DISPCNT;
  if (g_mode >= DSS_DISPLAY_NOOP && (d & MODE_MASK) == MODE_1
      && (g_table_mode != g_mode || g_table_base != d || g_table_first != first)) {
    for (l = 0; l < 192; l++)
      g_table[l] = (g_mode >= DSS_DISPLAY_ON && l >= first && l <= LAST_LINE) ? ((d & ~MODE_MASK) | MODE_3) : d;
    dscm_dc_clean(g_table, sizeof(g_table));
    /* in NWRAM next to the band, like it: its word each H-Blank is never
     * held up on the main RAM bus (a late one starts the band mid-line) */
    g_table_src = (g_nwram && dma_copy(DSCM_PHYS(g_table), NW_TABLE, 192)) ? NW_TABLE : DSCM_PHYS(g_table);
    g_table_mode = g_mode;
    g_table_base = d;
    g_table_first = (uint16_t)first;
  }
  /* Arm late, a few lines before the band's first line: an IRQ between the
   * arming and the band (one not let through) ends it before it starts, and
   * costs the frame (the
   * FIFO can't be fed twice in one frame: hardware run 3 showed the second
   * feed garbled). Until then this idle thread watches the line with IRQs
   * on: a game IRQ runs as usual, and a thread it wakes takes the CPU as it
   * would from the halt. Bounded: a frame is 263 lines. */
  vc = VCOUNT;
  if (vc < 192 && vc + ARM_LEAD < first) {
    for (i = 0; i < 400000; i++) {
      vc = VCOUNT;
      if (vc + ARM_LEAD >= first || vc >= 192)
        break;
    }
  }

  cpsr = dscm_irq_off();
  /* the wrapper must run at the VBlank at the latest; no IRQ on every line */
  ie = REG_IE;
  ds = DISPSTAT;
  if (!(ie & 1) || !(ds & 0x08) || ((ie & 2) && (ds & 0x10))) {
    SKIP(skip_irq, 1);
    goto out;
  }
  d = DISPCNT;
  if ((d & MODE_MASK) != MODE_1 || (d & 0x80) || ((MASTER_BRIGHT >> 14) && (MASTER_BRIGHT & 31))
      || (DISPCAPCNT & 0x80000000u)) {
    SKIP(skip_display, 2);
    goto out;
  }
  if (dma_busy()) {
    SKIP(skip_dma, 4);
    goto out;
  }
  if (g_screen_frames < SCREEN_STABLE || (POWCNT1 & POWCNT1_TOP) != g_screen_last) {
    SKIP(skip_screen, 8);
    goto out;
  }
  /* high vectors, so the BIOS's IRQ entry reads [DTCM+0x3FFC], and DTCM on */
  ctl = dscm_cp15_control();
  vec = irq_vector();
  if (!(ctl & (1u << 13)) || !(ctl & (1u << 16))) {
    SKIP(skip_vector, 16);
    goto out;
  }
  cur = *vec;
  if (cur == WRAPPER || (cur - 0x01000000u >= 0x01000000u && cur - 0x02000000u >= 0x01000000u)) {
    SKIP(skip_vector, 16);
    goto out;
  }
  if (g_mode >= DSS_DISPLAY_NOOP && (g_table_mode != g_mode || g_table_base != d || g_table_first != first)) {
    SKIP(skip_display, 2);                     /* DISPCNT changed since the table: next wake */
    goto out;
  }
  /* whether the IPC interrupt may pass this band (before the timed part) */
  pass = pass_allowed() && dispatcher_known(cur) && pass_ready();
  /* Out of this line's H-Blank (bounded), so its H-Blank trigger is still to
   * come and the table's first word lands at the end of this line. */
  for (i = 0; i < 256 && (DISPSTAT & 0x02); i++) {}
  vc = VCOUNT;
  if (vc >= 192 || vc + 2 > first) {
    SKIP(skip_late, 32);
    goto out;
  }
  if (REG_IE & REG_IF) {
    SKIP(skip_pending, 64);
    goto out;
  }
  /* a VCOUNT-match IRQ from here to the band's end would cut it every frame */
  if ((ie & 4) && (ds & 0x20)) {
    const uint32_t lyc = (ds >> 8) | ((ds & 0x80) << 1);
    if (lyc >= vc && lyc <= 191) {
      SKIP(skip_irq, 1);
      goto out;
    }
  }

  if (g_mode >= DSS_DISPLAY_NOOP) {
    /* NDMA3: from the H-Blank of this line, the next line's word each H-Blank */
    NDMA_CNT(CH_DISPCNT) = 0;
    NDMA_SAD(CH_DISPCNT) = g_table_src + (vc + 1) * 4;
    NDMA_DAD(CH_DISPCNT) = 0x04000000u;
    NDMA_TCNT(CH_DISPCNT) = 191 - vc;
    NDMA_WCNT(CH_DISPCNT) = 1;
    NDMA_BCNT(CH_DISPCNT) = 0;
    if ((DISPSTAT & 0x02) || VCOUNT != vc) {   /* this line's H-Blank came meanwhile */
      SKIP(skip_late, 32);
      goto out;
    }
    NDMA_CNT(CH_DISPCNT) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX | NDMA_PHYS(0);
    if (g_mode >= DSS_DISPLAY_ON) {
      /* NDMA2: the band's rows (and the margin) into the display FIFO; mode 1
       * lines don't drain it, so it is ready when the band's first line comes.
       * While it slides, the band's lower rows show: it rises from the edge. */
      NDMA_CNT(CH_FIFO) = 0;
      NDMA_SAD(CH_FIFO) = g_src + (uint32_t)(g_rows - rows) * 512;
      NDMA_DAD(CH_FIFO) = MMEM_FIFO;
      NDMA_TCNT(CH_FIFO) = (uint32_t)(rows + MARGIN) * 128;
      NDMA_WCNT(CH_FIFO) = 4;
      NDMA_BCNT(CH_FIFO) = 0;
      NDMA_CNT(CH_FIFO) = NDMA_ON | NDMA_START(0x09) | NDMA_PHYS(2) | NDMA_DFIX;
      g_fifo_frame = g_frame;                  /* fed now: no second start this frame */
    }
    if (VCOUNT != vc) {                        /* a line went by: the table would be a line off */
      stop_channels();
      SKIP(skip_late, 32);
      goto out;
    }
  }
  dscm_wrap_orig = cur;
  dscm_wrap_pass = pass;
  if (pass) {
    dscm_wrap_table = g_disp.table;
    dscm_wrap_switch = g_disp.sw;
    dscm_wrap_info = g_disp.info;
    dscm_wrap_wake = g_disp.wake;
  }
  dscm_wrap_open = 0;
  /* the wrapper's words to RAM: a game's cache invalidate must not lose them */
  dscm_dc_clean((const void*)&dscm_wrap_orig, 64);
  g_armed_hits = dscm_wrap_hits;
  g_armed_first = (uint16_t)first;
  g_armed = 1;
  *vec = WRAPPER;
  g_stats.arms++;
out:
  dscm_irq_restore(cpsr);
}

/* ---- the line path (start.s line_path) ----
 * The band switched on by our own VCOUNT IRQ every frame, whether the game
 * idles or not, and left up through every IRQ and game thread until the
 * VBlank (start.s describes the IRQ side). This side installs it when the
 * game leaves the VCOUNT IRQ unused, publishes each frame's band (the
 * wrapper latches it at the VBlank), takes it down again, and watches for
 * our mode leaking into the game: DISPCNT mode 3 with no band up, and
 * GX_DispOff's saved mode (sDispMode) reading 3, both put back to 1. */

/* start.s: the wrapper's words as one array; the line path's part, at the
 * offsets of start.s's L_* and C_*. */
extern volatile uint32_t dscm_wrap_words[64];
#define LW(off) dscm_wrap_words[(off) / 4]
enum {
  LW_STATE = 64, LW_STAGE = 68, LW_UP = 72, LW_OWN = 76, LW_NFIRST = 80, LW_NSRC = 84, LW_NWORDS = 88,
  LW_NSCREEN = 92, LW_FIRST = 96, LW_LYC = 112, LW_LATCHED = 120,
  LC_VC = 128, LC_ARMS = 132, LC_WHOLE = 136, LC_CUT = 140, LC_LATE = 144, LC_SKIP = 148, LC_LEAK = 152,
  LC_MODE0 = 156, LC_REASSERT = 160, LC_ENDED = 164, LC_LATENCY = 168, LC_SPIN = 172, LC_STANDDOWN = 176,
  LC_SKIPWHY = 180, LW_T2ADDR = 192, LW_T2VAL = 196, LW_NEXPIRE = 200, LW_LYC2 = 204,
  LC_DMASEEN = 208, LC_BUSYS = 212, LC_BUSYE = 216, LW_HOLD = 232, LC_HELD = 236, LC_HOLDIPC = 240,
  LC_GXS = 244, LC_GXE = 248,
};
#define LC_COUNT     10                        /* LC_VC .. LC_ENDED, read as deltas each tick */
#define LINE_EXPIRE  120                       /* frames a published band stays good without the idle pass */
#define LINE_LYC0    150                       /* LYC before the first band */
#define CODE16(a) (*(volatile uint16_t*)POPUP_CODE(a))

uint8_t dscm_popup_watch = 1;                  /* module.c calls in with no pop-up too: the GX_DispOff scan, the leak watch */
static uint8_t  g_line;                        /* the line path is installed */
static uint8_t  g_line_off;                    /* left to the game's VCOUNT use: the idle-armed path for the game */
static uint8_t  g_line_used;                   /* a line path went up in this game: the leak watch from then on */
static uint32_t g_line_seen[LC_COUNT];         /* the counters at the last tick */
static const uint16_t k_feed_off[6] = { LC_BUSYS, LC_BUSYE, LC_GXS, LC_GXE, LC_HELD, LC_HOLDIPC };
static uint32_t g_feed_seen[6];                /* those counters at the last tick */

/* GX_DispOff, word for word in NitroSDK 2.0 (Super Mario 64 DS) and 4.x
 * (Pokemon HG, Platinum): DISPCNT's mode bits saved in sDispMode (ldr r0,=),
 * mode 0 written; GX_DispOn writes sDispMode back. One run during a band of
 * ours saves 3; that is put back to 1 at the next idle pass, so the next
 * GX_DispOn doesn't show an unfed main-memory display. The masked words are
 * literal offsets. */
static const uint32_t k_dispoff[11] = {
  0xE3A0E301, 0xE59EC000, 0xE59F1000, 0xE20C2803, 0xE3A03000, 0xE59F0000, 0xE1A02822, 0xE1C130B0,
  0xE1C020B0, 0xE3CC0803, 0xE58E0000,
};
#define K_DISPOFF_MASKED ((1u << 2) | (1u << 5))
#define DISPOFF_STEP     2048                  /* words scanned per idle pass */
static struct {
  uint32_t at, end;                            /* the scan through the game's ARM9 image */
  uint32_t mode;                               /* sDispMode, 0 if not found */
  uint8_t  state;                              /* 0 scanning, 1 found, 2 not found */
} g_dispoff;

/* One step of the scan for GX_DispOff in the game's ARM9 image (from its
 * header's load address, up to 2 MB within the first 4 MB of main RAM). */
static void dispoff_scan(void)
{
  uint32_t a, end, i;
  if (g_dispoff.state)
    return;
  if (!g_dispoff.end) {
    const volatile uint8_t* h = (const volatile uint8_t*)(uintptr_t)POPUP_NDS_HEADER;
    const uint32_t load = (uint32_t)h[0x28] | (uint32_t)h[0x29] << 8 | (uint32_t)h[0x2A] << 16 | (uint32_t)h[0x2B] << 24;
    if (load - 0x02000000u >= 0x003C0000u || (load & 3)) {
      g_dispoff.state = 2;
      return;
    }
    g_dispoff.at = load;
    g_dispoff.end = load + 0x00200000u < 0x023C0000u ? load + 0x00200000u : 0x023C0000u;
  }
  end = g_dispoff.at + DISPOFF_STEP * 4 < g_dispoff.end ? g_dispoff.at + DISPOFF_STEP * 4 : g_dispoff.end;
  for (a = g_dispoff.at; a < end; a += 4) {
    if (CODE32(a) != k_dispoff[0])
      continue;
    for (i = 1; i < 11 && (CODE32(a + i * 4) & ((K_DISPOFF_MASKED >> i) & 1 ? 0xFFFFF000u : 0xFFFFFFFFu)) == k_dispoff[i]; i++) {}
    if (i < 11)
      continue;
    {
      const uint32_t mode = CODE32(a + 5 * 4 + 8 + (CODE32(a + 5 * 4) & 0xFFF));
      const uint32_t other = CODE32(a + 2 * 4 + 8 + (CODE32(a + 2 * 4) & 0xFFF));
      if (mode - 0x02000000u < 0x003C0000u && !(mode & 1) && mode != other) {
        g_dispoff.mode = mode;
        g_dispoff.state = 1;
        return;
      }
    }
  }
  g_dispoff.at = end;
  if (end >= g_dispoff.end)
    g_dispoff.state = 2;
}

static int line_wanted(void)
{
  return ((g_mode >= DSS_DISPLAY_DRY && g_mode <= DSS_DISPLAY_ON) || g_mode == DSS_DISPLAY_ON_HOLD)
         && !g_line_off && !g_standdown;
}

/* 0 watch (our VCOUNT IRQ only), 1 dry (the feed, and the game's own mode
 * written), 2 on */
static uint32_t line_stage(void)
{
  return g_mode == DSS_DISPLAY_ON || g_mode == DSS_DISPLAY_ON_HOLD ? 2 : g_mode == DSS_DISPLAY_NOOP ? 1 : 0;
}

/* A legacy DMA channel in main-memory display mode: the game feeds the FIFO
 * itself, so a mode 3 is its own. */
static int game_fifo_dma(void)
{
  int i;
  for (i = 0; i < 4; i++)
    if ((DMA_CNT(i) & 0xB8000000u) == 0xA0000000u)
      return 1;
  return 0;
}

/* From the idle pass, once a band of the line path has been on in this game:
 * mode 3 with no band of ours up (a game RMW straddled our VBlank write, or
 * GX_DispOn wrote back a leaked mode) and sDispMode reading 3, put back to 1;
 * never while the game feeds the FIFO itself or an idle-armed band is out.
 * The wrapper does the first at every IRQ while installed. */
static void leak_watch(void)
{
  uint32_t cpsr;
  if (!g_line_used || g_armed || game_fifo_dma())
    return;
  cpsr = dscm_irq_off();
  if (g_line && LW(LW_UP) && VCOUNT < LW(LW_LYC)) {
    /* a band of ours still marked up in a new frame: the VBlank IRQ didn't
     * come (the game masked it), so nothing put mode 1 back */
    if ((NDMA_CNT(CH_FIFO) & NDMA_ON) && NDMA_DAD(CH_FIFO) == MMEM_FIFO)
      NDMA_CNT(CH_FIFO) = 0;
    LW(LW_UP) = 0;
    dscm_dc_clean((const void*)dscm_wrap_words, 256);
  }
  if (!(g_line && LW(LW_UP)) && (DISPCNT & MODE_MASK) == MODE_3) {
    DISPCNT_MODE = (uint8_t)(DISPCNT_MODE & ~2);
    g_stats.line_leak++;
  }
  if (g_dispoff.mode) {
    const uint16_t m = CODE16(g_dispoff.mode);
    if (m == 3) {
      CODE16(g_dispoff.mode) = 1;
      g_stats.dispmode_fixed++;
    } else if (m > 3) {
      g_dispoff.mode = 0;                      /* not a display mode: not the variable we think */
    }
  }
  dscm_irq_restore(cpsr);
}

/* The line path on, with IRQs masked: the wrapper on the vector for good,
 * the VCOUNT IRQ ours (IF first cleared, so no stale match fires). Only when
 * the game leaves the VCOUNT IRQ alone, its VBlank IRQ is on, no H-Blank IRQ
 * runs (the spin would hold it off), its dispatcher is NitroSDK's (for the
 * VCOUNT slot of its table) and GX_DispOff is known. 1 if installed. */
static int line_install(void)
{
  volatile uint32_t* vec = irq_vector();
  const uint32_t ctl = dscm_cp15_control();
  uint32_t cpsr, cur, i;
  int ok;
  if (g_armed || g_dispoff.state != 1)
    return 0;
  cpsr = dscm_irq_off();
  cur = *vec;
  ok = (ctl & (1u << 13)) && (ctl & (1u << 16)) && cur != WRAPPER
       && (cur - 0x01000000u < 0x01000000u || cur - 0x02000000u < 0x01000000u)
       && (dispatcher_known(cur), (g_disp.variant & (DISP_V1 | DISP_V2)) != 0)
       && (in_dtcm(g_disp.table, 22 * 4) || in_main_ram(g_disp.table)) && !(g_disp.table & 3)
       && (g_src == NW_BASE || POPUP_LINE_ANY_SOURCE)   /* main RAM starves the feed while the game works */
       && g_dispoff.mode && CODE16(g_dispoff.mode) != 3 && !game_fifo_dma()
       && !(REG_IE & 4) && !(DISPSTAT & 0x20) && !(DISPSTAT & 0x80)
       && (REG_IE & 1) && (DISPSTAT & 0x08) && !((REG_IE & 2) && (DISPSTAT & 0x10))
       && (DISPCNT & MODE_MASK) == MODE_1;
  if (!ok) {
    dscm_irq_restore(cpsr);
    return 0;
  }
  for (i = 64; i < 256; i += 4)
    LW(i) = 0;
  LW(LW_T2ADDR) = g_disp.table + 2 * 4;
  LW(LW_T2VAL) = CODE32(g_disp.table + 2 * 4);
  LW(LW_STAGE) = line_stage();
  LW(LW_OWN) = 1;
  LW(LW_NEXPIRE) = LINE_EXPIRE;
  LW(LW_LYC) = LW(LW_LYC2) = LINE_LYC0;
  DISPSTAT = (uint16_t)((DISPSTAT & 0x00FF) | LINE_LYC0 << 8);
  REG_IF = 4;
  DISPSTAT = (uint16_t)(DISPSTAT | 0x20);
  REG_IE |= 4;
  dscm_wrap_orig = cur;
  LW(LW_STATE) = 1;
  dscm_dc_clean((const void*)dscm_wrap_words, 256);
  *vec = WRAPPER;
  dscm_irq_restore(cpsr);
  for (i = 0; i < LC_COUNT; i++)
    g_line_seen[i] = 0;
  for (i = 0; i < 6; i++)
    g_feed_seen[i] = 0;
  g_line = 1;
  dscm_popup_watch = 1;
  return 1;
}

/* The line path off, IRQs masked: the vector back if it's still ours, the
 * VCOUNT IRQ given back as we found it (give_back, unless the game put a
 * handler in its slot), a band still up taken down. */
static void line_uninstall(int give_back)
{
  volatile uint32_t* vec = irq_vector();
  const uint32_t cpsr = dscm_irq_off();
  if (*vec == WRAPPER)
    *vec = dscm_wrap_orig;
  if (give_back && CODE32(LW(LW_T2ADDR)) == LW(LW_T2VAL)) {
    REG_IE &= ~4u;
    DISPSTAT = (uint16_t)(DISPSTAT & ~0x20);
    REG_IF = 4;
  }
  if ((NDMA_CNT(CH_FIFO) & NDMA_ON) && NDMA_DAD(CH_FIFO) == MMEM_FIFO)
    NDMA_CNT(CH_FIFO) = 0;
  if (LW(LW_UP) && (DISPCNT & MODE_MASK) == MODE_3)
    DISPCNT_MODE = (uint8_t)(DISPCNT_MODE & ~2);
  LW(LW_UP) = 0;
  LW(LW_STATE) = 0;
  dscm_dc_clean((const void*)dscm_wrap_words, 256);
  dscm_irq_restore(cpsr);
  g_line = 0;
}

/* This frame's band for the wrapper to latch at the next VBlank (none: 0),
 * with its expiry: without the idle pass refreshing it, the band stops after
 * LINE_EXPIRE frames. */
static void line_publish(int rows)
{
  const uint32_t cpsr = dscm_irq_off();
  LW(LW_STAGE) = line_stage();
  LW(LW_HOLD) = g_mode == DSS_DISPLAY_ON_HOLD;
  if (rows > 0) {
    LW(LW_NFIRST) = (uint32_t)(192 - rows);
    LW(LW_NSRC) = g_src + (uint32_t)(g_rows - rows) * 512;
    LW(LW_NWORDS) = (uint32_t)(rows + MARGIN) * 128;
    LW(LW_NSCREEN) = POWCNT1 & POWCNT1_TOP;
  } else {
    LW(LW_NFIRST) = 0;
  }
  LW(LW_NEXPIRE) = LW(LW_LATCHED) + LINE_EXPIRE;
  dscm_dc_clean((const void*)dscm_wrap_words, 256);
  dscm_irq_restore(cpsr);
}

/* No band of ours latched or up: the band's pixels may be redrawn. */
static int line_down(void)
{
  return !g_line || (!LW(LW_NFIRST) && !LW(LW_FIRST) && !LW(LW_UP));
}

/* The counters since the last tick into the statistics; returns the frames a
 * band was up (whole or cut; in the watch stage, our VCOUNT IRQs). */
static uint32_t line_collect(uint32_t* cut)
{
  uint32_t d[LC_COUNT], i;
  for (i = 0; i < LC_COUNT; i++) {
    const uint32_t v = LW(LC_VC + i * 4);
    d[i] = v - g_line_seen[i];
    g_line_seen[i] = v;
  }
  g_stats.line_vc += d[0];
  g_stats.arms += d[1];
  g_stats.frames_shown += d[2];
  g_stats.frames_cut += d[3];
  g_stats.line_late += d[4];
  g_stats.line_skip += d[5];
  g_stats.line_leak += d[6];
  g_stats.line_mode0 += d[7];
  g_stats.line_reassert += d[8];
  g_stats.line_ended += d[9];
  g_stats.line_latency = (uint16_t)(LW(LC_LATENCY) < 0xFFFF ? LW(LC_LATENCY) : 0xFFFF);
  g_stats.line_spin = (uint16_t)(LW(LC_SPIN) < 0xFFFF ? LW(LC_SPIN) : 0xFFFF);
  g_stats.line_skipwhy = (uint8_t)LW(LC_SKIPWHY);
  g_stats.line_standdown = (uint8_t)LW(LC_STANDDOWN);
  {
    /* the feed's telemetry, as deltas like the counters above */
    uint32_t f[6];
    for (i = 0; i < 6; i++) {
      const uint32_t v = LW(k_feed_off[i]);
      f[i] = v - g_feed_seen[i];
      g_feed_seen[i] = v;
    }
    g_stats.feed_dma_seen |= LW(LC_DMASEEN);
    g_stats.feed_busy_start += f[0];
    g_stats.feed_busy_end += f[1];
    g_stats.feed_gx_start += f[2];
    g_stats.feed_gx_end += f[3];
    g_stats.feed_held += f[4];
    g_stats.feed_hold_ipc += f[5];
  }
  if (LW(LW_STAGE) == 2 && d[1])
    g_line_used = 1;                           /* our mode 3 went out: the leak watch for good */
  *cut = d[3];
  return LW(LW_STAGE) == 0 ? d[0] : d[2] + d[3];
}

/* The wrapper still on the vector? A soft reset (nds-bootstrap clears it)
 * or the game took it: the line path is gone; a band of ours still up is
 * taken down here. Also: our VCOUNT IRQ left to the game (stood down). */
static void line_check(void)
{
  uint32_t cpsr;
  if (!g_line)
    return;
  cpsr = dscm_irq_off();
  if (*irq_vector() != WRAPPER) {
    if ((NDMA_CNT(CH_FIFO) & NDMA_ON) && NDMA_DAD(CH_FIFO) == MMEM_FIFO)
      NDMA_CNT(CH_FIFO) = 0;
    if ((DISPCNT & MODE_MASK) == MODE_3)
      DISPCNT_MODE = (uint8_t)(DISPCNT_MODE & ~2);
    if (LW(LW_OWN) && CODE32(LW(LW_T2ADDR)) == LW(LW_T2VAL)) {
      REG_IE &= ~4u;                           /* the game's new handler gets no VCOUNT IRQ of ours */
      DISPSTAT = (uint16_t)(DISPSTAT & ~0x20);
      REG_IF = 4;
    }
    LW(LW_UP) = 0;
    LW(LW_STATE) = 0;
    dscm_dc_clean((const void*)dscm_wrap_words, 256);
    dscm_irq_restore(cpsr);
    g_line = 0;
    if (++g_stats.vector_lost >= VECTOR_LOST_MAX)
      g_standdown = DSS_POPUP_WHY_VECTOR;
    return;
  }
  dscm_irq_restore(cpsr);
  if (!LW(LW_OWN)) {
    /* left to the game: its VCOUNT setup stays as it is, except when it
     * kept turning ours off (stand-down 4): then off as it wants it */
    line_uninstall(LW(LC_STANDDOWN) == 4);
    g_line_off = 1;
  }
}

/* ---- the pop-up's life, a frame at a time ---- */

static void next_popup(void)
{
  if (g_cur.phase != P_FREE || !g_queue_count)
    return;
  g_cur = g_queue[0];
  memmove(&g_queue[0], &g_queue[1], (size_t)(g_queue_count - 1) * sizeof(popup_t));
  g_queue_count--;
  g_rendered = 0;
}

static void tick_life(uint32_t passed, uint32_t shown);

/* Once per new frame: what the band did since the last tick moves the pop-up on. */
static void tick(uint32_t frame)
{
  const uint32_t passed = frame - g_frame;
  const uint16_t screen = POWCNT1 & POWCNT1_TOP;
  uint32_t shown;
  int showed;
  g_frame = frame;
  g_skips_seen = 0;
  send_code();
  if (screen == g_screen_last) {
    if (g_screen_frames < 0xFFFF)   /* one a tick: a frame skipped between ticks may have swapped */
      g_screen_frames++;
  } else {
    g_screen_last = screen;
    g_screen_frames = 0;
  }
  g_stats.screen = screen ? 1 : 0;
  g_stats.mode = g_mode;

  if (g_line) {
    /* the line path: the frames a band was up since the last tick (in a
     * 30 fps scene, two a tick) */
    uint32_t cut;
    shown = line_collect(&cut);
    if (cut)
      g_cur.st.frames_cut = (uint16_t)(g_cur.st.frames_cut + cut);
  } else {
    shown = g_stats.frames_shown != g_shown_seen || g_stats.frames_cut != g_cut_seen;
    if (g_stats.frames_cut != g_cut_seen)
      g_cur.st.frames_cut++;
  }
  showed = shown != 0;
  g_shown_seen = g_stats.frames_shown;
  g_cut_seen = g_stats.frames_cut;
  if (g_wakes) {
    /* the frame just ended wanted the band: drawn, or where the game left
     * time for it; the frames since had no idle wake at all */
    g_stats.shown_bits = passed >= 32 ? 0 : (g_stats.shown_bits << 1 | (showed ? 1u : 0u)) << (passed - 1);
    if (!showed) {
      if (g_wakes & W_EARLY)
        g_stats.wake_early++;
      else if (g_wakes & W_LATE)
        g_stats.wake_late++;
      else
        g_stats.wake_vblank++;
    }
    g_stats.wake_none += passed - 1;
    g_wakes = 0;
  }

  tick_life(passed, shown);

  /* the line path: this frame's band published, or the path taken down
   * once no pop-up wants it and no band of ours is latched or up */
  if (g_line) {
    if (!line_wanted() || (g_cur.phase == P_FREE && !g_queue_count)) {
      line_publish(0);
      if (line_down())
        line_uninstall(LW(LW_OWN) != 0);
    } else {
      line_publish(g_cur.phase >= P_IN && g_rendered && g_screen_frames >= SCREEN_STABLE ? visible_rows() : 0);
    }
  }
}

/* The pop-up's life for a tick: passed frames since the last, shown of them
 * with the band up. */
static void tick_life(uint32_t passed, uint32_t shown)
{
  uint32_t n;
  if (g_mode == DSS_DISPLAY_OFF || g_standdown) {
    /* everything waiting is refused, one a frame (the ARM7's send ring is small) */
    if (g_cur.phase != P_FREE)
      finish(&g_cur, DSS_POPUP_DROPPED, g_standdown ? g_standdown : DSS_POPUP_WHY_OFF);
    else if (g_queue_count)
      finish(&g_queue[--g_queue_count], DSS_POPUP_REFUSED, g_standdown ? g_standdown : DSS_POPUP_WHY_OFF);
    return;
  }

  next_popup();
  if (g_cur.phase == P_FREE)
    return;
  g_stats.frames_wanted++;

  if (g_cur.phase == P_QUEUED) {
    /* the badge's chunks come right before the pop-up: wait a little for them */
    const badge_slot_t* b = g_cur.badge_id ? badge_find(g_cur.badge_id) : NULL;
    if (g_cur.badge_id && !badge_ready(b) && g_cur.st.frames_waited < BADGE_WAIT) {
      g_cur.st.frames_waited++;
      return;
    }
    if (g_armed || !line_down())
      return;                                  /* the last pop-up's band is still armed or latched */
    render();
    choose_source();
    g_cur.phase = P_IN;
    g_cur.step = 0;
    g_cur.waited_run = 0;
    if (line_wanted() && !g_line)
      line_install();                          /* else the idle-armed path for this pop-up */
    return;
  }

  if (!shown) {
    g_cur.st.frames_waited = (uint16_t)(g_cur.st.frames_waited + (passed < 0xFFFF ? passed : 1));
    g_cur.waited_run = (uint16_t)(g_cur.waited_run + (passed < 0xFFFF ? passed : 1));
    if (g_cur.waited_run >= WAIT_MAX)
      finish(&g_cur, g_cur.st.frames_shown ? DSS_POPUP_DONE : DSS_POPUP_DROPPED, DSS_POPUP_WHY_WAITED);
    return;
  }
  g_cur.waited_run = 0;
  g_cur.st.frames_shown = (uint16_t)(g_cur.st.frames_shown + shown < 0xFFFF ? g_cur.st.frames_shown + shown : 0xFFFF);
  if (g_cur.st.state == DSS_POPUP_QUEUED)
    set_state(&g_cur, DSS_POPUP_SHOWN, DSS_POPUP_WHY_NONE);
  /* a step a frame shown (the line path may have shown two since the last tick) */
  for (n = shown < 64 ? shown : 64; n && g_cur.phase != P_FREE; n--) {
    g_cur.step++;
    switch (g_cur.phase) {
      case P_IN:
        if (g_cur.step >= SLIDE) { g_cur.phase = P_HOLD; g_cur.step = 0; }
        break;
      case P_HOLD:
        if (g_cur.step >= g_cur.hold) { g_cur.phase = P_OUT; g_cur.step = 0; }
        break;
      case P_OUT:
        if (g_cur.step >= SLIDE - 1)
          finish(&g_cur, DSS_POPUP_DONE, DSS_POPUP_WHY_NONE);
        break;
    }
  }
}

/* ---- entry points for module.c and stream.c ---- */

void dscm_popup_init(void)
{
  int g;
  for (g = 0; g < 256; g++) {
    /* advance: up to the rightmost lit column, plus one (dsd_init's rule) */
    const uint8_t* rows = dsd_font_latin.glyphs + g * 8;
    uint8_t lit = 0;
    int r, w = 0;
    for (r = 0; r < 8; r++)
      lit |= rows[r];
    while (lit >> w)
      w++;
    g_widths[g] = w ? (uint8_t)(w + 1) : 4;
  }
  g_screen_last = POWCNT1 & POWCNT1_TOP;
  {
    /* The NWRAM window is free above nds-bootstrap's copy of the game's
     * FNT/FAT (its size as the card engine takes it, from the game's header,
     * which the game's boot left at 0x027FFE00). */
    const volatile uint8_t* h = (const volatile uint8_t*)(uintptr_t)POPUP_NDS_HEADER;
#define HDR32(o) ((uint32_t)h[o] | (uint32_t)h[(o) + 1] << 8 | (uint32_t)h[(o) + 2] << 16 | (uint32_t)h[(o) + 3] << 24)
    const uint32_t fnt_off = HDR32(0x40), fat_off = HDR32(0x48), fat_size = HDR32(0x4C);
#undef HDR32
    g_nwram = fat_off >= fnt_off && fat_off - fnt_off <= NW_BASE - NW_FNTFAT
              && fat_size <= NW_BASE - NW_FNTFAT - (fat_off - fnt_off);
  }
  g_src = DSCM_PHYS(g_band);
}

void dscm_popup_set_mode(uint8_t mode)
{
  if (mode > DSS_DISPLAY_ON_HOLD)
    return;
  g_mode = mode;
  g_table_mode = 0xFF;                         /* rebuild the table for the new mode */
}

static void update_active(void)
{
  dscm_popup_active = (uint8_t)(g_cur.phase != P_FREE || g_queue_count || g_armed || g_line);
  if (!dscm_popup_active)
    g_wakes = 0;                               /* the next tick may be minutes on */
}

/* Where this idle wake fell against the band's deadline (telemetry). The
 * frames a new pop-up waits for the screen to settle don't count: the pop-up
 * starts later, it doesn't flicker. */
static void note_wake(void)
{
  const uint32_t vc = VCOUNT;
  if (g_screen_frames < SCREEN_STABLE)
    return;
  if (vc >= 192)
    g_wakes |= W_VBLANK;
  else if (vc + 2 <= (uint32_t)(LAST_LINE + 1 - visible_rows()))
    g_wakes |= W_EARLY;
  else
    g_wakes |= W_LATE;
}

/* Each idle pass while a pop-up is waiting, up or armed, or the line path is
 * installed (dscm_popup_active), and while dscm_popup_watch asks (the
 * GX_DispOff scan, the leak watch), from module_idle with the lock held: the
 * last arming's result, the pop-up's frame tick when a VBlank came, then the
 * band for this frame (the idle-armed path). */
void dscm_popup_idle(uint32_t frame)
{
  dispoff_scan();
  leak_watch();
  if (!dscm_popup_active) {
    if (g_dispoff.state && !g_line_used)
      dscm_popup_watch = 0;                    /* nothing to watch until a pop-up comes */
    return;
  }
  line_check();
  collect();
  if (frame != g_frame)
    tick(frame);
  if (!g_line && g_cur.phase >= P_IN && g_rendered) {
    note_wake();
    arm();
  }
  update_active();
}

/* After the idle pass's own work (network, stream): arm again if an IRQ
 * ended the band meanwhile and there is still time in the frame (the
 * idle-armed path only). */
void dscm_popup_rearm(void)
{
  collect();
  if (!g_line && g_cur.phase >= P_IN && g_rendered)
    arm();
  update_active();
}

/* nds-bootstrap's reset (module.c, H.reset_pending): the game starts over,
 * with the vector, IE and the NDMA channels cleared by the reset. Our state
 * goes, no register is touched. */
void dscm_popup_reset(void)
{
  LW(LW_STATE) = 0;
  LW(LW_UP) = 0;
  dscm_dc_clean((const void*)dscm_wrap_words, 256);
  g_line = 0;
  g_line_off = 0;
  g_line_used = 0;
  g_armed = 0;
  g_cur.phase = P_FREE;
  g_queue_count = 0;
  g_rendered = 0;
  dscm_popup_active = 0;
  memset(&g_dispoff, 0, sizeof(g_dispoff));    /* looked for again: the image may differ */
  dscm_popup_watch = 1;
}

void dscm_popup_status(dss_display_status_t* out)
{
  g_stats.mode = g_mode;
  g_stats.irq_passed = dscm_wrap_passes;
  g_stats.irq_woke = dscm_wrap_woke;
  g_stats.pass_on = (uint8_t)(pass_allowed() && (g_disp.variant & (DISP_SDK3 | DISP_SDK2)));
  g_stats.dispatcher = g_disp.variant;
  g_stats.line = (uint8_t)(g_line ? 1 : g_line_off ? 2 : 0);
  g_stats.line_stage = (uint8_t)line_stage();
  g_stats.dispoff = g_dispoff.state;
  *out = g_stats;
}
