/*
 * Host test of the streamer's recording (src/record.c): random word changes
 * over many frames, a table change and frame-count gaps are recorded into a
 * small ring; a decoder written from the format in ds_stream_protocol.h then
 * replays from several starting frames, as the phone does, and every frame it
 * evaluates must see exactly the values that were recorded for that frame.
 *
 *   gcc -O1 -g -Wall -DDSCM_STREAM -I../include test_record.c ../src/record.c -o build/test_record.exe
 */
#include "dscheevos_module.h"
#include "ds_stream_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dscm_dc_clean(const void* addr, uint32_t len) { (void)addr; (void)len; }

void dscm_rec_init(void* ring, uint32_t size, uint16_t run, const char* hash);
void dscm_rec_frame(uint32_t frame, const uint32_t* addr, uint16_t count, uint16_t generation, const uint8_t* cur);
uint32_t dscm_rec_written(void);
uint32_t dscm_rec_end_frame(void);
uint32_t dscm_rec_find(uint32_t from_frame, int* gap);
uint32_t dscm_rec_read(uint32_t abs_offset, uint8_t* out, uint32_t len);
dscm_rec_t* dscm_rec_header(void);
void dscm_rec_note(uint32_t line, const char* text);

#define FRAMES 6000
#define WORDS  40

static uint8_t g_ring[16 * 1024];
static uint32_t g_frame_of[FRAMES];                     /* frame number of record i */
static uint8_t g_vals[FRAMES][WORDS * 4];               /* the words as recorded for it */
static uint16_t g_count_of[FRAMES];
static uint32_t g_addr_of[FRAMES][WORDS];

static uint32_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Replays the stream from `start` like the phone: segments set the table and
 * values, frames from from_frame on are checked. Returns frames checked, -1
 * on a mismatch. */
static int replay(uint32_t start, uint32_t from_frame, int first_record_after)
{
  static uint8_t buf[64 * 1024];
  uint32_t len = dscm_rec_read(start, buf, sizeof(buf)), at = 0, frame = 0, count = 0, addr[WORDS];
  uint8_t vals[WORDS * 4];
  int checked = 0, rec = first_record_after, have = 0;
  while (at < len) {
    const uint8_t t = buf[at];
    if (t == DSS_REC_PAD) {
      at++;
      continue;
    }
    if (len - at >= 4 && rd32(buf + at) == DSS_REC_MAGIC) {
      uint32_t i;
      count = rd16(buf + at + 4);
      frame = rd32(buf + at + 8);
      for (i = 0; i < count; i++)
        addr[i] = rd32(buf + at + 12 + i * 4);
      memcpy(vals, buf + at + 12 + count * 4, count * 4);
      at += 12 + count * 8;
      have = 1;
      continue;
    }
    if (!have) {
      printf("FAIL: a frame before any segment at %u\n", (unsigned)at);
      return -1;
    }
    {
      uint32_t n, adv, i;
      if (t == DSS_REC_FRAME16) {
        n = rd16(buf + at + 1);
        adv = rd16(buf + at + 3);
        at += 5;
      } else {
        n = t;
        adv = buf[at + 1];
        at += 2;
      }
      for (i = 0; i < n; i++) {
        memcpy(vals + rd16(buf + at) * 4, buf + at + 2, 4);
        at += 6;
      }
      frame += adv;
    }
    /* one evaluated frame: find its record by frame number */
    while (rec < FRAMES && g_frame_of[rec] < frame)
      rec++;
    if (frame >= from_frame) {
      if (rec >= FRAMES || g_frame_of[rec] != frame) {
        printf("FAIL: replayed frame %u was never recorded\n", (unsigned)frame);
        return -1;
      }
      if (g_count_of[rec] != count || memcmp(g_addr_of[rec], addr, count * 4)
          || (memcmp(g_vals[rec], vals, count * 4) && !(rec && g_count_of[rec - 1] != count))) {
        printf("FAIL: frame %u values differ\n", (unsigned)frame);
        return -1;
      }
      checked++;
    }
  }
  return checked;
}

int main(void)
{
  uint32_t addr[WORDS], frame = 1000, i, k;
  uint8_t cur[WORDS * 4];
  uint16_t count = 30, gen = 1;
  int gap, n;
  srand(7);
  for (i = 0; i < WORDS; i++)
    addr[i] = 0x02100000u + i * 4;
  memset(cur, 0, sizeof(cur));
  dscm_rec_init(g_ring, sizeof(g_ring), 0x1234, "ba3c4052e00c5cc31df5d5534c39de1b");
  for (k = 0; k < FRAMES; k++) {
    /* 30 fps stretches, a long pause, a table change half-way */
    frame += (k % 700 == 699) ? 400 : (k % 3 == 0 ? 2 : 1);
    if (k == FRAMES / 2) {
      count = WORDS;
      gen = 2;
      for (i = 0; i < WORDS; i++)
        addr[i] = 0x02200000u + i * 8;
    }
    for (i = 0; i < 3; i++) {
      const uint32_t w = (uint32_t)rand() % count, v = (uint32_t)rand();
      if (rand() % 4 == 0)
        memcpy(cur + w * 4, &v, 4);
    }
    if (k % 997 == 5)                  /* a burst: every word changes */
      for (i = 0; i < count; i++)
        cur[i * 4] ^= 0x5A;
    dscm_rec_frame(frame, addr, count, gen, cur);
    g_frame_of[k] = frame;
    memcpy(g_vals[k], cur, sizeof(cur));
    g_count_of[k] = count;
    memcpy(g_addr_of[k], addr, sizeof(addr));
  }
  printf("recorded %d frames, %u bytes (ring %u), end frame %u\n", FRAMES, (unsigned)dscm_rec_written(),
         (unsigned)sizeof(g_ring), (unsigned)dscm_rec_end_frame());
  if (dscm_rec_end_frame() != frame) { printf("FAIL: end frame\n"); return 1; }

  /* replay from frames near the end: the segment found must cover them, no gap */
  for (k = FRAMES - 1; k > FRAMES - 900; k -= 137) {
    const uint32_t from = g_frame_of[k];
    const uint32_t start = dscm_rec_find(from, &gap);
    if (start == (uint32_t)-1 || gap) { printf("FAIL: find %u: start %u gap %d\n", (unsigned)from, (unsigned)start, gap); return 1; }
    n = replay(start, from, 0);
    if (n != FRAMES - (int)k) { printf("FAIL: from frame %u: %d frames checked, want %d\n", (unsigned)from, n, FRAMES - (int)k); return 1; }
  }
  /* from a frame long overwritten: the oldest segment, with a gap */
  {
    const uint32_t start = dscm_rec_find(g_frame_of[10], &gap);
    if (start == (uint32_t)-1 || !gap) { printf("FAIL: an overwritten start should report a gap\n"); return 1; }
    n = replay(start, g_frame_of[10], 0);
    if (n <= 0) { printf("FAIL: replay after a gap\n"); return 1; }
    printf("gap replay: %d frames checked from the oldest segment\n", n);
  }
  /* from_frame past the end: nothing to evaluate */
  {
    const uint32_t start = dscm_rec_find(frame + 1, &gap);
    if (start == (uint32_t)-1 || gap || replay(start, frame + 1, 0) != 0) { printf("FAIL: up to date\n"); return 1; }
  }
  printf("test_record: ok\n");
  return 0;
}
