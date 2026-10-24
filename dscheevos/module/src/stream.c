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
#include <string.h>

/* module.c */
extern int dscm_stream_read_bytes(uint32_t ra_addr, void* out, uint32_t len);
extern uint32_t dscm_irq_off(void);
extern void dscm_irq_restore(uint32_t cpsr);
extern dscm_status_t dscm_g_status;   /* the in-game menu reads it: show stream state here */

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
static uint32_t  g_last_phone_ms;
static uint32_t  g_last_hello_ms;
static int       g_force_full;

/* HELLO payload, filled at start */
static dss_hello_t g_hello;
static uint32_t    g_frame_sent;       /* last frame number streamed */
static uint32_t    g_frames_sent_total, g_frames_stale_total, g_peeks_answered;

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

static void send_hello(void)
{
  uint8_t buf[DSS_HEADER_SIZE + sizeof(dss_hello_t)];
  ip_addr_t bcast;
  uint8_t* body = header(buf, DSS_T_HELLO, 0, (uint16_t)sizeof(dss_hello_t));
  memcpy(body, &g_hello, sizeof(dss_hello_t));
  ip_addr_set_ip4_u32(&bcast, 0xFFFFFFFFu);   /* 255.255.255.255 */
  send_to(&bcast, DSS_PORT, buf, (uint16_t)sizeof(buf));
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
static void send_frame(uint32_t frame)
{
  static uint8_t cur[DSS_MAX_TABLE_WORDS * 4];
  uint8_t buf[DSS_MAX_PAYLOAD];
  uint16_t n = g_watch_count;
  int full;
  read_words(cur, n);

  full = g_force_full || !g_have_last || (DSS_HEADER_SIZE + 10 + n * 4 <= DSS_MAX_PAYLOAD && n * 4 + 10 + DSS_HEADER_SIZE <= DSS_MAX_PAYLOAD);
  /* keyframe cadence */
  if (!g_force_full && g_have_last && (frame % DSS_FULL_EVERY) != 0 && (DSS_HEADER_SIZE + 10 + n * 4 > DSS_MAX_PAYLOAD))
    full = 0;

  if (full) {
    uint8_t* body;
    uint16_t len = (uint16_t)(10 + n * 4);
    if (DSS_HEADER_SIZE + len > DSS_MAX_PAYLOAD)
      return;                        /* a full table too big for one packet: DELTA only (below) */
    body = header(buf, DSS_T_FRAME, g_session, len);
    put32(body + 0, frame);
    put16(body + 4, g_generation);
    body[6] = DSS_FRAME_FULL;
    body[7] = 0;
    put16(body + 8, n);
    memcpy(body + 10, cur, n * 4);
    send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)(DSS_HEADER_SIZE + len));
    g_force_full = 0;
  } else {
    /* DELTA: index (u16) + 4 bytes, for each changed word, as many as fit */
    uint8_t* body = header(buf, DSS_T_FRAME, g_session, 0);
    uint16_t changed = 0, i;
    uint8_t* w = body + 10;
    uint16_t room = (uint16_t)(DSS_MAX_PAYLOAD - DSS_HEADER_SIZE - 10);
    for (i = 0; i < n && (uint16_t)((changed + 1) * 6) <= room; i++) {
      if (memcmp(cur + i * 4, g_last + i * 4, 4) != 0) {
        put16(w, i);
        memcpy(w + 2, cur + i * 4, 4);
        w += 6;
        changed++;
      }
    }
    put32(body + 0, frame);
    put16(body + 4, g_generation);
    body[6] = 0;                     /* DELTA */
    body[7] = 0;
    put16(body + 8, changed);
    {
      uint16_t len = (uint16_t)(10 + changed * 6);
      put16(buf + 6, len);
      send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)(DSS_HEADER_SIZE + len));
    }
  }
  memcpy(g_last, cur, n * 4);
  g_have_last = 1;
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
    default: break;      /* DELTA on/off is automatic in this build */
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
          ip_addr_copy(g_phone_ip, *addr);
          g_phone_port = port;
          g_last_phone_ms = 0;        /* set by the caller's clock next service */
          on_table(body, length);
        } else if (g_paired && session == g_session) {
          switch (type) {
            case DSS_T_PEEK:      on_peek(body, length); break;
            case DSS_T_CONTROL:   on_control(body, length); break;
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
  g_hello.caps = dtcm_known ? DSS_CAP_DTCM : 0;
  g_hello.max_table_words = DSS_MAX_TABLE_WORDS;
  g_hello.max_chain_nodes = 0;
  if (hash)  memcpy(g_hello.hash, hash, strnlen(hash, sizeof(g_hello.hash)));
  if (game_code) memcpy(g_hello.game_code, game_code, strnlen(game_code, sizeof(g_hello.game_code)));
  if (title) memcpy(g_hello.title, title, strnlen(title, sizeof(g_hello.title)));
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
    }
    return;
  }
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
  uint8_t buf[DSS_HEADER_SIZE + sizeof(dss_status_t)];
  dss_status_t s;
  uint8_t* body;
  if (!g_paired)
    return;
  memset(&s, 0, sizeof(s));
  s.vblank = frame;
  s.frames_sent = g_frames_sent_total;
  s.frames_stale = g_frames_stale_total;
  s.peeks_answered = g_peeks_answered;
  s.net_rejoins = 0;
  s.net_state = (uint8_t)net_state;
  s.shim_dead = 0;
  s.idle_lines_max = idle_lines_max;
  body = header(buf, DSS_T_STATUS, g_session, (uint16_t)sizeof(dss_status_t));
  memcpy(body, &s, sizeof(dss_status_t));
  send_to(&g_phone_ip, g_phone_port, buf, (uint16_t)sizeof(buf));
}
