/*
 * Cheevotron RAM streamer (DSCM_STREAM build): instead of evaluating the
 * achievement set on the console, send the RAM words rcheevos reads to the
 * phone over UDP once a frame; the phone runs rcheevos. Wire contract:
 * include/ds_stream_protocol.h and cheevotron/docs/dsi-stream-protocol.md.
 *
 * This file is the protocol and the UDP glue. It runs only in the game's idle
 * time, from the module's halt path (module.c, DSCM_STREAM build), never in an
 * interrupt. It reuses the module's Wi-Fi link (net/link.c) and lwIP, and the
 * module's memory reads (dscm_stream_read_bytes); it adds no evaluation.
 */
#include "dscheevos_module.h"
#include "ds_stream_protocol.h"
#include "net/net.h"

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* module.c */
extern int dscm_stream_read_bytes(uint32_t ra_addr, void* out, uint32_t len);
extern uint32_t dscm_irq_off(void);
extern void dscm_irq_restore(uint32_t cpsr);
extern dscm_status_t dscm_g_status;   /* the in-game menu reads it: show stream state here */

/* start.s: bump the counters the ARM7 card engine reads to blink the DSi LED */
extern void dscm_publish_notify(uint32_t count);
extern void dscm_publish_cues(uint32_t cues);

/* popup.c: pop-ups on the console's own screen */
void dscm_popup_on_popup(const uint8_t* body, uint16_t len);
void dscm_popup_on_badge(const uint8_t* body, uint16_t len);
void dscm_popup_set_mode(uint8_t mode);
void dscm_popup_status(dss_display_status_t* out);
void dscm_popup_local(const char* title, const char* detail, const char* value, uint16_t hold);

/* record.c: the recording the phone replays after a drop */
void dscm_rec_init(void* ring, uint32_t size, uint16_t run, const char* hash);
dscm_rec_t* dscm_rec_header(void);
void dscm_rec_note(uint32_t line, const char* text);
void dscm_rec_frame(uint32_t frame, const uint32_t* addr, uint16_t count, uint16_t generation, const uint8_t* cur);
uint32_t dscm_rec_written(void);
uint32_t dscm_rec_end_frame(void);
void dscm_rec_phone_frame(uint32_t frame);
const uint8_t* dscm_rec_values(const uint32_t* addr, uint16_t count, uint16_t generation);
uint32_t dscm_rec_find(uint32_t from_frame, int* gap);
uint32_t dscm_rec_read(uint32_t abs_offset, uint8_t* out, uint32_t len);
extern void dscm_publish_rec(void* rec);   /* start.s */

/* ---- state ---- */

static struct udp_pcb* g_pcb;

/* the published watch table (table order), and the last values sent */
static uint32_t g_watch[DSS_MAX_TABLE_WORDS];
static uint8_t  g_last[DSS_MAX_TABLE_WORDS * 4];
static uint16_t g_watch_count;
static uint16_t g_generation;          /* generation of g_watch */
static int      g_have_last;           /* g_last holds the previous frame (for DELTA) */

/* assembling an incoming generation, chunk by chunk */
#define CHUNKS ((DSS_MAX_TABLE_WORDS + DSS_TABLE_CHUNK - 1) / DSS_TABLE_CHUNK)
static uint16_t g_asm_gen;
static uint16_t g_asm_total;
static uint32_t g_asm_words[DSS_MAX_TABLE_WORDS];
static uint8_t  g_asm_seen[CHUNKS];
static uint16_t g_asm_have;
static int      g_asm_active;

/* pairing */
static int       g_paired;
static uint16_t  g_session;            /* phone-chosen, adopted from TABLE; 0 unpaired */
static ip_addr_t g_phone_ip;
static uint16_t  g_phone_port;
static int       g_phone_known;        /* g_phone_ip: the launcher's phone, a PROBE's sender or the last pairing */
static uint32_t  g_last_phone_ms;
static uint32_t  g_last_hello_ms;
static int       g_force_full;
static uint32_t  g_lost_ms;            /* when a pairing timed out (0: none since the last) */
static uint8_t   g_lost_shown;         /* "Phone link lost" notices shown since */
static uint32_t  g_reconnect_ms;        /* the in-game menu's Reconnect: when (0: none waiting) */
static uint32_t  g_start_ms;           /* the first service pass (0: not yet) */
static int       g_ever_paired;        /* a phone paired since the game started */
#define FIRST_PAIR_WAIT_MS 20000u      /* no phone this long after the start: the lost notice (a fast start
                                          looks for the phone only here) */
#define RECONNECT_WAIT_MS 60000u       /* no phone this long after it: the link-lost notice again */
#define SERVICE_GAP_MS 1000u           /* no idle pass this long: the game kept the module from running */

/* the recording: this run's id (HELLO), and a replay the phone is reading */
static uint16_t  g_run;
static int       g_recording;          /* the ring is set up */
static int       g_replaying;          /* live FRAMEs held back until the phone caught up */
static uint32_t  g_replay_stream;      /* the replay's first segment (stream offset) */
static uint32_t  g_replay_ms;          /* the phone's last REPLAY_REQ/DONE */
static uint32_t  g_now_ms;             /* dscm_stream_service's clock, for the receive callback */

/* the boot's timeline (DSCM_BOOT_LOG), sent once after the first pairing */
static uint32_t  g_boot_log_sent;      /* bytes sent so far */
static uint32_t  g_boot_log_ms;        /* when the last piece went */
static int       g_boot_log_done;
#define BOOT_LOG_GAP_MS 50u            /* between pieces: the ARM7 shim's TX ring has 4 slots */

/* HELLO payload, filled at start */
static dss_hello_t g_hello;
static uint32_t    g_frame_sent;       /* last frame number streamed */
static uint32_t    g_frames_sent_total, g_frames_stale_total, g_peeks_answered;
static uint8_t     g_capture_seq;      /* frames captured (recorded, sent, or both): FRAME's seq */

/* LED cues: the phone tells us what it unlocked; the ARM7 card engine reads
 * these counters (header notify_count/cues) and blinks the DSi's LED. */
static uint32_t    g_notify_count;     /* unlocks the phone reported (monotonic) */
static uint32_t    g_led_cues;         /* DSCM_CUE_* bitfield for leaderboard/progress */

/* ---- small writers ---- */

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

static uint8_t* header(uint8_t* buf, uint8_t type, uint16_t session, uint16_t length)
{
  put16(buf, DSS_MAGIC);
  buf[2] = type;
  buf[3] = DSS_VERSION;
  put16(buf + 4, session);
  put16(buf + 6, length);
  return buf + DSS_HEADER_SIZE;
}

static void send_to(const ip_addr_t* ip, uint16_t port, const uint8_t* data, uint16_t len)
{
  struct pbuf* p;
  if (!g_pcb)
    return;
  p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
  if (!p)
    return;
  memcpy(p->payload, data, len);
  udp_sendto(g_pcb, p, ip, port);
  pbuf_free(p);
}

/* ---- outgoing ---- */

/* One packet to the paired phone (popup.c); nothing while unpaired. */
void dscm_stream_send(uint8_t type, const void* body, uint16_t len)
{
  uint8_t buf[DSS_HEADER_SIZE + 64];
  if (!g_paired || len > sizeof(buf) - DSS_HEADER_SIZE)
    return;
  memcpy(header(buf, type, g_session, len), body, len);
  send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)(DSS_HEADER_SIZE + len));
}

/* HELLO to everyone, and straight to the phone we know: a phone with its
 * screen off drops broadcasts after a few seconds, but still takes unicast. */
static void send_hello(void)
{
  uint8_t buf[DSS_HEADER_SIZE + sizeof(dss_hello_t)];
  ip_addr_t bcast;
  uint8_t* body = header(buf, DSS_T_HELLO, 0, (uint16_t)sizeof(dss_hello_t));
  memcpy(body, &g_hello, sizeof(dss_hello_t));
  ip_addr_set_ip4_u32(&bcast, 0xFFFFFFFFu);   /* 255.255.255.255 */
  send_to(&bcast, DSS_PORT, buf, (uint16_t)sizeof(buf));
  if (g_phone_known)
    send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)sizeof(buf));
}

/* One piece of the boot's timeline a call, whole lines, until all of it went:
 * stage 1's log and the bootloader's stamps, which nds-bootstrap left in the
 * module region's spare 4 KB (nothing of ours touches it, so the cache holds
 * none of it). Once a game start; a lost piece stays lost (log only). */
static void send_boot_log(uint32_t now_ms)
{
  static uint8_t buf[DSS_HEADER_SIZE + sizeof(dss_boot_log_t) + DSS_BOOT_LOG_PIECE];
  const volatile uint32_t* head = (const volatile uint32_t*)DSCM_DATA_ADDR(DSCM_BOOT_LOG);
  const char* text = (const char*)DSCM_DATA_ADDR(DSCM_BOOT_LOG + 8);
  dss_boot_log_t h;
  uint32_t total, n;
  uint8_t* body;
  if (g_boot_log_done || now_ms - g_boot_log_ms < BOOT_LOG_GAP_MS)
    return;
  total = head[1];
  if (head[0] != DSCM_BOOT_LOG_MAGIC || total > DSCM_BOOT_LOG_SIZE - 8 || g_boot_log_sent >= total) {
    g_boot_log_done = 1;
    return;
  }
  n = total - g_boot_log_sent;
  if (n > DSS_BOOT_LOG_PIECE) {
    n = DSS_BOOT_LOG_PIECE;
    while (n > 1 && text[g_boot_log_sent + n - 1] != '\n')
      n--;
    if (n <= 1)
      n = DSS_BOOT_LOG_PIECE;   /* no line end in a whole piece: cut it */
  }
  h.offset = (uint16_t)g_boot_log_sent;
  h.total = (uint16_t)total;
  h.vblank = g_hello.vblank;
  body = header(buf, DSS_T_BOOT_LOG, g_session, (uint16_t)(sizeof(h) + n));
  memcpy(body, &h, sizeof(h));
  memcpy(body + sizeof(h), text + g_boot_log_sent, n);
  send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)(DSS_HEADER_SIZE + sizeof(h) + n));
  g_boot_log_sent += n;
  g_boot_log_ms = now_ms;
}

/* Read the watched words into out (table order); IRQs masked so no VBlank
 * splits the copy. Returns 0 if every word read, else the first bad index + 1
 * (an unreadable word reads as zero and the frame is still sent). */
/* While the copy below runs, only the pop-up band's VCOUNT IRQ may come in:
 * every 32 words, if it is pending, IE holds that one alone and the CPU takes
 * it. Its handler switches the display and nothing else (no game memory, no
 * game thread), so the copy stays one frame's; every other IRQ (the VBlank
 * first) still waits for its end. Before, the whole copy kept IRQs masked
 * (HeartGold's 1024 words: the band's IRQ up to 27 lines late, the band
 * missing in frames while walking). */
#define STREAM_REG_IE (*(volatile uint32_t*)0x04000210)
#define STREAM_REG_IF (*(volatile uint32_t*)0x04000214)
#define STREAM_IRQ_VCOUNT (1u << 2)
int dscm_popup_owns_vcount(void);   /* popup.c */

static void let_band_irq_in(uint32_t* cpsr)
{
  const uint32_t ie = STREAM_REG_IE;
  if (!(ie & STREAM_REG_IF & STREAM_IRQ_VCOUNT) || !dscm_popup_owns_vcount())
    return;
  STREAM_REG_IE = STREAM_IRQ_VCOUNT;
  dscm_irq_restore(*cpsr);
  *cpsr = dscm_irq_off();
  STREAM_REG_IE = ie;
}

static void read_words(uint8_t* out, uint16_t count)
{
  uint32_t cpsr = dscm_irq_off();
  uint16_t i;
  for (i = 0; i < count; i++) {
    if ((i & 31) == 31)
      let_band_irq_in(&cpsr);
    /* a word in main RAM read straight: through the call, HeartGold's 1024
     * words kept IRQs masked about 2.5 ms, past the pop-up band's VCOUNT IRQ */
    const uint32_t a = g_watch[i];
    if (a < 0x400000u && !(a & 3)) {
      const uint32_t v = *(const volatile uint32_t*)(0x02000000u + a);
      memcpy(out + i * 4, &v, 4);
    } else if (!dscm_stream_read_bytes(a, out + i * 4, 4)) {
      memset(out + i * 4, 0, 4);
    }
  }
  dscm_irq_restore(cpsr);
}

/* Build and send a FRAME for frame number `frame`. FULL while the table fits a
 * packet or a keyframe is due; DELTA otherwise (only changed words). */
/* One FRAME. FULL (every word) only when the whole table fits one packet and
 * a keyframe is due (forced, or no history yet); otherwise DELTA of the words
 * that changed, as many as fit one packet. A table too big for a FULL packet
 * (over ~345 words) streams by DELTA alone: the phone seeded every word with a
 * PEEK when it registered the address, so it never needs a giant FULL. g_last
 * is updated only for the words actually sent, so a big first frame or a burst
 * of changes drains over the next few frames instead of being dropped. An
 * all-unchanged frame still sends an empty DELTA, which carries the VBlank
 * count and keeps the pairing alive. */
static void send_frame(uint32_t frame, const uint8_t* cur)
{
  uint8_t buf[DSS_MAX_PAYLOAD];
  uint16_t n = g_watch_count;
  int fits_full = (DSS_HEADER_SIZE + 10 + (int)n * 4 <= DSS_MAX_PAYLOAD);

  if (fits_full && (g_force_full || !g_have_last)) {
    uint8_t* body = header(buf, DSS_T_FRAME, g_session, (uint16_t)(10 + n * 4));
    put32(body + 0, frame);
    put16(body + 4, g_generation);
    body[6] = DSS_FRAME_FULL | DSS_FRAME_SEQ;
    body[7] = g_capture_seq;
    put16(body + 8, n);
    if (n)
      memcpy(body + 10, cur, n * 4);
    send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)(DSS_HEADER_SIZE + 10 + n * 4));
    memcpy(g_last, cur, (size_t)n * 4);
    g_have_last = 1;
    g_force_full = 0;
  } else {
    uint8_t* body = header(buf, DSS_T_FRAME, g_session, 0);
    uint8_t* w = body + 10;
    uint16_t changed = 0, i, len;
    const uint16_t max_pairs = (uint16_t)((DSS_MAX_PAYLOAD - DSS_HEADER_SIZE - 10) / 6);
    for (i = 0; i < n && changed < max_pairs; i++) {
      if (memcmp(cur + i * 4, g_last + i * 4, 4) != 0) {
        put16(w, i);
        memcpy(w + 2, cur + i * 4, 4);
        w += 6;
        changed++;
        memcpy(g_last + i * 4, cur + i * 4, 4);   /* only the words sent this frame */
      }
    }
    put32(body + 0, frame);
    put16(body + 4, g_generation);
    body[6] = DSS_FRAME_SEQ;         /* DELTA */
    body[7] = g_capture_seq;
    put16(body + 8, changed);
    len = (uint16_t)(10 + changed * 6);
    put16(buf + 6, len);
    send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)(DSS_HEADER_SIZE + len));
    g_have_last = 1;
    if (!fits_full)
      g_force_full = 0;              /* can't honour a FULL request on a big table */
  }
  g_frames_sent_total++;
  /* the in-game menu's "Frames" counter moves while streaming works */
  dscm_g_status.frames_snapshotted = g_frames_sent_total;
  dscm_g_status.frames_evaluated = g_frames_sent_total;
  dscm_g_status.unlocks_sent = g_watch_count;   /* words being streamed */
}

static void send_peek_reply(uint16_t request, uint32_t address, uint32_t frame, const uint8_t* data, uint16_t len)
{
  uint8_t buf[DSS_HEADER_SIZE + 12 + DSS_PEEK_MAX];
  uint8_t* body = header(buf, DSS_T_PEEK_REPLY, g_session, (uint16_t)(12 + len));
  put16(body + 0, request);
  put16(body + 2, len);
  put32(body + 4, address);
  put32(body + 8, frame);
  if (len)
    memcpy(body + 12, data, len);
  send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)(DSS_HEADER_SIZE + 12 + len));
}

/* ---- incoming ---- */

static void publish_generation(void)
{
  g_watch_count = g_asm_total;
  g_generation = g_asm_gen;
  memcpy(g_watch, g_asm_words, g_asm_total * sizeof(uint32_t));
  g_have_last = 0;           /* the old table's values don't line up */
  g_force_full = 1;
  g_asm_active = 0;
}

static void on_table(const uint8_t* body, uint16_t len)
{
  uint16_t gen, total, idx, words, i;
  if (len < 8)
    return;
  gen = get16(body + 0);
  total = get16(body + 2);
  idx = get16(body + 4);
  words = get16(body + 6);
  if (total > DSS_MAX_TABLE_WORDS || idx >= CHUNKS || words > DSS_TABLE_CHUNK)
    return;
  if (8 + words * 4 > len)
    return;
  if (total == 0) {          /* pairing with no words: stream the frame count only */
    g_asm_gen = gen; g_asm_total = 0; g_asm_have = 0; g_asm_active = 1;
    publish_generation();
    return;
  }
  if (!g_asm_active || gen != g_asm_gen) {
    g_asm_gen = gen; g_asm_total = total; g_asm_have = 0; g_asm_active = 1;
    memset(g_asm_seen, 0, sizeof(g_asm_seen));
  }
  if (!g_asm_seen[idx]) {
    for (i = 0; i < words; i++)
      g_asm_words[idx * DSS_TABLE_CHUNK + i] = get32(body + 8 + i * 4);
    g_asm_seen[idx] = 1;
    g_asm_have = (uint16_t)(g_asm_have + words);
  }
  if (g_asm_have >= g_asm_total)
    publish_generation();
}

static void on_peek(const uint8_t* body, uint16_t len)
{
  uint16_t request, length;
  uint32_t address;
  uint8_t data[DSS_PEEK_MAX];
  if (len < 8)
    return;
  request = get16(body + 0);
  length = get16(body + 2);
  address = get32(body + 4);
  if (length == 0 || length > DSS_PEEK_MAX) {
    send_peek_reply(request, address, 0, NULL, 0);
    return;
  }
  if (!dscm_stream_read_bytes(address, data, length)) {
    send_peek_reply(request, address, 0, NULL, 0);   /* unreadable: len 0 */
    return;
  }
  send_peek_reply(request, address, g_frame_sent, data, length);
  g_peeks_answered++;
}

static void on_control(const uint8_t* body, uint16_t len)
{
  if (len < 2)
    return;
  switch (body[0]) {
    case DSS_CTRL_FULL:  g_force_full = 1; break;
    case DSS_CTRL_RESET: g_frames_sent_total = g_frames_stale_total = g_peeks_answered = 0; break;
    case DSS_CTRL_DISPLAY: dscm_popup_set_mode(body[1]); break;
    default: break;      /* DELTA on/off is automatic in this build */
  }
}

/* ---- replay: the phone reads what it missed from the recording ---- */

static void send_replay(uint8_t flags, uint32_t stream, uint32_t offset, uint16_t max_len)
{
  static uint8_t buf[DSS_HEADER_SIZE + sizeof(dss_replay_data_t) + DSS_REPLAY_CHUNK];
  const uint32_t written = dscm_rec_written();
  uint8_t* body;
  uint32_t len = 0;
  if (max_len > DSS_REPLAY_CHUNK)
    max_len = DSS_REPLAY_CHUNK;
  if (!(flags & DSS_REPLAY_NONE)) {
    len = dscm_rec_read(stream + offset, buf + DSS_HEADER_SIZE + sizeof(dss_replay_data_t), max_len);
    if (!len && stream + offset < written)
      flags |= DSS_REPLAY_GAP;         /* already overwritten: the phone starts over */
    if (stream + offset + len >= written)
      flags |= DSS_REPLAY_END;
  }
  body = header(buf, DSS_T_REPLAY_DATA, g_session, (uint16_t)(sizeof(dss_replay_data_t) + len));
  put16(body + 0, g_run);
  body[2] = flags | DSS_REPLAY_SEQ;
  body[3] = g_capture_seq;         /* the last record's: the next live FRAME's seq is one more */
  put32(body + 4, stream);
  put32(body + 8, offset);
  put32(body + 12, written - stream);
  put32(body + 16, dscm_rec_end_frame());
  put16(body + 20, (uint16_t)len);
  put16(body + 22, 0);
  send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)(DSS_HEADER_SIZE + sizeof(dss_replay_data_t) + len));
}

static void on_replay_req(const uint8_t* body, uint16_t len, uint32_t now_ms)
{
  uint16_t run, max_len;
  uint32_t from_frame, stream, offset;
  if (len < sizeof(dss_replay_req_t))
    return;
  run = get16(body + 0);
  max_len = get16(body + 2);
  from_frame = get32(body + 4);
  stream = get32(body + 8);
  offset = get32(body + 12);
  if (!g_recording || run != g_run) {
    send_replay(DSS_REPLAY_NONE, 0, 0, 0);
    return;
  }
  if (stream == 0) {
    int gap;
    const uint32_t start = dscm_rec_find(from_frame, &gap);
    if (start == (uint32_t)-1) {
      send_replay(DSS_REPLAY_NONE, 0, 0, 0);
      return;
    }
    g_replay_stream = start;
    g_replaying = 1;                   /* no live FRAMEs until the phone caught up */
    g_replay_ms = now_ms;
    send_replay(gap ? DSS_REPLAY_GAP : 0, start, 0, max_len);
    return;
  }
  g_replay_ms = now_ms;
  send_replay(0, stream, offset, max_len);
}

static void on_replay_done(const uint8_t* body, uint16_t len, uint32_t now_ms)
{
  uint32_t stream, offset, written;
  const uint8_t* vals;
  /* (a late DONE after the replay ended would reset the live stream's base) */
  if (len < sizeof(dss_replay_done_t) || !g_recording || !g_replaying || get16(body + 0) != g_run)
    return;
  stream = get32(body + 4);
  offset = get32(body + 8);
  g_replay_ms = now_ms;
  written = dscm_rec_written();
  if (stream + offset < written) {
    send_replay(0, stream, offset, DSS_REPLAY_CHUNK);   /* more came meanwhile */
    /* The game records a frame every VBlank or two, so a phone a round trip
     * behind never finds the end empty (HeartGold stayed in a replay for 38 s,
     * the phone sending a DONE a frame). Once the rest fits this one piece,
     * it ends the replay: live FRAMEs go on after it (its seq tells the phone
     * which comes next; if the piece is lost, the next FRAME shows the gap). */
    if (written - (stream + offset) > DSS_REPLAY_CHUNK)
      return;
  }
  /* caught up: live again, as DELTAs from the values the phone now has */
  vals = dscm_rec_values(g_watch, g_watch_count, g_generation);
  if (vals) {
    memcpy(g_last, vals, (size_t)g_watch_count * 4);
    g_have_last = 1;
    g_force_full = 0;
  } else {
    g_have_last = 0;
  }
  g_replaying = 0;
}

/* The phone evaluates the set and unlocks; it then sends a CUE so the console
 * can blink its LED, the way the on-console build does for its own unlocks. */
static void on_cue(const uint8_t* body, uint16_t len)
{
  if (len < 1)
    return;
  switch (body[0]) {
    case DSS_CUE_UNLOCK:
      dscm_publish_notify(++g_notify_count);
      break;
    case DSS_CUE_LEADERBOARD:
      g_led_cues = (g_led_cues & ~DSCM_CUE_LEADERBOARD_MASK)
                 | ((g_led_cues + (1u << DSCM_CUE_LEADERBOARD_SHIFT)) & DSCM_CUE_LEADERBOARD_MASK);
      dscm_publish_cues(g_led_cues);
      break;
    case DSS_CUE_PROGRESS:
      g_led_cues = (g_led_cues & ~DSCM_CUE_PROGRESS_MASK)
                 | ((g_led_cues + 1) & DSCM_CUE_PROGRESS_MASK);
      dscm_publish_cues(g_led_cues);
      break;
    default:
      break;
  }
}

static void on_udp(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port)
{
  uint8_t buf[DSS_MAX_PAYLOAD];
  uint16_t total, length, session;
  uint8_t type;
  const uint8_t* body;
  (void)arg; (void)pcb;
  if (!p)
    return;
  total = (uint16_t)(p->tot_len < sizeof(buf) ? p->tot_len : 0);
  if (total >= DSS_HEADER_SIZE) {
    pbuf_copy_partial(p, buf, total, 0);
    if (get16(buf) == DSS_MAGIC && buf[3] == DSS_VERSION) {
      type = buf[2];
      session = get16(buf + 4);
      length = get16(buf + 6);
      body = buf + DSS_HEADER_SIZE;
      if (DSS_HEADER_SIZE + length <= total) {
        /* TABLE pairs us and sets the session; everything else must match it */
        if (type == DSS_T_TABLE) {
          g_paired = 1;
          g_ever_paired = 1;
          g_session = session;
          g_lost_ms = 0;
          ip_addr_copy(g_phone_ip, *addr);
          g_phone_port = port;
          g_phone_known = 1;
          g_last_phone_ms = 0;        /* set by the caller's clock next service */
          on_table(body, length);
        } else if (type == DSS_T_PROBE) {
          /* a phone looking for us: answer it directly, and keep telling it */
          if (!g_paired) {
            ip_addr_copy(g_phone_ip, *addr);
            g_phone_port = port;
            g_phone_known = 1;
            send_hello();
          }
        } else if (g_paired && session == g_session) {
          switch (type) {
            case DSS_T_PEEK:      on_peek(body, length); break;
            case DSS_T_CONTROL:   on_control(body, length); break;
            case DSS_T_CUE:       on_cue(body, length); break;
            case DSS_T_POPUP:     dscm_popup_on_popup(body, length); break;
            case DSS_T_BADGE:     dscm_popup_on_badge(body, length); break;
            case DSS_T_REPLAY_REQ:  on_replay_req(body, length, g_now_ms); break;
            case DSS_T_REPLAY_DONE: on_replay_done(body, length, g_now_ms); break;
            case DSS_T_KEEPALIVE:
              /* the last frame the phone evaluated, into the recording's header (no
               * frame of ours yet past it: not a stale number from an earlier run) */
              if (length >= 4 && get32(body) <= g_frame_sent)
                dscm_rec_phone_frame(get32(body));
              break;
            default: break;
          }
        }
        if (g_paired && session == g_session)
          g_last_phone_ms = 1;         /* marker: refreshed; stream_service sets the real time */
      }
    }
  }
  pbuf_free(p);
}

/* ---- entry points for module.c ---- */

void dscm_stream_start(const char* hash, const char* game_code, const char* title, int dtcm_known)
{
  memset(&g_hello, 0, sizeof(g_hello));
  g_hello.caps = (uint8_t)((dtcm_known ? DSS_CAP_DTCM : 0) | DSS_CAP_POPUP);
  g_hello.max_table_words = DSS_MAX_TABLE_WORDS;
  g_hello.max_chain_nodes = 0;
  if (hash)  memcpy(g_hello.hash, hash, strnlen(hash, sizeof(g_hello.hash)));
  if (game_code) memcpy(g_hello.game_code, game_code, strnlen(game_code, sizeof(g_hello.game_code)));
  if (title) memcpy(g_hello.title, title, strnlen(title, sizeof(g_hello.title)));
}

/* The recording's ring (the module's view), from module.c once the stream
 * is set up: from now on every frame is recorded, and HELLO says so with
 * this run's id (new each game start, never 0). */
void dscm_stream_record(void* ring, uint32_t size, uint32_t seed)
{
  dscm_rec_t* rec;
  if (size < 16 * DSS_REC_SECTOR)
    return;
  g_run = (uint16_t)(seed ^ (seed >> 16));
  if (!g_run)
    g_run = 1;
  dscm_rec_init(ring, size, g_run, g_hello.hash[0] ? g_hello.hash : NULL);
  rec = dscm_rec_header();
  if (!rec)
    return;
  dscm_publish_rec((void*)DSCM_PHYS(rec));
  g_recording = 1;
  g_hello.caps |= DSS_CAP_RECORD;
  g_hello.run = g_run;
}

/* The phone the launcher found before the game (network byte order; 0 none):
 * HELLO goes to it directly as well as to everyone. */
void dscm_stream_set_phone(uint32_t ip)
{
  if (!ip)
    return;
  ip_addr_set_ip4_u32(&g_phone_ip, ip);
  g_phone_port = DSS_PORT;
  g_phone_known = 1;
}

int dscm_stream_net_init(void)
{
  g_pcb = udp_new();
  if (!g_pcb)
    return -1;
  if (udp_bind(g_pcb, IP_ANY_TYPE, DSS_PORT) != ERR_OK) {
    udp_remove(g_pcb);
    g_pcb = NULL;
    return -1;
  }
  udp_recv(g_pcb, on_udp, NULL);
  return 0;
}

/* The console's own notice that the phone is gone (no unlocks are checked
 * until it is back), at the loss and again 2 minutes on if it still is. The
 * value at the end of its first line is the link in short, for when the
 * phone never hears from us again: R the firmware's reason for the
 * disconnect that took the link down (2 link lost, 4 the AP dropped us...),
 * K AP key messages we couldn't answer (a group rekey: we left to rejoin), J
 * the association state (0 up .. 5 given up), C connect events (rejoins that
 * associated). The phone logs everything else once it hears from us again. */
static void lost_notice(uint32_t now_ms)
{
  static const uint32_t at_ms[2] = { 0, 120000 };
  dss_link_status_t l;
  char value[DSS_POPUP_VALUE_MAX + 1];
  if (!g_lost_ms || g_lost_shown >= 2 || g_reconnect_ms || now_ms - g_lost_ms < at_ms[g_lost_shown])
    return;
  link_diag(&l);
  snprintf(value, sizeof(value), "R%u K%u J%u C%u", (unsigned)l.cause_reason, (unsigned)l.need_ptk, (unsigned)l.join,
           (unsigned)l.connect_events);
  g_lost_shown++;
  dscm_popup_local("Phone not reachable", g_recording ? "Recording: unlocks will follow" : "Achievements paused for now", value, 600);
}

/* The link in one line of text in the recording's header on the SD card
 * (record.c dscm_rec_note), for a run the phone stopped hearing: line 0 the
 * latest (at the STATUS cadence and whenever the link goes up or down), line
 * 1 as it was when the pairing was lost. R K J C as in the notice, plus the
 * 802.11 code after R; L the latest disconnect, D disconnect events, T
 * association/handshake timeouts, F shim flags (1 attached, 2 dead, 4 run),
 * q TX/RX ring slots in use; then the shim's counters (full: RX frames thrown
 * away with the ring full; int: the chip's last interrupt status; pw: VBlanks
 * paused for the game's DS wireless). */
static void link_note(uint32_t line, uint32_t now_ms)
{
  static char text[DSCM_REC_NOTE_BYTES];
  static dss_link_status_t l;
  if (!g_recording)
    return;
  link_diag(&l);
  snprintf(text, sizeof(text),
           "%s t=%us R%u/%u L%u/%u K%u J%u C%u D%u T%u/%u F%u rej%u q%u/%u polls%u rx%u tx%u full%u sdio%u/%x int%x pw%u",
           line ? "lost" : "now", (unsigned)(now_ms / 1000), (unsigned)l.cause_reason, (unsigned)l.cause_proto,
           (unsigned)l.last_reason, (unsigned)l.last_proto, (unsigned)l.need_ptk, (unsigned)l.join,
           (unsigned)l.connect_events, (unsigned)l.disc_events, (unsigned)l.join_timeouts, (unsigned)l.key_timeouts,
           (unsigned)l.shim_flags, (unsigned)l.rejoins, (unsigned)l.tx_queued, (unsigned)l.rx_queued,
           (unsigned)l.shim_polls, (unsigned)l.shim_rx, (unsigned)l.shim_tx, (unsigned)l.shim_rx_ring_full,
           (unsigned)l.shim_sdio_errors, (unsigned)l.shim_last_error, (unsigned)l.shim_last_int,
           (unsigned)l.shim_paused_wifi);
  dscm_rec_note(line, text);
}

/* "Reconnect" in nds-bootstrap's in-game menu (module.c, at the next idle
 * pass): a fresh Wi-Fi association with the rejoin's pause back to its
 * shortest, and a notice so the player sees it started. HELLO goes out every
 * DSS_HELLO_MS while unpaired, so a phone back on the network pairs again. */
void dscm_stream_reconnect(void)
{
  link_reconnect();
  g_reconnect_ms = g_now_ms ? g_now_ms : 1;
  dscm_popup_local("Reconnecting...", "Looking for the phone", "", 180);
}

/* After a reconnect: "Reconnected" once the phone is heard on the new
 * association; after RECONNECT_WAIT_MS without it, the link-lost notice
 * (held back meanwhile) says the rest. */
static void reconnect_check(uint32_t now_ms, int link_is_up)
{
  if (!g_reconnect_ms)
    return;
  if (g_paired && link_is_up && (int32_t)(g_last_phone_ms - g_reconnect_ms) > 0) {
    g_reconnect_ms = 0;
    g_lost_ms = 0;
    dscm_popup_local("Reconnected", "The phone is back", "", 180);
  } else if (now_ms - g_reconnect_ms > RECONNECT_WAIT_MS) {
    g_reconnect_ms = 0;
  }
}

/* Called each idle pass after net_service(now_ms): the recv callback has
 * already run inside it. Keeps the pairing alive and broadcasts HELLO. */
void dscm_stream_service(uint32_t now_ms)
{
  static int link_was_up = 1;
  static uint32_t last_pass_ms;
  const int link_is_up = link_up();
  g_now_ms = now_ms;
  if (!g_start_ms)
    g_start_ms = now_ms ? now_ms : 1;
  /* The game gave the module no idle time for a while (HeartGold while it
   * saves: about 7 s): nothing was heard or sent meanwhile (the chip's RX
   * filled up), so that time doesn't count towards losing the phone, nor
   * towards giving a replay up; no "Phone not reachable" for it. */
  if (last_pass_ms && now_ms - last_pass_ms > SERVICE_GAP_MS) {
    if (g_paired)
      g_last_phone_ms = now_ms ? now_ms : 1;
    if (g_replaying)
      g_replay_ms = now_ms;
  }
  last_pass_ms = now_ms ? now_ms : 1;
  if (link_is_up != link_was_up) {
    link_was_up = link_is_up;
    link_note(0, now_ms);
    if (link_is_up && g_reconnect_ms)
      g_reconnect_ms = now_ms ? now_ms : 1;   /* the phone must be heard on this association */
  }
  if (g_last_phone_ms == 1)         /* the callback heard from the phone this pass */
    g_last_phone_ms = now_ms ? now_ms : 1;
  {
    /* While the game's own DS wireless has the radio (the card engine leaves the chip
     * alone: HeartGold switched it on for 4 s in play), nothing can be heard: that time
     * doesn't count towards losing the phone, and no "Phone not reachable" for it. */
    static uint32_t paused_seen;
    const uint32_t paused = link_paused_count();
    if (paused != paused_seen) {
      paused_seen = paused;
      if (g_paired)
        g_last_phone_ms = now_ms ? now_ms : 1;
      if (!g_ever_paired)
        g_start_ms = now_ms ? now_ms : 1;
    }
  }
  reconnect_check(now_ms, link_is_up);
  dscm_g_status.net_detail = g_paired ? g_session : 0;   /* nonzero in the menu = paired */
  if (g_replaying && now_ms - g_replay_ms > DSS_REPLAY_TIMEOUT_MS) {
    g_replaying = 0;                /* the phone stopped reading: live again, from a FULL */
    g_have_last = 0;
  }
  if (g_paired) {
    if (now_ms - g_last_phone_ms > DSS_PAIR_TIMEOUT_MS) {
      g_paired = 0;
      g_session = 0;
      g_have_last = 0;
      g_replaying = 0;
      g_lost_ms = now_ms ? now_ms : 1;
      g_lost_shown = 0;
      link_note(1, now_ms);
    } else {
      send_boot_log(now_ms);
    }
    return;
  }
  if (!g_ever_paired && !g_lost_ms && now_ms - g_start_ms >= FIRST_PAIR_WAIT_MS) {
    g_lost_ms = now_ms ? now_ms : 1;   /* never found: the same notice, now and 2 minutes on */
    g_lost_shown = 0;
  }
  lost_notice(now_ms);
  if (now_ms - g_last_hello_ms >= DSS_HELLO_MS) {
    g_last_hello_ms = now_ms;
    send_hello();
  }
}

/* Called once per new frame (module.c, from the idle/halt path). */
void dscm_stream_frame(uint32_t frame)
{
  static uint8_t cur[DSS_MAX_TABLE_WORDS * 4];
  const int record = g_recording && g_watch_count;
  g_frame_sent = frame;
  g_hello.vblank = frame;
  if (!g_paired && !record)
    return;
  g_capture_seq++;                  /* a FRAME the phone never gets shows there as a step of two or more */
  read_words(cur, g_watch_count);
  /* recorded first, always: the phone may not hear the frame sent next */
  if (record)
    dscm_rec_frame(frame, g_watch, g_watch_count, g_generation, cur);
  if (g_paired && !g_replaying)
    send_frame(frame, cur);
}

/* STATUS for the phone's cart screen (every DSS_FULL_EVERY frames). */
void dscm_stream_status(uint32_t frame, uint32_t net_state, uint32_t idle_lines_max)
{
  /* static: about 500 bytes, off the module's small stack (idle path only) */
  static uint8_t buf[DSS_HEADER_SIZE + sizeof(dss_status_t) + sizeof(dss_display_status_t) + sizeof(dss_link_status_t)];
  static dss_display_status_t d;
  static dss_link_status_t l;
  dss_status_t s;
  uint8_t* body;
  link_note(0, g_now_ms);
  if (!g_paired)
    return;
  link_diag(&l);
  memset(&s, 0, sizeof(s));
  s.vblank = frame;
  s.frames_sent = g_frames_sent_total;
  s.frames_stale = g_frames_stale_total;
  s.peeks_answered = g_peeks_answered;
  s.net_rejoins = l.rejoins;
  s.net_state = (uint8_t)net_state;
  s.shim_dead = (uint8_t)((l.shim_flags & 2) ? 1 : 0);
  s.idle_lines_max = idle_lines_max;
  dscm_popup_status(&d);
  body = header(buf, DSS_T_STATUS, g_session, (uint16_t)(sizeof(dss_status_t) + sizeof(d) + sizeof(l)));
  memcpy(body, &s, sizeof(dss_status_t));
  memcpy(body + sizeof(dss_status_t), &d, sizeof(d));   /* DSS_CAP_POPUP: the strip's counters */
  memcpy(body + sizeof(dss_status_t) + sizeof(d), &l, sizeof(l));   /* the Wi-Fi link's */
  send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)sizeof(buf));
}
