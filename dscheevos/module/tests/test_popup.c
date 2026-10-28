/* Host test of src/popup.c (the Cheevotron streamer's pop-ups on the DSi's
 * screen): the console's registers and IRQ vector are plain memory here.
 *  - a toast and a small pop-up go through their whole life (queued, shown,
 *    done) with the wrapper's part simulated at each VBlank, and the bands are
 *    written as PPM images to look at;
 *  - the packet parsers get random, truncated and mutated packets.
 * usage: test_popup <nds-bootstrap fonts dir> <out dir> */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_io[0x5000];
static volatile uint32_t g_vector;
#define POPUP_IO(a) ((uintptr_t)(g_io + ((a) - 0x04000000u)))
#define POPUP_HOST_VECTOR (&g_vector)

/* start.s stand-ins */
void dscm_irq_wrapper(void) {}
volatile uint32_t dscm_wrap_orig, dscm_wrap_hits, dscm_wrap_line, dscm_wrap_irqs;
uint32_t dscm_cp15_control(void) { return 1u << 13; }
uint32_t dscm_dtcm_base(void) { return 0x027C0000u; }
uint32_t dscm_irq_off(void) { return 0; }
void dscm_irq_restore(uint32_t cpsr) { (void)cpsr; }
void dscm_dc_clean(const void* addr, uint32_t len) { (void)addr; (void)len; }

/* stream.c stand-in: the states the phone would get */
static uint8_t g_sent_states[64];
static int g_sent_count;
void dscm_stream_send(uint8_t type, const void* body, uint16_t len)
{
  const uint8_t* b = body;
  if (type != 0x05 || len != 12) {
    printf("FAIL: sent type %02x len %u\n", type, len);
    exit(1);
  }
  if (g_sent_count < 64)
    g_sent_states[g_sent_count++] = b[2];
}

/* the font, from nds-bootstrap's in-game menu files (fontgen.py's job; ASCII code points only) */
static uint8_t g_glyphs[2048];
static uint16_t g_codepoints[256];
#include "dsd.h"
const dsd_font_t dsd_font_latin = { g_glyphs, g_codepoints };

#include "../src/popup.c"

static int lz10(const char* path, uint8_t* out, int max)
{
  FILE* f = fopen(path, "rb");
  static uint8_t in[65536];
  int n, size, i = 4, o = 0;
  if (!f) { printf("FAIL: %s\n", path); exit(1); }
  n = (int)fread(in, 1, sizeof(in), f);
  fclose(f);
  if (n < 4 || in[0] != 0x10) { printf("FAIL: %s not LZ10\n", path); exit(1); }
  size = in[1] | in[2] << 8 | in[3] << 16;
  while (o < size && i < n) {
    int flags = in[i++], bit;
    for (bit = 0; bit < 8 && o < size; bit++) {
      if (flags & (0x80 >> bit)) {
        int len = (in[i] >> 4) + 3, disp = ((in[i] & 15) << 8 | in[i + 1]) + 1, k;
        i += 2;
        for (k = 0; k < len && o < size; k++, o++)
          if (o < max) out[o] = o - disp >= 0 && o - disp < max ? out[o - disp] : 0;
      } else {
        if (o < max) out[o] = in[i];
        i++, o++;
      }
    }
  }
  return o;
}

static void ppm(const char* path, int rows)
{
  FILE* f = fopen(path, "wb");
  int i;
  fprintf(f, "P6\n256 %d\n255\n", rows);
  for (i = 0; i < 256 * rows; i++) {
    const uint16_t c = g_band[i];
    const uint8_t rgb[3] = { (uint8_t)((c & 31) * 255 / 31), (uint8_t)(((c >> 5) & 31) * 255 / 31), (uint8_t)(((c >> 10) & 31) * 255 / 31) };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

static uint8_t g_pkt[1400];

static uint16_t popup_packet(uint16_t id, uint8_t kind, uint8_t style, uint8_t pos, uint16_t badge,
                             const char* label, const char* title, const char* detail, const char* value)
{
  const char* s[4] = { label, title, detail, value };
  uint16_t at = 12;
  int k;
  memset(g_pkt, 0, 12);
  g_pkt[0] = (uint8_t)id; g_pkt[1] = (uint8_t)(id >> 8);
  g_pkt[2] = kind; g_pkt[3] = style; g_pkt[4] = pos;
  g_pkt[8] = (uint8_t)badge; g_pkt[9] = (uint8_t)(badge >> 8);
  for (k = 0; k < 4; k++) {
    const size_t n = strlen(s[k]);
    g_pkt[at++] = (uint8_t)n;
    memcpy(g_pkt + at, s[k], n);
    at = (uint16_t)(at + n);
  }
  return at;
}

static void badge_packets(uint16_t id, int side)
{
  static uint16_t px[32 * 32];
  const int total = side * side * 2;
  int x, y, off;
  for (y = 0; y < side; y++)
    for (x = 0; x < side; x++)   /* a gold ring on violet */
      px[y * side + x] = (uint16_t)(((x - side / 2) * (x - side / 2) + (y - side / 2) * (y - side / 2) < side * side / 5
                                     && (x - side / 2) * (x - side / 2) + (y - side / 2) * (y - side / 2) > side * side / 12)
                                    ? (31 | 25 << 5 | 6 << 10) : (12 | 6 << 5 | 20 << 10));
  for (off = 0; off < total; off += 1024) {
    const int n = total - off < 1024 ? total - off : 1024;
    g_pkt[0] = (uint8_t)id; g_pkt[1] = (uint8_t)(id >> 8);
    g_pkt[2] = (uint8_t)side; g_pkt[3] = 0;
    g_pkt[4] = (uint8_t)off; g_pkt[5] = (uint8_t)(off >> 8);
    g_pkt[6] = (uint8_t)total; g_pkt[7] = (uint8_t)(total >> 8);
    memcpy(g_pkt + 8, (uint8_t*)px + off, (size_t)n);
    dscm_popup_on_badge(g_pkt, (uint16_t)(8 + n));
  }
}

static uint32_t t_frame = 1000;

/* One frame: the game idles at line 100, the band is armed, the VBlank enters the wrapper. */
static void frame(void)
{
  REG16(0x04000006) = 100;
  dscm_popup_idle(++t_frame);
  dscm_popup_rearm();
  if (g_vector == WRAPPER) {
    g_vector = dscm_wrap_orig;
    dscm_wrap_hits++;
    dscm_wrap_line = 192;
    NDMA_CNT(2) = 0;
    NDMA_CNT(3) = 0;
  }
}

/* Runs a pop-up to its end; returns the frames it took, writes its band once it holds. */
static int run(const char* image, int rows, int dump_mode)
{
  int f;
  for (f = 0; f < 2000; f++) {
    frame();
    if (dump_mode && g_cur.phase == P_HOLD && g_cur.step == 10)
      ppm(image, rows + MARGIN);
    if (g_cur.phase == P_FREE && !g_queue_count)
      return f;
  }
  printf("FAIL: pop-up never ended: phase %d step %d state %d, arms %u shown %u early %u, skips late %u pending %u display %u dma %u irq %u vector %u screen %u\n",
         g_cur.phase, g_cur.step, g_cur.st.state, g_stats.arms, g_stats.frames_shown, g_stats.early, g_stats.skip_late,
         g_stats.skip_pending, g_stats.skip_display, g_stats.skip_dma, g_stats.skip_irq, g_stats.skip_vector, g_stats.skip_screen);
  exit(1);
}

static void expect_states(const uint8_t* want, int n)
{
  int i;
  if (g_sent_count != n) { printf("FAIL: %d states sent, want %d\n", g_sent_count, n); exit(1); }
  for (i = 0; i < n; i++)
    if (g_sent_states[i] != want[i]) { printf("FAIL: state %d is %d, want %d\n", i, g_sent_states[i], want[i]); exit(1); }
  g_sent_count = 0;
}

int main(int argc, char** argv)
{
  char path[512];
  int i, frames;
  const uint8_t life[3] = { DSS_POPUP_QUEUED, DSS_POPUP_SHOWN, DSS_POPUP_DONE };
  if (argc < 3) { printf("usage: test_popup <fonts dir> <out dir>\n"); return 2; }
  snprintf(path, sizeof(path), "%s/ascii.lz77", argv[1]);
  lz10(path, g_glyphs, 1024);
  snprintf(path, sizeof(path), "%s/extended_latin.lz77", argv[1]);
  lz10(path, g_glyphs + 1024, 1024);
  for (i = 0x20; i < 0x7F; i++)
    g_codepoints[i] = (uint16_t)i;

  dscm_popup_init();
  dscm_popup_set_mode(DSS_DISPLAY_ON);
  DISPCNT = 0x10108u;          /* mode 1, BG0 3D, BG0 on */
  REG_IE = 1;                  /* VBlank */
  POWCNT1 = 0x8203;            /* engine A on top */
  g_vector = 0x01FF8000u;      /* the game's handler, in ITCM */
  for (i = 0; i < 40; i++)
    frame();                   /* the screen has stayed put */

  /* a toast with its badge, then a small one on the right with no badge */
  badge_packets(1, 32);
  dscm_popup_on_popup(g_pkt, popup_packet(1, DSS_POPUP_UNLOCK, DSS_POPUP_TOAST, 0, 1, "",
                                          "The Power Star in the Castle", "Collect the first Power Star of Bob-omb Battlefield", "10 pts"));
  snprintf(path, sizeof(path), "%s/toast.ppm", argv[2]);
  frames = run(path, TOAST_ROWS, 1);
  expect_states(life, 3);
  printf("toast: %d frames, %u arms, %u shown\n", frames, g_stats.arms, g_stats.frames_shown);

  dscm_popup_on_popup(g_pkt, popup_packet(2, DSS_POPUP_UNLOCK, DSS_POPUP_SMALL, DSS_POPUP_POS_RIGHT, 0, "",
                                          "Bowser's Sub, a very long title that will not fit", "", "25 pts"));
  snprintf(path, sizeof(path), "%s/small.ppm", argv[2]);
  run(path, SMALL_ROWS, 1);
  expect_states(life, 3);

  badge_packets(3, 32);
  dscm_popup_on_popup(g_pkt, popup_packet(3, DSS_POPUP_STATUS, DSS_POPUP_TOAST, DSS_POPUP_POS_RIGHT, 3, "Cheevotron",
                                          "Super Mario 64 DS", "Ready! 12 of 45 unlocked", ""));
  snprintf(path, sizeof(path), "%s/ready.ppm", argv[2]);
  run(path, TOAST_ROWS, 1);
  expect_states(life, 3);

  /* a repeat of a finished one gets its last state again, and isn't shown again */
  dscm_popup_on_popup(g_pkt, popup_packet(1, DSS_POPUP_UNLOCK, DSS_POPUP_TOAST, 0, 0, "", "x", "", ""));
  expect_states(&life[2], 1);

  /* off: refused at once */
  dscm_popup_set_mode(DSS_DISPLAY_OFF);
  dscm_popup_on_popup(g_pkt, popup_packet(4, DSS_POPUP_TEST, DSS_POPUP_TOAST, 0, 0, "", "x", "", ""));
  { const uint8_t r[1] = { DSS_POPUP_REFUSED }; expect_states(r, 1); }
  dscm_popup_set_mode(DSS_DISPLAY_ON);

  /* a game that never leaves time: dropped after 10 s */
  dscm_popup_on_popup(g_pkt, popup_packet(5, DSS_POPUP_TEST, DSS_POPUP_TOAST, 0, 0, "", "x", "", ""));
  for (i = 0; i < 700; i++) {
    REG16(0x04000006) = 170;   /* too late for the band every frame */
    dscm_popup_idle(++t_frame);
  }
  { const uint8_t r[2] = { DSS_POPUP_QUEUED, DSS_POPUP_DROPPED }; expect_states(r, 2); }

  /* the vector changed under an armed band: stood down */
  dscm_popup_on_popup(g_pkt, popup_packet(6, DSS_POPUP_TEST, DSS_POPUP_TOAST, 0, 0, "", "x", "", ""));
  REG16(0x04000006) = 100;
  dscm_popup_idle(++t_frame);
  dscm_popup_idle(++t_frame);
  if (g_vector != WRAPPER) { printf("FAIL: not armed\n"); return 1; }
  g_vector = 0x01FF9000u;      /* the game set another handler */
  dscm_popup_idle(++t_frame);
  if (g_standdown != DSS_POPUP_WHY_VECTOR || NDMA_CNT(3)) { printf("FAIL: no stand-down\n"); return 1; }
  g_standdown = 0;
  g_sent_count = 0;
  g_cur.phase = P_FREE;

  /* fuzz: random, truncated and mutated packets must never write out of bounds */
  srand(1);
  for (i = 0; i < 300000; i++) {
    uint16_t n = (uint16_t)(rand() % 400), k;
    if (i % 3 == 0) {
      n = popup_packet((uint16_t)(rand() & 0xFFFF), (uint8_t)rand(), (uint8_t)rand(), (uint8_t)rand(), (uint16_t)rand(),
                       "label", "title", "detail", "value");
      for (k = 0; k < 4; k++)
        g_pkt[rand() % n] = (uint8_t)rand();
      n = (uint16_t)(rand() % (n + 1));
    } else {
      for (k = 0; k < n; k++)
        g_pkt[k] = (uint8_t)rand();
    }
    if (i & 1)
      dscm_popup_on_popup(g_pkt, n);
    else
      dscm_popup_on_badge(g_pkt, n);
    if (i % 64 == 0)
      frame();
    g_sent_count = 0;
  }
  printf("fuzz: ok (%u arms, %u shown in all)\n", g_stats.arms, g_stats.frames_shown);
  printf("test_popup: ok\n");
  return 0;
}
