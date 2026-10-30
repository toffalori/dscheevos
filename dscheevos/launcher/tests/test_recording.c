/*
 * Host test of the launcher's replay from the SD copy of a recording
 * (source/recording.c) and of the slot rules in dscm_record.h. The module's
 * own recorder (module/src/record.c) writes a stream of random word changes
 * into a ring; its completed sectors are copied into a slot's data sectors
 * the way the ARM7 does (sector k to data sector k % DSCM_REC_DATA_SECTORS,
 * the ring overtaking the copy for a while), the slot's header is filled as
 * the ARM7 fills it, and the replay is served through REPLAY_REQ /
 * REPLAY_DATA / REPLAY_DONE as the phone drives it. Every frame the decoded
 * replay evaluates must show the words as they were recorded for it.
 */
#include "recording.h"
#include "dscheevos_module.h"
#include "ds_stream_protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dscm_dc_clean(const void* addr, uint32_t len) { (void)addr; (void)len; }
void log_line(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

void dscm_rec_init(void* ring, uint32_t size, uint16_t run, const char* hash);
void dscm_rec_frame(uint32_t frame, const uint32_t* addr, uint16_t count, uint16_t generation, const uint8_t* cur);
uint32_t dscm_rec_written(void);
uint32_t dscm_rec_end_frame(void);
dscm_rec_t* dscm_rec_header(void);
uint32_t dscm_rec_read(uint32_t abs_offset, uint8_t* out, uint32_t len);

#define FRAMES 9000
#define WORDS  48
#define SECTOR DSS_REC_SECTOR

static uint8_t g_ring[24 * 1024];              /* smaller than the slot: the ring laps */
static uint32_t g_frame_of[FRAMES];
static uint8_t g_vals[FRAMES][WORDS * 4];
static uint16_t g_count_of[FRAMES];
static uint32_t g_addr_of[FRAMES][WORDS];
static int g_records;

static recording_t g_rec;
static uint8_t g_slot[DSCM_REC_DATA_SECTORS * SECTOR];
static uint32_t g_sd_written, g_valid_from, g_skipped;

static uint32_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int fail(const char* what)
{
  printf("FAIL: %s\n", what);
  return 1;
}

/* The ARM7's copying: every completed sector the ring still holds goes to
 * its data sector; after a spell without copying (the SD card was busy) the
 * ring has overtaken it, and sectors are skipped as the ARM7 skips them. */
static void mirror(void)
{
  const uint32_t written = dscm_rec_written();
  const uint32_t ring = sizeof(g_ring);
  while ((g_sd_written / SECTOR + 1) * SECTOR <= written) {
    uint32_t k = g_sd_written / SECTOR;
    if (written > k * SECTOR + ring) {
      const uint32_t oldest = (written - ring + SECTOR - 1) / SECTOR;
      g_skipped += oldest - k;
      g_sd_written = g_valid_from = oldest * SECTOR;
      k = oldest;
      if ((k + 1) * SECTOR > written)
        break;
    }
    if (dscm_rec_read(k * SECTOR, g_slot + (k % DSCM_REC_DATA_SECTORS) * SECTOR, SECTOR) != SECTOR) {
      printf("ring read failed at sector %u\n", (unsigned)k);
      exit(1);
    }
    g_sd_written = (k + 1) * SECTOR;
  }
}

static void fill_header(void)
{
  dscm_rec_t* h = dscm_rec_header();
  memset(&g_rec.h, 0, sizeof(g_rec.h));
  g_rec.h.magic = DSCM_RECSLOT_MAGIC;
  g_rec.h.size = sizeof(g_rec.h);
  g_rec.h.seq = 7;
  g_rec.h.run = h->run;
  g_rec.h.flags = DSCM_RECSLOT_CLAIMED;
  memcpy(g_rec.h.game_hash, h->game_hash, 32);
  g_rec.h.data_sectors = DSCM_REC_DATA_SECTORS;
  g_rec.h.written = g_sd_written;
  g_rec.h.valid_from = g_valid_from;
  g_rec.h.segment = h->segment;
  g_rec.h.end_frame = dscm_rec_end_frame();
  g_rec.h.skipped = g_skipped;
}

/* Records FRAMES frames of random changes, a table change on the way and a
 * few frame-count gaps, copying sectors to the slot as it goes, except
 * during `pause` frames from frame 2000 on (the SD card busy: the ring laps
 * the copy when the pause is long enough). */
static void record(int pause)
{
  uint32_t addr[WORDS];
  uint8_t cur[WORDS * 4];
  uint16_t count = WORDS - 8, generation = 1;
  uint32_t frame = 100;
  int i, w;
  for (w = 0; w < WORDS; w++)
    addr[w] = 0x1000u + 4u * (uint32_t)w;
  memset(cur, 0, sizeof(cur));
  dscm_rec_init(g_ring, sizeof(g_ring), 0x4D21, "0123456789abcdef0123456789abcdef");
  g_records = 0;
  for (i = 0; i < FRAMES; i++) {
    int changes = rand() % 6, c;
    if (i % 1500 == 0 && i)
      changes = WORDS;                /* a keyframe's worth */
    for (c = 0; c < changes; c++) {
      const int wi = rand() % count;
      const uint32_t v = (uint32_t)rand();
      cur[wi * 4] = (uint8_t)v; cur[wi * 4 + 1] = (uint8_t)(v >> 8);
      cur[wi * 4 + 2] = (uint8_t)(v >> 16); cur[wi * 4 + 3] = (uint8_t)(v >> 24);
    }
    if (i == 4000) {
      count = WORDS;                  /* a new table */
      generation++;
    }
    frame += (i % 777 == 0) ? 300 : 1 + (rand() % 3 == 0);
    dscm_rec_frame(frame, addr, count, generation, cur);
    g_frame_of[g_records] = frame;
    g_count_of[g_records] = count;
    memcpy(g_addr_of[g_records], addr, sizeof(addr));
    memcpy(g_vals[g_records], cur, sizeof(cur));
    g_records++;
    if (i % 3 == 0 && (i < 2000 || i >= 2000 + pause))
      mirror();
  }
  mirror();
  fill_header();
}

/* The replay as the phone does it: REPLAY_REQ from from_frame, pieces until
 * END, REPLAY_DONE for each; the decoded frames checked against what was
 * recorded. Returns the frames checked, -1 on a mismatch. */
static int replay(uint32_t from_frame, int expect_gap)
{
  static uint8_t stream[DSCM_REC_DATA_SECTORS * SECTOR];
  static uint8_t piece[DSS_REPLAY_CHUNK];
  dss_replay_req_t req;
  dss_replay_data_t data;
  dss_replay_done_t done;
  uint32_t len = 0, at = 0, frame = 0, count = 0, addr[WORDS];
  uint8_t vals[WORDS * 4];
  int checked = 0, have = 0, r, pieces = 0;
  memset(&req, 0, sizeof(req));
  req.run = g_rec.h.run;
  req.max_len = DSS_REPLAY_CHUNK;
  req.from_frame = from_frame;
  for (;;) {
    int n = recording_reply(&g_rec, &req, &data, piece);
    pieces++;
    if (data.flags & DSS_REPLAY_NONE)
      return fail("REPLAY_NONE");
    if (pieces == 1 && !!(data.flags & DSS_REPLAY_GAP) != expect_gap)
      return fail("GAP flag");
    if (data.offset != len)
      return fail("piece offset");
    memcpy(stream + len, piece, n);
    len += n;
    memset(&done, 0, sizeof(done));
    done.run = req.run;
    done.stream = data.stream;
    done.offset = len;
    r = recording_done(&g_rec, &done);
    if (data.flags & DSS_REPLAY_END) {
      if (r != 1 || !g_rec.done)
        return fail("DONE at the end");
      break;
    }
    if (r != 0)
      return fail("DONE before the end");
    req.stream = data.stream;
    req.offset = len;
    if (len > sizeof(stream) - DSS_REPLAY_CHUNK)
      return fail("replay longer than the slot");
  }
  if (len != data.avail)
    return fail("avail");
  g_rec.done = 0;   /* the test serves it again */
  /* decode */
  while (at < len) {
    const uint8_t t = stream[at];
    if (t == DSS_REC_PAD) {
      at++;
      continue;
    }
    if (at % SECTOR == 0 && len - at >= 4 && rd32(stream + at) == DSS_REC_MAGIC) {
      uint32_t i;
      count = rd16(stream + at + 4);
      if (len - at < 12 + count * 8)
        break;                         /* a segment cut by the end of the copy */
      frame = rd32(stream + at + 8);
      for (i = 0; i < count; i++)
        addr[i] = rd32(stream + at + 12 + i * 4);
      memcpy(vals, stream + at + 12 + count * 4, count * 4);
      at += 12 + count * 8;
      have = 1;
      continue;
    }
    if (!have)
      return fail("records before the first segment");
    {
      uint32_t n, advance, i;
      int rec;
      if (t == DSS_REC_FRAME16) {
        if (len - at < 5) break;
        n = rd16(stream + at + 1);
        advance = rd16(stream + at + 3);
        at += 5;
      } else {
        if (len - at < 2) break;
        n = t;
        advance = stream[at + 1];
        at += 2;
      }
      if (len - at < n * 6)
        break;                         /* the last record, cut at the sector end */
      for (i = 0; i < n; i++) {
        const uint32_t idx = rd16(stream + at);
        if (idx >= count)
          return fail("index");
        memcpy(vals + idx * 4, stream + at + 2, 4);
        at += 6;
      }
      frame += advance;
      /* the recorded frame with this number */
      for (rec = 0; rec < g_records; rec++)
        if (g_frame_of[rec] == frame)
          break;
      if (rec == g_records)
        return fail("a frame that was never recorded");
      if ((int32_t)(frame - from_frame) < 0)
        continue;                      /* before the phone's interest: applied, not checked */
      if (count != g_count_of[rec] || memcmp(addr, g_addr_of[rec], count * 4) != 0
          || memcmp(vals, g_vals[rec], count * 4) != 0)
        return fail("values");
      checked++;
    }
  }
  return checked;
}

static int test_slots(void)
{
  dscm_recslot_t a, b;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  if (dscm_rec_pick_slot(&a, &b) != 0 || dscm_rec_offer_slot(&a, &b) != -1 || dscm_rec_next_seq(&a, &b) != 1)
    return fail("empty file");
  a.magic = b.magic = DSCM_RECSLOT_MAGIC;
  a.size = b.size = sizeof(a);
  a.data_sectors = b.data_sectors = DSCM_REC_DATA_SECTORS;
  a.seq = 1; a.run = 5; a.written = 4096;              /* a run the phone hasn't replayed */
  if (dscm_rec_pick_slot(&a, &b) != 1 || dscm_rec_offer_slot(&a, &b) != 0 || dscm_rec_next_seq(&a, &b) != 2)
    return fail("one run");
  b.seq = 2; b.run = 6; b.written = 512;               /* a second: the older is overwritten next */
  if (dscm_rec_pick_slot(&a, &b) != 0 || dscm_rec_offer_slot(&a, &b) != 1 || dscm_rec_next_seq(&a, &b) != 3)
    return fail("two runs");
  b.replayed = 1;
  if (dscm_rec_pick_slot(&a, &b) != 1 || dscm_rec_offer_slot(&a, &b) != 0)
    return fail("the newer replayed");
  b.replayed = 0; b.written = 0;                       /* a claim that never recorded */
  if (dscm_rec_pick_slot(&a, &b) != 1 || dscm_rec_offer_slot(&a, &b) != 0)
    return fail("an empty claim");
  a.replayed = 1; b.seq = 0;
  if (dscm_rec_pick_slot(&a, &b) != 1 || dscm_rec_offer_slot(&a, &b) != -1 || dscm_rec_next_seq(&a, &b) != 2)
    return fail("all replayed");
  /* the phone's last live frame against the last record's (fast start) */
  memset(&b, 0, sizeof(b));
  a.replayed = 0; a.end_frame = 1000;
  a.phone_frame = 0;                                   /* never paired: offered */
  if (dscm_recslot_covered(&a) || dscm_rec_offer_slot(&a, &b) != 0)
    return fail("never paired");
  a.phone_frame = 1000 - DSCM_REC_COVER_FRAMES - 1;    /* lost the link a little over a second before the end */
  if (dscm_recslot_covered(&a) || dscm_rec_offer_slot(&a, &b) != 0)
    return fail("lost before the end");
  a.phone_frame = 1000 - DSCM_REC_COVER_FRAMES;        /* its report a second behind: seen to the end */
  if (!dscm_recslot_covered(&a) || dscm_rec_offer_slot(&a, &b) != -1 || dscm_rec_pick_slot(&a, &b) != 1)
    return fail("covered");
  a.phone_frame = 1010;                                /* ahead of the last header's record */
  if (!dscm_recslot_covered(&a) || dscm_rec_offer_slot(&a, &b) != -1)
    return fail("covered, ahead");
  return 0;
}

int main(void)
{
  int failures = test_slots();
  int pause;
  srand(12345);
  for (pause = 0; pause <= 2000; pause += 2000) {
    uint32_t from;
    int n, gone;
    memset(&g_rec, 0, sizeof(g_rec));
    memset(g_slot, 0xA5, sizeof(g_slot));
    g_sd_written = g_valid_from = g_skipped = 0;
    record(pause);
    g_rec.data = g_slot;
    recording_scan(&g_rec);
    printf("pause %d: %u bytes recorded, %u on the card from %u, %u sectors skipped, %u segments\n", pause,
           (unsigned)dscm_rec_written(), (unsigned)g_rec.h.written, (unsigned)g_rec.lo, (unsigned)g_skipped,
           g_rec.segments);
    if (g_rec.segments < 3)
      failures += fail("segments found");
    if (pause && (!g_skipped || !g_rec.lo))
      failures += fail("the ring was meant to overtake the copy");
    /* from the oldest, from a frame in the middle, from just after the first
     * frame (gone from the copy after the pause: a gap), from after the end
     * (the last segment, nothing to check) */
    n = replay(0, 0);
    printf("  from the oldest: %d frames checked\n", n);
    if (n <= 0) failures++;
    from = g_frame_of[g_records * 2 / 3];
    n = replay(from, 0);
    printf("  from frame %u: %d frames checked\n", (unsigned)from, n);
    if (n <= 0 || n > g_records / 3 + 2) failures += fail("the middle replay's frames");
    from = g_frame_of[0] + 1;
    gone = g_rec.seg_frame[0] >= from;
    n = replay(from, gone);
    printf("  from frame %u (%s): %d frames checked\n", (unsigned)from, gone ? "gone" : "held", n);
    if (n <= 0) failures++;
    if (!pause && gone) failures += fail("the whole run is on the card");
    if (pause && !gone) failures += fail("the start was meant to be gone");
    from = g_frame_of[g_records - 1] + 1000;
    n = replay(from, 0);
    printf("  from frame %u (after the end): %d frames checked\n", (unsigned)from, n);
    if (n != 0) failures += fail("nothing to check after the end");
    g_rec.data = NULL;
  }
  {
    /* another run's request, and a request for nothing */
    dss_replay_req_t req;
    dss_replay_data_t data;
    uint8_t piece[DSS_REPLAY_CHUNK];
    memset(&req, 0, sizeof(req));
    req.run = (uint16_t)(g_rec.h.run + 1);
    req.max_len = 100;
    g_rec.data = g_slot;
    if (recording_reply(&g_rec, &req, &data, piece) != 0 || !(data.flags & DSS_REPLAY_NONE))
      failures += fail("another run");
    g_rec.data = NULL;
  }
  printf(failures ? "FAILED\n" : "OK\n");
  return failures ? 1 : 0;
}
