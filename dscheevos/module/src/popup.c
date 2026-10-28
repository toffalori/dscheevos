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
 * vector, and only then runs the game's handler. So the game never sees mode
 * 3 and never loses a DISPCNT write. A band an IRQ ends early is re-armed at
 * the next idle halt if there is still time in the frame.
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
extern uint32_t dscm_cp15_control(void);
extern uint32_t dscm_dtcm_base(void);
extern uint32_t dscm_irq_off(void);
extern void dscm_irq_restore(uint32_t cpsr);
extern void dscm_dc_clean(const void* addr, uint32_t len);

/* The host test (tests/popup) puts the registers and the IRQ vector in plain
 * memory; on the console these are the addresses themselves. */
#ifndef POPUP_IO
#define POPUP_IO(a) (a)
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
#define SCREEN_STABLE  30                      /* frames engine A stays on one screen before a band */

/* ---- the band ---- */

#define TOAST_ROWS 32
#define SMALL_ROWS 16
#define MARGIN     4                           /* rows fed past the band: the FIFO reads ahead */
#define SLIDE      4                           /* frames to slide in, and out */
#define HOLD_DEFAULT 180
#define WAIT_MAX   600                         /* frames with no chance to show: dropped */
#define BADGE_WAIT 30                          /* frames a pop-up waits for its badge's chunks */

static uint16_t g_band[256 * (TOAST_ROWS + MARGIN)] __attribute__((aligned(32)));
static uint32_t g_table[192] __attribute__((aligned(32)));   /* DISPCNT per line */
static uint32_t g_table_base;                  /* what g_table holds: the game's DISPCNT, */
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

static uint8_t  g_mode;                        /* DSS_DISPLAY_*, from the phone (off until it says) */
static uint8_t  g_standdown;                   /* DSS_POPUP_WHY_*: off for the rest of the game */
static uint8_t  g_armed;                       /* the wrapper's entry for the last arming is due */
static uint32_t g_armed_hits;                  /* dscm_wrap_hits at that arming */
static uint16_t g_armed_first;                 /* its band's first line */
static uint32_t g_shown_seen, g_cut_seen;      /* counts at the last frame tick */
static uint32_t g_frame;                       /* the VBlank count at the last tick */
static uint16_t g_screen_frames;               /* frames engine A stayed on the same screen */
static uint16_t g_screen_last;
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

  if (g_standdown) {
    refuse(p.st.popup_id, g_standdown);
    return;
  }
  if (g_mode == DSS_DISPLAY_OFF) {
    refuse(p.st.popup_id, DSS_POPUP_WHY_OFF);
    return;
  }
  if (g_queue_count >= QUEUE_MAX) {
    refuse(p.st.popup_id, DSS_POPUP_WHY_BUSY);
    return;
  }
  p.phase = P_QUEUED;
  p.st.edge = 0;       /* the bottom edge only, for now */
  g_queue[g_queue_count] = p;
  set_state(&g_queue[g_queue_count], DSS_POPUP_QUEUED, DSS_POPUP_WHY_NONE);
  g_queue_count++;
  return;
bad:
  refuse(p.st.popup_id, DSS_POPUP_WHY_BAD);
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
  for (x = 0; x < 256; x++)
    g_band[(rows - 1) * 256 + x] = C_EDGE;
  for (y = 0; y < rows; y++)                   /* accent bar on the badge's side */
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
      return;                                  /* no IRQ yet */
    }
    /* The vector was changed while we were on it: nothing will stop the
     * band, so stop it here, and show nothing more in this game. */
    stop_channels();
    dscm_irq_restore(cpsr);
    g_armed = 0;
    g_standdown = DSS_POPUP_WHY_VECTOR;
    g_stats.skip_vector++;
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

/* Arm the band for this frame, from the idle thread. IRQs masked throughout:
 * the wrapper goes on the vector last, then the caller halts. */
static void arm(void)
{
  const int rows = visible_rows();
  const uint32_t first = (uint32_t)(LAST_LINE + 1 - rows);
  volatile uint32_t* vec;
  uint32_t cpsr, vc, d, cur, l;

  if (rows <= 0 || g_armed || g_standdown || g_mode == DSS_DISPLAY_OFF)
    return;
  cpsr = dscm_irq_off();
  vc = VCOUNT;
  if (vc >= 192 || vc + 2 > first) {
    g_stats.skip_late++;
    goto out;
  }
  if (REG_IE & REG_IF) {
    g_stats.skip_pending++;
    goto out;
  }
  /* the wrapper must run at the VBlank at the latest; no IRQ on every line */
  if (!(REG_IE & 1) || ((REG_IE & 2) && (DISPSTAT & 0x10))) {
    g_stats.skip_irq++;
    goto out;
  }
  d = DISPCNT;
  if ((d & MODE_MASK) != MODE_1 || (d & 0x80) || ((MASTER_BRIGHT >> 14) && (MASTER_BRIGHT & 31))
      || (DISPCAPCNT & 0x80000000u)) {
    g_stats.skip_display++;
    goto out;
  }
  if (dma_busy()) {
    g_stats.skip_dma++;
    goto out;
  }
  if (g_screen_frames < SCREEN_STABLE) {
    g_stats.skip_screen++;
    goto out;
  }
  /* high vectors: the BIOS's IRQ entry reads [DTCM+0x3FFC] */
  vec = irq_vector();
  cur = *vec;
  if (!(dscm_cp15_control() & (1u << 13)) || cur == WRAPPER
      || (cur - 0x01000000u >= 0x01000000u && cur - 0x02000000u >= 0x01000000u)) {
    g_stats.skip_vector++;
    goto out;
  }

  if (g_mode >= DSS_DISPLAY_NOOP) {
    /* the per-line DISPCNT table: rebuilt only when the game's value, the
     * band's first line or the mode changed */
    if (g_table_mode != g_mode || g_table_base != d || g_table_first != first) {
      for (l = 0; l < 192; l++)
        g_table[l] = (g_mode == DSS_DISPLAY_ON && l >= first && l <= LAST_LINE) ? ((d & ~MODE_MASK) | MODE_3) : d;
      dscm_dc_clean(g_table, sizeof(g_table));
      g_table_mode = g_mode;
      g_table_base = d;
      g_table_first = (uint16_t)first;
    }
    /* NDMA3: from the H-Blank of this line, the next line's word each H-Blank */
    NDMA_CNT(CH_DISPCNT) = 0;
    NDMA_SAD(CH_DISPCNT) = DSCM_PHYS(&g_table[vc + 1]);
    NDMA_DAD(CH_DISPCNT) = 0x04000000u;
    NDMA_TCNT(CH_DISPCNT) = 191 - vc;
    NDMA_WCNT(CH_DISPCNT) = 1;
    NDMA_BCNT(CH_DISPCNT) = 0;
    NDMA_CNT(CH_DISPCNT) = NDMA_ON | NDMA_START(0x07) | NDMA_DFIX | NDMA_PHYS(0);
    if (g_mode == DSS_DISPLAY_ON) {
      /* NDMA2: the band's rows (and the margin) into the display FIFO; mode 1
       * lines don't drain it, so it is ready when the band's first line comes */
      NDMA_CNT(CH_FIFO) = 0;
      NDMA_SAD(CH_FIFO) = DSCM_PHYS(g_band);
      NDMA_DAD(CH_FIFO) = MMEM_FIFO;
      NDMA_TCNT(CH_FIFO) = (uint32_t)(rows + MARGIN) * 128;
      NDMA_WCNT(CH_FIFO) = 4;
      NDMA_BCNT(CH_FIFO) = 0;
      NDMA_CNT(CH_FIFO) = NDMA_ON | NDMA_START(0x09) | NDMA_PHYS(2) | NDMA_DFIX;
    }
    if (VCOUNT != vc) {                        /* a line went by: the table would be a line off */
      stop_channels();
      g_stats.skip_late++;
      goto out;
    }
  }
  dscm_wrap_orig = cur;
  g_armed_hits = dscm_wrap_hits;
  g_armed_first = (uint16_t)first;
  g_armed = 1;
  *vec = WRAPPER;
  g_stats.arms++;
out:
  dscm_irq_restore(cpsr);
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

/* Once per new frame: what the band did since the last tick moves the pop-up on. */
static void tick(uint32_t frame)
{
  const uint32_t passed = frame - g_frame;
  const uint16_t screen = POWCNT1 & POWCNT1_TOP;
  int showed;
  g_frame = frame;
  if (screen == g_screen_last) {
    const uint32_t f = g_screen_frames + (passed < 0xFFFF ? passed : 0xFFFF);
    g_screen_frames = (uint16_t)(f < 0xFFFF ? f : 0xFFFF);
  } else {
    g_screen_last = screen;
    g_screen_frames = 0;
  }
  g_stats.screen = screen ? 1 : 0;
  g_stats.mode = g_mode;

  showed = g_stats.frames_shown != g_shown_seen || g_stats.frames_cut != g_cut_seen;
  if (g_stats.frames_cut != g_cut_seen)
    g_cur.st.frames_cut++;
  g_shown_seen = g_stats.frames_shown;
  g_cut_seen = g_stats.frames_cut;

  if (g_mode == DSS_DISPLAY_OFF || g_standdown) {
    /* everything waiting is refused */
    if (g_cur.phase != P_FREE)
      finish(&g_cur, DSS_POPUP_DROPPED, g_standdown ? g_standdown : DSS_POPUP_WHY_OFF);
    while (g_queue_count)
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
    if (g_armed)
      return;                                  /* the last pop-up's band is still armed */
    render();
    g_cur.phase = P_IN;
    g_cur.step = 0;
    g_cur.waited_run = 0;
    return;
  }

  if (!showed) {
    g_cur.st.frames_waited = (uint16_t)(g_cur.st.frames_waited + (passed < 0xFFFF ? passed : 1));
    g_cur.waited_run = (uint16_t)(g_cur.waited_run + (passed < 0xFFFF ? passed : 1));
    if (g_cur.waited_run >= WAIT_MAX)
      finish(&g_cur, g_cur.st.frames_shown ? DSS_POPUP_DONE : DSS_POPUP_DROPPED, DSS_POPUP_WHY_WAITED);
    return;
  }
  g_cur.waited_run = 0;
  if (g_cur.st.frames_shown < 0xFFFF)
    g_cur.st.frames_shown++;
  if (g_cur.st.state == DSS_POPUP_QUEUED)
    set_state(&g_cur, DSS_POPUP_SHOWN, DSS_POPUP_WHY_NONE);
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
}

void dscm_popup_set_mode(uint8_t mode)
{
  if (mode > DSS_DISPLAY_ON)
    return;
  g_mode = mode;
  g_table_mode = 0xFF;                         /* rebuild the table for the new mode */
}

/* Each idle pass, from module_idle with the lock held: the last arming's
 * result, the pop-up's frame tick when a VBlank came, then the band for this
 * frame. Cheap when nothing is up: a few compares. */
void dscm_popup_idle(uint32_t frame)
{
  collect();
  if (frame != g_frame)
    tick(frame);
  if (g_cur.phase >= P_IN && g_rendered)
    arm();
}

/* After the idle pass's own work (network, stream): arm again if an IRQ
 * ended the band meanwhile and there is still time in the frame. */
void dscm_popup_rearm(void)
{
  collect();
  if (g_cur.phase >= P_IN && g_rendered)
    arm();
}

void dscm_popup_status(dss_display_status_t* out)
{
  g_stats.mode = g_mode;
  *out = g_stats;
}
