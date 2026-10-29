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
void dscm_popup_local(const char* title, const char* detail, const char* value);

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

/* HELLO payload, filled at start */
static dss_hello_t g_hello;
static uint32_t    g_frame_sent;       /* last frame number streamed */
static uint32_t    g_frames_sent_total, g_frames_stale_total, g_peeks_answered;

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

/* Read the watched words into out (table order); IRQs masked so no VBlank
 * splits the copy. Returns 0 if every word read, else the first bad index + 1
 * (an unreadable word reads as zero and the frame is still sent). */
static void read_words(uint8_t* out, uint16_t count)
{
  uint32_t cpsr = dscm_irq_off();
  uint16_t i;
  for (i = 0; i < count; i++) {
    if (!dscm_stream_read_bytes(g_watch[i], out + i * 4, 4))
      memset(out + i * 4, 0, 4);
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
static void send_frame(uint32_t frame)
{
  static uint8_t cur[DSS_MAX_TABLE_WORDS * 4];
  uint8_t buf[DSS_MAX_PAYLOAD];
  uint16_t n = g_watch_count;
  int fits_full = (DSS_HEADER_SIZE + 10 + (int)n * 4 <= DSS_MAX_PAYLOAD);
  read_words(cur, n);

  if (fits_full && (g_force_full || !g_have_last)) {
    uint8_t* body = header(buf, DSS_T_FRAME, g_session, (uint16_t)(10 + n * 4));
    put32(body + 0, frame);
    put16(body + 4, g_generation);
    body[6] = DSS_FRAME_FULL;
    body[7] = 0;
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
    body[6] = 0;                     /* DELTA */
    body[7] = 0;
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
            case DSS_T_KEEPALIVE: break;   /* liveness only */
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

/* The console's own notice that the phone is gone, with the link's state in
 * short (the phone can't log it any more): R the firmware's reason / 802.11
 * code of the disconnect that took the link down, L the latest disconnect's,
 * K AP key messages we couldn't answer (a group rekey: we left to rejoin), C
 * connect events (rejoins associating), T association / handshake timeouts,
 * J association state, P the ARM7 shim's polls (moving: the shim runs). The
 * value counts the notices: three, at the loss and 30 s and 90 s after, so
 * the numbers' movement shows too. */
static void lost_notice(uint32_t now_ms)
{
  static const uint32_t at_ms[3] = { 0, 30000, 90000 };
  dss_link_status_t l;
  char detail[48], value[8];
  if (!g_lost_ms || g_lost_shown >= 3 || now_ms - g_lost_ms < at_ms[g_lost_shown])
    return;
  link_diag(&l);
  snprintf(detail, sizeof(detail), "R%u/%u L%u/%u K%u C%u T%u/%u J%u P%u",
           (unsigned)l.cause_reason, (unsigned)l.cause_proto, (unsigned)l.last_reason, (unsigned)l.last_proto,
           (unsigned)l.need_ptk, (unsigned)l.connect_events, (unsigned)l.join_timeouts, (unsigned)l.key_timeouts,
           (unsigned)l.join, (unsigned)(l.shim_polls % 100000u));
  g_lost_shown++;
  snprintf(value, sizeof(value), "#%u", (unsigned)g_lost_shown);
  dscm_popup_local("Phone link lost", detail, value);
}

/* Called each idle pass after net_service(now_ms): the recv callback has
 * already run inside it. Keeps the pairing alive and broadcasts HELLO. */
void dscm_stream_service(uint32_t now_ms)
{
  if (g_last_phone_ms == 1)         /* the callback heard from the phone this pass */
    g_last_phone_ms = now_ms ? now_ms : 1;
  dscm_g_status.net_detail = g_paired ? g_session : 0;   /* nonzero in the menu = paired */
  if (g_paired) {
    if (now_ms - g_last_phone_ms > DSS_PAIR_TIMEOUT_MS) {
      g_paired = 0;
      g_session = 0;
      g_have_last = 0;
      g_lost_ms = now_ms ? now_ms : 1;
      g_lost_shown = 0;
    }
    return;
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
  g_frame_sent = frame;
  g_hello.vblank = frame;
  if (g_paired)
    send_frame(frame);
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
