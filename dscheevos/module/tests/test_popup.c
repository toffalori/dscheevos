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
static uint8_t g_header[0x200];              /* the game's header: no FNT/FAT */
#define POPUP_NDS_HEADER ((uintptr_t)g_header)
/* the game's ITCM (its IRQ dispatcher) and main RAM (its threads) */
static uint32_t g_itcm[0x2000 / 4];
static uint8_t g_ram[0x00800000];
static uintptr_t host_code(uint32_t a)
{
  static uint32_t junk;
  if (a - 0x01FF8000u < sizeof(g_itcm))
    return (uintptr_t)g_itcm + (a - 0x01FF8000u);
  if (a - 0x02000000u < sizeof(g_ram))
    return (uintptr_t)(g_ram + (a - 0x02000000u));
  printf("FAIL: game read at %08x\n", a);
  exit(1);
  return (uintptr_t)&junk;
}
#define POPUP_CODE(a) host_code(a)

/* start.s stand-ins */
void dscm_irq_wrapper(void) {}
volatile uint32_t dscm_wrap_orig, dscm_wrap_hits, dscm_wrap_line, dscm_wrap_irqs;
volatile uint32_t dscm_wrap_pass, dscm_wrap_passes, dscm_wrap_woke, dscm_wrap_open;
volatile uint32_t dscm_wrap_table, dscm_wrap_switch, dscm_wrap_info, dscm_wrap_wake;
uint32_t dscm_cp15_control(void) { return 1u << 13 | 1u << 16; }   /* high vectors, DTCM on */
uint32_t dscm_dtcm_base(void) { return 0x027C0000u; }
uint32_t dscm_irq_off(void) { return 0; }
void dscm_irq_restore(uint32_t cpsr) { (void)cpsr; }
void dscm_dc_clean(const void* addr, uint32_t len) { (void)addr; (void)len; }
void dscm_dc_invalidate(const void* addr, uint32_t len) { (void)addr; (void)len; }

/* stream.c stand-in: the states the phone would get */
static uint8_t g_sent_states[64];
static int g_sent_count;
static uint32_t g_code_sent[8];
static int g_code_count;
void dscm_stream_send(uint8_t type, const void* body, uint16_t len)
{
  const uint8_t* b = body;
  if (type == 0x06 && len == 52) {             /* a piece of the dispatcher's code */
    if (g_code_count < 8)
      memcpy(&g_code_sent[g_code_count++], b, 4);
    return;
  }
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

/* A NitroSDK V1 dispatcher at 0x01FF8000 (the census's signature, with our
 * own literals) and a ThreadSwitch shaped like SDK 3.x/4.x's after it
 * (OSi_IrqThreadQueue at T_QUEUE, OSi_ThreadInfo at T_INFO), or like SDK
 * 2.0's (its IRQ wake bits at T_WAKE, in the DTCM). */
#define T_QUEUE  0x02100000u
#define T_INFO   0x02100100u
#define T_WAKE   0x027C0100u
#define RAM32(a) (*(uint32_t*)(g_ram + ((a) - 0x02000000u)))
static void fake_dispatcher(int sdk2)
{
  uint32_t* sw = g_itcm + 0x58 / 4;
  uint32_t i;
  memset(g_itcm, 0, sizeof(g_itcm));
  memcpy(g_itcm, k_disp_v1, sizeof(k_disp_v1));
  g_itcm[20] = sdk2 ? 0x02099FE0u : 0x027C0000u;   /* OS_IRQTable: main RAM (SDK 2.0) or the DTCM's start */
  g_itcm[21] = 0x01FF8058u;    /* OS_IrqHandler_ThreadSwitch */
  if (sdk2) {
    memcpy(sw, k_switch2, sizeof(k_switch2));
    sw[0] |= 0x100 - 0 - 8;    /* ldr ip, =wake bits, at sw + 0x100 */
    sw[6] |= 0x104 - 6 * 4 - 8;    /* ldr ip, =info twice, at sw + 0x104 */
    sw[18] |= 0x104 - 18 * 4 - 8;
    sw[0x100 / 4] = T_WAKE;
  } else {
    memcpy(sw, k_switch3, sizeof(k_switch3));
    sw[6] |= 0x64;             /* str r2, [ip, #0x64]: state */
    sw[7] |= 0x78;             /* the queue field, link.prev, link.next */
    sw[8] |= 0x7C;
    sw[9] |= 0x80;
    sw[10] |= 0x80;
    for (i = 0; i < 23; i++)   /* ldr ip, =queue / =info: literals at sw + 0x100 and 0x104 */
      if (i == 0 || i == 14 || i == 17 || i == 20)
        sw[i] |= (i == 0 || i == 14 ? 0x100 : 0x104) - i * 4 - 8;
    sw[0x100 / 4] = T_QUEUE;
  }
  sw[0x104 / 4] = T_INFO;
  RAM32(g_itcm[20] + 18 * 4) = 0x01FF8200u;   /* OS_IRQTable[18]: a handler in ITCM */
}

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

/* One frame: the game idles at line 154, the band is armed, the VBlank enters the wrapper. */
static void frame(void)
{
  REG16(0x04000006) = 154;   /* idle a few lines before a toast band: armed at once */
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

/* The VBlank ends an armed band (the wrapper's part), and the next idle pass sees it. */
static void end_band(void)
{
  if (g_vector == WRAPPER) {
    g_vector = dscm_wrap_orig;
    dscm_wrap_hits++;
    dscm_wrap_line = 192;
    NDMA_CNT(2) = 0;
    NDMA_CNT(3) = 0;
  }
  dscm_popup_idle(++t_frame);
}

/* One frame the game idles only at line vc: too late for the band, or an IRQ is due. */
static void idle_only(uint16_t vc)
{
  REG16(0x04000006) = vc;
  dscm_popup_idle(++t_frame);
  dscm_popup_rearm();
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
  DISPSTAT = 0x08;             /* VBlank IRQ on */
  POWCNT1 = 0x8203;            /* engine A on top */
  g_vector = 0x01FF8000u;      /* the game's handler, in ITCM */
  fake_dispatcher(0);
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
  /* the dispatcher was recognised, its addresses handed to the wrapper, its code sent once */
  if (g_disp.variant != (DISP_V1 | DISP_SDK3) || dscm_wrap_pass != 1 || dscm_wrap_table != 0x027C0000u
      || dscm_wrap_switch != 0x01FF8058u || dscm_wrap_info != T_INFO || dscm_wrap_wake != 0 || g_code_count != 6
      || g_code_sent[0] != 0x01FF8000u || g_code_sent[1] != 0x01FF8030u || g_code_sent[2] != 0x01FF8058u) {
    printf("FAIL: dispatcher: variant %02x pass %u table %08x switch %08x info %08x wake %08x, %d code chunks\n",
           g_disp.variant, dscm_wrap_pass, dscm_wrap_table, dscm_wrap_switch, dscm_wrap_info, dscm_wrap_wake, g_code_count);
    return 1;
  }

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

  /* telemetry: every frame so far had the band; then frames the game was
   * busy into the band, idle only in VBlank, idle in time with an IRQ due,
   * and never idle */
  if (g_stats.shown_bits != 0xFFFFFFFFu || g_stats.wake_early || g_stats.wake_late || g_stats.wake_vblank || g_stats.wake_none) {
    printf("FAIL: telemetry after clean runs: bits %08x early %u late %u vblank %u none %u\n", g_stats.shown_bits,
           g_stats.wake_early, g_stats.wake_late, g_stats.wake_vblank, g_stats.wake_none);
    return 1;
  }
  dscm_popup_on_popup(g_pkt, popup_packet(9, DSS_POPUP_TEST, DSS_POPUP_SMALL, 0, 0, "", "x", "", ""));
  while (g_cur.phase != P_HOLD)
    frame();
  for (i = 0; i < 3; i++)
    idle_only(189);
  for (i = 0; i < 2; i++)
    idle_only(200);
  REG_IF = 1;
  idle_only(170);
  REG_IF = 0;
  t_frame += 2;
  frame();
  frame();
  if (g_stats.wake_late != 3 || g_stats.wake_vblank != 2 || g_stats.wake_early != 1 || g_stats.wake_none != 2
      || (g_stats.shown_bits & 0x3FF) != 0x201) {
    printf("FAIL: telemetry: bits %08x early %u late %u vblank %u none %u\n", g_stats.shown_bits,
           g_stats.wake_early, g_stats.wake_late, g_stats.wake_vblank, g_stats.wake_none);
    return 1;
  }
  run(path, SMALL_ROWS, 0);
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

  /* the vector changed under an armed band (a soft reset): the band is stopped;
   * the third time, pop-ups stand down for the game */
  dscm_popup_on_popup(g_pkt, popup_packet(6, DSS_POPUP_TEST, DSS_POPUP_TOAST, 0, 0, "", "x", "", ""));
  REG16(0x04000006) = 154;
  dscm_popup_idle(++t_frame);
  for (i = 0; i < VECTOR_LOST_MAX; i++) {
    dscm_popup_idle(++t_frame);
    if (g_vector != WRAPPER) { printf("FAIL: not armed (%d)\n", i); return 1; }
    g_vector = 0x01FF9000u;    /* the game set another handler */
    dscm_popup_idle(++t_frame);
    if (g_stats.vector_lost != (uint32_t)i + 1) { printf("FAIL: vector loss not seen\n"); return 1; }
    if ((g_standdown == DSS_POPUP_WHY_VECTOR) != (i == VECTOR_LOST_MAX - 1)) { printf("FAIL: stand-down at %d\n", i); return 1; }
  }
  g_standdown = 0;
  g_sent_count = 0;
  g_cur.phase = P_FREE;

  /* the wrapper let an IPC interrupt through and the game's handler never came
   * back to it: the band is stopped, it isn't a vector loss, and no interrupt
   * passes again */
  {
    const uint32_t lost = g_stats.vector_lost;
    g_vector = 0x01FF8000u;    /* the dispatcher again, after the test above changed it */
    dscm_popup_on_popup(g_pkt, popup_packet(7, DSS_POPUP_TEST, DSS_POPUP_TOAST, 0, 0, "", "x", "", ""));
    REG16(0x04000006) = 154;
    for (i = 0; i < 20 && g_vector != WRAPPER; i++)
      dscm_popup_idle(++t_frame);
    if (g_vector != WRAPPER || dscm_wrap_pass != 1) { printf("FAIL: not armed to pass\n"); return 1; }
    g_vector = dscm_wrap_orig;
    dscm_wrap_open = 1;
    dscm_popup_idle(t_frame);
    if (!g_pass_off || g_stats.pass_lost != 1 || g_stats.vector_lost != lost || dscm_wrap_open || g_armed) {
      printf("FAIL: lost pass not handled\n");
      return 1;
    }
    for (i = 0; i < 20 && g_vector != WRAPPER; i++)
      dscm_popup_idle(++t_frame);
    if (g_vector != WRAPPER || dscm_wrap_pass != 0) { printf("FAIL: passing after a lost pass\n"); return 1; }
    g_cur.phase = P_FREE;
    g_sent_count = 0;
    g_pass_off = 0;
    end_band();
  }

  /* a handler that isn't NitroSDK's known dispatcher: no passing; one whose
   * ThreadSwitch differs: recognised, but no passing */
  g_itcm[3] ^= 1;
  if (dispatcher_known(0x01FF8000u) || g_disp.variant) { printf("FAIL: a changed dispatcher was recognised\n"); return 1; }
  g_itcm[3] ^= 1;
  g_itcm[0x58 / 4 + 15] ^= 4;
  if (dispatcher_known(0x01FF8000u) || g_disp.variant != DISP_V1) { printf("FAIL: a changed ThreadSwitch was taken\n"); return 1; }
  g_itcm[0x58 / 4 + 15] ^= 4;
  g_itcm[0x58 / 4 + 8] += 4;   /* link.prev not right before link.next */
  if (dispatcher_known(0x01FF8000u)) { printf("FAIL: odd OSThread offsets were taken\n"); return 1; }
  g_itcm[0x58 / 4 + 8] -= 4;
  g_itcm[0x58 / 4 + 20] += 4;  /* the two loads of OSi_ThreadInfo disagree */
  if (dispatcher_known(0x01FF8000u)) { printf("FAIL: disagreeing literals were taken\n"); return 1; }
  g_itcm[0x58 / 4 + 20] -= 4;
  if (!dispatcher_known(0x01FF8000u) || !pass_ready()) { printf("FAIL: the dispatcher isn't recognised again\n"); return 1; }
  RAM32(0x027C0000u + 18 * 4) = 0x04000000u;   /* the IPC handler not in code memory */
  if (pass_ready()) { printf("FAIL: an odd IPC handler taken\n"); return 1; }
  RAM32(0x027C0000u + 18 * 4) = 0x01FF8200u;

  /* NitroSDK 2.0 (Super Mario 64 DS): its table in main RAM, its ThreadSwitch
   * with IRQ wake bits; armed to pass with those handed to the wrapper */
  fake_dispatcher(1);
  if (!dispatcher_known(0x01FF8000u) || g_disp.variant != (DISP_V1 | DISP_SDK2) || g_disp.info != T_INFO
      || g_disp.wake != T_WAKE || g_disp.table != 0x02099FE0u || !pass_ready()) {
    printf("FAIL: SDK 2.0: variant %02x info %08x wake %08x table %08x\n", g_disp.variant, g_disp.info, g_disp.wake, g_disp.table);
    return 1;
  }
  dscm_popup_on_popup(g_pkt, popup_packet(8, DSS_POPUP_TEST, DSS_POPUP_TOAST, 0, 0, "", "x", "", ""));
  REG16(0x04000006) = 154;
  for (i = 0; i < 20 && g_vector != WRAPPER; i++)
    dscm_popup_idle(++t_frame);
  if (g_vector != WRAPPER || dscm_wrap_pass != 1 || dscm_wrap_wake != T_WAKE || dscm_wrap_info != T_INFO
      || dscm_wrap_table != 0x02099FE0u || g_stats.dispatcher != 0) {
    /* (the status's copy is taken at dscm_popup_status) */
    if (!(g_vector == WRAPPER && dscm_wrap_pass == 1 && dscm_wrap_wake == T_WAKE)) {
      printf("FAIL: SDK 2.0 not armed to pass (pass %u, wake %08x)\n", dscm_wrap_pass, dscm_wrap_wake);
      return 1;
    }
  }
  g_cur.phase = P_FREE;
  g_sent_count = 0;
  end_band();
  fake_dispatcher(0);

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
