#include "recording.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SECTOR DSCM_REC_SECTOR_BYTES
#define SEGMENTS_MAX (sizeof(((recording_t*)0)->seg_offset) / sizeof(uint32_t))

static uint32_t rd16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The ROM's title and game code, from its header. */
static void rom_names(const char* rom_path, char title[12], char code[4])
{
  char header[16];
  FILE* f = fopen(rom_path, "rb");
  memset(title, 0, 12);
  memset(code, 0, 4);
  if (!f)
    return;
  if (fread(header, 1, sizeof(header), f) == sizeof(header)) {
    memcpy(title, header, 12);
    memcpy(code, header + 0x0C, 4);
  }
  fclose(f);
}

/* Writes one sector of the file in place (same size, same clusters). */
static int write_sector(uint32_t sector, const void* data)
{
  int ok;
  FILE* f = fopen(DSCM_REC_FILE, "r+b");
  if (!f)
    return 0;
  ok = fseek(f, (long)(sector * SECTOR), SEEK_SET) == 0 && fwrite(data, 1, SECTOR, f) == SECTOR;
  if (fclose(f) != 0)
    ok = 0;
  return ok;
}

static int read_sector(FILE* f, uint32_t sector, void* data)
{
  return fseek(f, (long)(sector * SECTOR), SEEK_SET) == 0 && fread(data, 1, SECTOR, f) == SECTOR;
}

int recording_prepare(const char* rom_path, const char* hash)
{
  static uint8_t zero[16 * 1024];
  static uint8_t sector[SECTOR];
  dscm_recfile_t* file = (dscm_recfile_t*)sector;
  struct stat st;
  if (stat(DSCM_REC_FILE, &st) != 0 || st.st_size != (off_t)DSCM_REC_FILE_SIZE) {
    /* the whole file at once, so its clusters never change afterwards */
    uint32_t left = DSCM_REC_FILE_SIZE;
    FILE* f;
    remove(DSCM_REC_FILE);
    f = fopen(DSCM_REC_FILE, "wb");
    if (!f)
      return 0;
    while (left) {
      const uint32_t n = left < sizeof(zero) ? left : sizeof(zero);
      if (fwrite(zero, 1, n, f) != n) {
        fclose(f);
        remove(DSCM_REC_FILE);
        return 0;
      }
      left -= n;
    }
    if (fclose(f) != 0) {
      remove(DSCM_REC_FILE);
      return 0;
    }
    log_line("recording: made %s", DSCM_REC_FILE);
  }
  memset(sector, 0, sizeof(sector));
  file->magic = DSCM_RECFILE_MAGIC;
  file->version = DSCM_RECFILE_VERSION;
  file->data_sectors = DSCM_REC_DATA_SECTORS;
  file->slots = DSCM_REC_SLOTS;
  rom_names(rom_path, file->title, file->game_code);
  memcpy(file->game_hash, hash, strnlen(hash, sizeof(file->game_hash)));
  return write_sector(0, sector);
}

static int read_slot_header(FILE* f, int slot, dscm_recslot_t* h)
{
  static uint8_t sector[SECTOR];
  if (!read_sector(f, DSCM_REC_SLOT_SECTOR(slot), sector))
    return 0;
  memcpy(h, sector, sizeof(*h));
  return 1;
}

int recording_offer(recording_t* out)
{
  dscm_recslot_t s[DSCM_REC_SLOTS];
  int slot;
  FILE* f = fopen(DSCM_REC_FILE, "rb");
  memset(out, 0, sizeof(*out));
  if (!f)
    return 0;
  memset(s, 0, sizeof(s));
  if (!read_slot_header(f, 0, &s[0]) || !read_slot_header(f, 1, &s[1])) {
    fclose(f);
    return 0;
  }
  fclose(f);
  slot = dscm_rec_offer_slot(&s[0], &s[1]);
  if (slot < 0)
    return 0;
  out->slot = slot;
  out->h = s[slot];
  log_line("recording: slot %d holds run %u of %.12s (%u bytes, frame %u, seq %u%s), not replayed", slot,
           (unsigned)out->h.run, out->h.title, (unsigned)out->h.written, (unsigned)out->h.end_frame,
           (unsigned)out->h.seq, (out->h.flags & DSCM_RECSLOT_FLUSHED) ? "" : ", game not ended");
  return 1;
}

int recording_last_paired(void)
{
  dscm_recslot_t s[DSCM_REC_SLOTS];
  const dscm_recslot_t* last = NULL;
  int i;
  FILE* f = fopen(DSCM_REC_FILE, "rb");
  if (!f)
    return 0;
  memset(s, 0, sizeof(s));
  if (!read_slot_header(f, 0, &s[0]) || !read_slot_header(f, 1, &s[1])) {
    fclose(f);
    return 0;
  }
  fclose(f);
  for (i = 0; i < (int)DSCM_REC_SLOTS; i++) {
    if (dscm_recslot_valid(&s[i]) && (!last || s[i].seq > last->seq))
      last = &s[i];
  }
  if (!last)
    return 0;
  log_line("recording: last run %u (seq %u): phone at frame %u of %u", (unsigned)last->run, (unsigned)last->seq,
           (unsigned)last->phone_frame, (unsigned)last->end_frame);
  return last->phone_frame != 0;
}

int recording_load(recording_t* r)
{
  const uint32_t bytes = r->h.data_sectors * SECTOR;
  FILE* f;
  if (r->data)
    return 1;
  if (r->h.data_sectors != DSCM_REC_DATA_SECTORS || r->h.written > 0xFFFFFFFFu - bytes)
    return 0;
  r->data = (uint8_t*)malloc(bytes);
  if (!r->data)
    return 0;
  f = fopen(DSCM_REC_FILE, "rb");
  if (!f || fseek(f, (long)((DSCM_REC_SLOT_SECTOR(r->slot) + 1) * SECTOR), SEEK_SET) != 0
      || fread(r->data, 1, bytes, f) != bytes) {
    if (f)
      fclose(f);
    free(r->data);
    r->data = NULL;
    return 0;
  }
  fclose(f);
  recording_scan(r);
  log_line("recording: loaded, %u segments in [%u, %u)", r->segments, (unsigned)r->lo, (unsigned)r->h.written);
  return 1;
}

void recording_free(recording_t* r)
{
  free(r->data);
  r->data = NULL;
}

/* The byte at a stream offset, in the slot's data. */
static const uint8_t* at(const recording_t* r, uint32_t abs_offset)
{
  return r->data + (abs_offset % (r->h.data_sectors * SECTOR));
}

/* Walks the records from a whole segment at `from`: the next segment's
 * offset, or `end` when the records run to the end, or (uint32_t)-1 when
 * they don't parse (a false segment: a word's value that looks like the
 * magic). */
static uint32_t walk(const recording_t* r, uint32_t from, uint32_t end)
{
  const uint32_t count = rd16(at(r, from + 4));
  uint32_t i, pos = from + 12 + count * 8;
  while (pos < end) {
    const uint8_t t = *at(r, pos);
    uint32_t n;
    if (t == DSS_REC_PAD) {
      /* to the end of the sector, then a segment (or the end) */
      const uint32_t next = (pos / SECTOR + 1) * SECTOR;
      for (i = pos; i < next && i < end; i++)
        if (*at(r, i) != DSS_REC_PAD)
          return (uint32_t)-1;
      if (next >= end)
        return end;
      return rd32(at(r, next)) == DSS_REC_MAGIC ? next : (uint32_t)-1;
    }
    if (pos % SECTOR == 0 && end - pos >= 4 && rd32(at(r, pos)) == DSS_REC_MAGIC)
      return pos;
    if (t == DSS_REC_FRAME16) {
      if (end - pos < 5)
        return end;              /* cut by the end: the rest of the record isn't on the card */
      n = rd16(at(r, pos + 1));
      pos += 5;
    } else {
      if (t > DSS_REC_FRAME_MAX || end - pos < 2)
        return t > DSS_REC_FRAME_MAX ? (uint32_t)-1 : end;
      n = t;
      pos += 2;
    }
    if (n > count)
      return (uint32_t)-1;
    for (i = 0; i < n; i++) {
      if (end - pos < 6)
        return end;
      if (rd16(at(r, pos)) >= count)
        return (uint32_t)-1;
      pos += 6;
    }
  }
  return end;
}

void recording_scan(recording_t* r)
{
  const uint32_t capacity = r->h.data_sectors * SECTOR;
  const uint32_t end = r->h.written - r->h.written % SECTOR;
  uint32_t start, pos;
  r->segments = 0;
  r->lo = end > capacity ? end - capacity : 0;
  if (r->h.valid_from > r->lo)
    r->lo = r->h.valid_from;
  r->lo = (r->lo + SECTOR - 1) / SECTOR * SECTOR;
  /* from the first segment start that parses through to the end */
  for (start = r->lo; start + 12 <= end; start += SECTOR) {
    if (rd32(at(r, start)) != DSS_REC_MAGIC)
      continue;
    r->segments = 0;
    for (pos = start; pos != end && r->segments < SEGMENTS_MAX;) {
      const uint32_t count = rd16(at(r, pos + 4));
      if (count == 0 || count > DSS_MAX_TABLE_WORDS) {
        pos = (uint32_t)-1;
        break;
      }
      if (end - pos < 12 + count * 8) {
        pos = end;                     /* cut by the end: the stream's last piece, nothing to replay from */
        break;
      }
      if (r->segments && (int32_t)(rd32(at(r, pos + 8)) - r->seg_frame[r->segments - 1]) < 0) {
        /* frames going backwards: what came before is newer data under old
         * offsets (a power-off between a lap of the slot and its header);
         * the recording starts here */
        r->segments = 0;
      }
      r->seg_offset[r->segments] = pos;
      r->seg_frame[r->segments] = rd32(at(r, pos + 8));
      r->segments++;
      pos = walk(r, pos, end);
      if (pos == (uint32_t)-1)
        break;
    }
    if (pos == end)
      return;
  }
  r->segments = 0;
}

uint32_t recording_find(const recording_t* r, uint32_t from_frame, int* gap)
{
  uint32_t best = (uint32_t)-1;
  unsigned i;
  *gap = 0;
  if (!r->segments)
    return (uint32_t)-1;
  for (i = 0; i < r->segments; i++)
    if (from_frame && (int32_t)(r->seg_frame[i] - from_frame) < 0)
      best = r->seg_offset[i];
  if (best != (uint32_t)-1)
    return best;
  /* every segment starts at or after from_frame: the frames from there to
   * the oldest one's base are gone */
  if (from_frame)
    *gap = 1;
  return r->seg_offset[0];
}

uint32_t recording_read(const recording_t* r, uint32_t abs_offset, uint8_t* out, uint32_t len)
{
  const uint32_t end = r->h.written - r->h.written % SECTOR;
  uint32_t done = 0;
  if (!r->data || abs_offset < r->lo || abs_offset >= end)
    return 0;
  if (len > end - abs_offset)
    len = end - abs_offset;
  while (done < len) {
    const uint32_t capacity = r->h.data_sectors * SECTOR;
    const uint32_t a = (abs_offset + done) % capacity;
    const uint32_t n = len - done < capacity - a ? len - done : capacity - a;
    memcpy(out + done, r->data + a, n);
    done += n;
  }
  return len;
}

int recording_reply(recording_t* r, const dss_replay_req_t* req, dss_replay_data_t* out, uint8_t* data)
{
  const uint32_t end = r->h.written - r->h.written % SECTOR;
  uint32_t stream = req->stream, len, max_len = req->max_len;
  int gap = 0;
  memset(out, 0, sizeof(*out));
  out->run = r->h.run;
  if (req->run != r->h.run || r->done || !recording_load(r)) {
    out->flags = DSS_REPLAY_NONE;
    return 0;
  }
  if (stream == 0) {
    stream = recording_find(r, req->from_frame, &gap);
    if (stream == (uint32_t)-1) {
      out->flags = DSS_REPLAY_NONE;
      return 0;
    }
    r->stream = stream;
    if (gap)
      out->flags |= DSS_REPLAY_GAP;
  }
  if (max_len > DSS_REPLAY_CHUNK)
    max_len = DSS_REPLAY_CHUNK;
  out->stream = stream;
  out->offset = req->offset;
  out->avail = stream < end ? end - stream : 0;
  out->end_frame = r->h.end_frame;
  len = req->offset < out->avail ? out->avail - req->offset : 0;
  if (len > max_len)
    len = max_len;
  len = recording_read(r, stream + req->offset, data, len);
  out->len = (uint16_t)len;
  if (req->offset + len >= out->avail)
    out->flags |= DSS_REPLAY_END;
  return (int)len;
}

static int mark_replayed(recording_t* r)
{
  static uint8_t sector[SECTOR];
  memset(sector, 0, sizeof(sector));
  r->h.replayed = 1;
  memcpy(sector, &r->h, sizeof(r->h));
  if (!write_sector(DSCM_REC_SLOT_SECTOR(r->slot), sector)) {
    log_line("recording: couldn't mark slot %d replayed", r->slot);
    return 0;
  }
  return 1;
}

int recording_done(recording_t* r, const dss_replay_done_t* done)
{
  const uint32_t end = r->h.written - r->h.written % SECTOR;
  if (done->run != r->h.run || r->done)
    return -1;
  if (done->stream >= end || done->stream + done->offset < end)
    return 0;
  r->done = 1;
  r->stream = 0;
  log_line("recording: run %u replayed to its end (%u bytes)", (unsigned)r->h.run, (unsigned)end);
  mark_replayed(r);
  return 1;
}
