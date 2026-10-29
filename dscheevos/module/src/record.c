/*
 * Cheevotron streamer: the recording (DSS_CAP_RECORD). Every frame's changes
 * of the watched words go into a ring, whether the phone listens or not, so
 * the phone can replay what it missed after a drop of the link: it asks for
 * the recording from the first frame it hasn't evaluated (stream.c answers
 * REPLAY_REQ from here). The record format is in ds_stream_protocol.h. The
 * ARM7 mirrors the ring onto the SD card (dscm_rec_t, dscheevos_module.h),
 * so a run the phone never heard the end of can be replayed later too.
 *
 * Runs in the game's idle time only (module_idle, through stream.c).
 */
#include "dscheevos_module.h"
#include "ds_stream_protocol.h"

#include <stddef.h>
#include <string.h>

void dscm_dc_clean(const void* addr, uint32_t len);   /* start.s */

#define SEGMENTS 128                   /* segment starts remembered (the ring holds fewer) */

static dscm_rec_t g_rec __attribute__((aligned(32)));
static uint8_t*  g_ring;               /* the module's view of the ring */
static uint32_t  g_size;               /* bytes, a multiple of DSS_REC_SECTOR */
static uint32_t  g_written;            /* stream bytes written (g_rec.written, kept here too) */
static uint32_t  g_clean_at;           /* stream offset up to which the ring is cleaned to RAM */

/* the table and the values the last record left (the next FRAME's base) */
static uint32_t  g_addr[DSS_MAX_TABLE_WORDS];
static uint8_t   g_vals[DSS_MAX_TABLE_WORDS * 4];
static uint16_t  g_count, g_generation;
static int       g_have_table;
static uint32_t  g_frame;              /* the frame of the last record */
static uint32_t  g_seg_frames;         /* FRAME records in the current segment */

static struct { uint32_t offset, frame; } g_segs[SEGMENTS];
static uint32_t  g_seg_head;           /* segments written so far (g_segs is a ring of the last SEGMENTS) */

static void put(const void* data, uint32_t len)
{
  const uint8_t* p = (const uint8_t*)data;
  while (len) {
    const uint32_t at = g_written % g_size;
    const uint32_t n = len < g_size - at ? len : g_size - at;
    memcpy(g_ring + at, p, n);
    g_written += n;
    p += n;
    len -= n;
  }
}

static void put16(uint32_t v) { const uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(b, 2); }
static void put32(uint32_t v) { const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; put(b, 4); }

/* Completed sectors to RAM (the ARM7 reads them there), then the counters.
 * Only the module's part of the header: the ARM7 writes the rest during
 * play, and cleaning its lines would put this cache's stale copy back. */
static void publish(void)
{
  const uint32_t done = g_written - g_written % DSS_REC_SECTOR;
  while (g_clean_at < done) {
    dscm_dc_clean(g_ring + g_clean_at % g_size, DSS_REC_SECTOR);
    g_clean_at += DSS_REC_SECTOR;
  }
  g_rec.written = g_written;
  g_rec.end_frame = g_frame;
  dscm_dc_clean(&g_rec, DSCM_REC_ARM9_BYTES);
}

void dscm_rec_init(void* ring, uint32_t size, uint16_t run, const char* hash)
{
  g_ring = (uint8_t*)ring;
  g_size = size - size % DSS_REC_SECTOR;
  g_written = g_clean_at = 0;
  g_have_table = 0;
  g_seg_head = 0;
  memset(&g_rec, 0, sizeof(g_rec));
  g_rec.magic = DSCM_REC_MAGIC;
  g_rec.ring = DSCM_PHYS(ring);
  g_rec.ring_size = g_size;
  g_rec.run = run;
  if (hash)
    memcpy(g_rec.game_hash, hash, strnlen(hash, sizeof(g_rec.game_hash)));
  dscm_dc_clean(&g_rec, sizeof(g_rec));
}

dscm_rec_t* dscm_rec_header(void)
{
  return g_size ? &g_rec : NULL;
}

/* A segment: padding to the next sector, then the table and every word's
 * value as of `base_frame` (no frame is evaluated for it). */
static void segment(uint32_t base_frame, const uint32_t* addr, uint16_t count, uint16_t generation, const uint8_t* vals)
{
  static const uint8_t pad[DSS_REC_SECTOR] = { [0 ... DSS_REC_SECTOR - 1] = DSS_REC_PAD };
  const uint32_t r = g_written % DSS_REC_SECTOR;
  uint16_t i;
  if (r)
    put(pad, DSS_REC_SECTOR - r);
  g_segs[g_seg_head % SEGMENTS].offset = g_written;
  g_segs[g_seg_head % SEGMENTS].frame = base_frame;
  g_seg_head++;
  g_rec.segment = g_written;
  put32(DSS_REC_MAGIC);
  put16(count);
  put16(generation);
  put32(base_frame);
  for (i = 0; i < count; i++)
    put32(addr[i]);
  put(vals, (uint32_t)count * 4);
  memcpy(g_addr, addr, (size_t)count * 4);
  memcpy(g_vals, vals, (size_t)count * 4);
  g_count = count;
  g_generation = generation;
  g_have_table = 1;
  g_frame = base_frame;
  g_seg_frames = 0;
}

/* One frame: the watched words (`cur`, table order) as read for frame
 * `frame`. A new table, a long segment or a step back in the frame count
 * starts a segment; else a FRAME of the words that changed. */
void dscm_rec_frame(uint32_t frame, const uint32_t* addr, uint16_t count, uint16_t generation, const uint8_t* cur)
{
  uint16_t changed = 0, i;
  uint32_t advance;
  if (!g_size || !count)
    return;
  if (!g_have_table || generation != g_generation || count != g_count
      || memcmp(addr, g_addr, (size_t)count * 4) != 0
      || frame - g_frame - 1 >= 0xFFFFu || g_seg_frames >= DSS_REC_SEGMENT_FRAMES) {
    /* the new table's words as they are now, as the base just before this
     * frame (its own changes are lost to the first record of a new table) */
    const int same = g_have_table && generation == g_generation && count == g_count
                     && memcmp(addr, g_addr, (size_t)count * 4) == 0 && frame - g_frame - 1 < 0xFFFFu;
    segment(same ? g_frame : frame - 1, addr, count, generation, same ? g_vals : cur);
  }
  for (i = 0; i < count; i++)
    if (memcmp(cur + i * 4, g_vals + i * 4, 4) != 0)
      changed++;
  advance = frame - g_frame;
  if (changed <= DSS_REC_FRAME_MAX && advance <= 0xFF) {
    const uint8_t h[2] = { (uint8_t)changed, (uint8_t)advance };
    put(h, 2);
  } else {
    const uint8_t t = DSS_REC_FRAME16;
    put(&t, 1);
    put16(changed);
    put16(advance);
  }
  for (i = 0; i < count && changed; i++) {
    if (memcmp(cur + i * 4, g_vals + i * 4, 4) != 0) {
      put16(i);
      put(cur + i * 4, 4);
      memcpy(g_vals + i * 4, cur + i * 4, 4);
      changed--;
    }
  }
  g_frame = frame;
  g_seg_frames++;
  publish();
}

uint32_t dscm_rec_written(void) { return g_written; }
uint32_t dscm_rec_end_frame(void) { return g_frame; }

/* The values the last record left, if they are for this table (the live
 * stream's base once a replay has caught up). */
const uint8_t* dscm_rec_values(const uint32_t* addr, uint16_t count, uint16_t generation)
{
  if (!g_have_table || generation != g_generation || count != g_count || memcmp(addr, g_addr, (size_t)count * 4) != 0)
    return NULL;
  return g_vals;
}

/* The segment to replay from: the latest one whose base frame is before
 * from_frame (the first frame the phone hasn't evaluated; 0: the oldest).
 * Returns its stream offset, or (uint32_t)-1 if nothing is recorded; *gap is
 * set when recorded frames the phone needs were already overwritten. */
uint32_t dscm_rec_find(uint32_t from_frame, int* gap)
{
  const uint32_t oldest_ok = g_written > g_size ? g_written - g_size : 0;
  const uint32_t first = g_seg_head > SEGMENTS ? g_seg_head - SEGMENTS : 0;
  uint32_t k, best = (uint32_t)-1, oldest = (uint32_t)-1;
  *gap = 0;
  for (k = first; k < g_seg_head; k++) {
    const uint32_t off = g_segs[k % SEGMENTS].offset, f = g_segs[k % SEGMENTS].frame;
    if (off < oldest_ok)
      continue;                        /* partly overwritten */
    if (oldest == (uint32_t)-1)
      oldest = off;
    if (from_frame && (int32_t)(f - from_frame) < 0)
      best = off;
  }
  if (best != (uint32_t)-1)
    return best;
  /* every segment left starts at or after from_frame: the frames from there
   * to the oldest one's base are gone */
  if (oldest != (uint32_t)-1 && from_frame)
    *gap = 1;
  return oldest;
}

/* Up to len bytes of the stream from abs_offset; 0 if they are no longer
 * (or not yet) in the ring. */
uint32_t dscm_rec_read(uint32_t abs_offset, uint8_t* out, uint32_t len)
{
  const uint32_t oldest_ok = g_written > g_size ? g_written - g_size : 0;
  uint32_t n, done = 0;
  if (!g_size || abs_offset < oldest_ok || abs_offset >= g_written)
    return 0;
  if (len > g_written - abs_offset)
    len = g_written - abs_offset;
  while (done < len) {
    const uint32_t at = (abs_offset + done) % g_size;
    n = len - done < g_size - at ? len - done : g_size - at;
    memcpy(out + done, g_ring + at, n);
    done += n;
  }
  return len;
}
