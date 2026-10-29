#include "phone.h"
#include "config.h"
#include "ds_stream_protocol.h"
#include "log.h"
#include "recording.h"
#include "ui.h"

#include <nds.h>
#include <dswifi9.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* A replay the phone is to start or go on with must come within this many
 * frames: after READY (or the last piece sent), else the game starts; and
 * the whole replay gets REPLAY_MAX_FRAMES from the phone's first word,
 * however often it asks. B cuts it short. */
#define REPLAY_GRACE_FRAMES   60
#define REPLAY_TIMEOUT_FRAMES ((int)(DSS_REPLAY_TIMEOUT_MS * 60 / 1000))
#define REPLAY_MAX_FRAMES     (20 * 60)
#define PLAIN_HELLO_FRAMES    (3 * 60)    /* the game's own HELLO after an offered recording: its READY by then */

void phone_ip_text(uint32_t ip, char out[16])
{
  const uint8_t* b = (const uint8_t*)&ip;
  snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

uint32_t phone_ip_parse(const char* text)
{
  unsigned a, b, c, d;
  uint32_t ip;
  uint8_t* p = (uint8_t*)&ip;
  if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return 0;
  p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = (uint8_t)c; p[3] = (uint8_t)d;
  return ip;
}

#define PHONE_PATH DSC_DIR "phone.txt"

uint32_t phone_remembered(void)
{
  char line[32];
  uint32_t ip = 0;
  FILE* f = fopen(PHONE_PATH, "r");
  if (f) {
    if (fgets(line, sizeof(line), f))
      ip = phone_ip_parse(line);
    fclose(f);
  }
  return ip;
}

void phone_remember(uint32_t ip)
{
  char text[16];
  FILE* f;
  if (!ip || ip == phone_remembered())
    return;
  phone_ip_text(ip, text);
  f = fopen(PHONE_PATH, "w");
  if (f) {
    fprintf(f, "%s\n", text);
    fclose(f);
  }
}

static void make_header(uint8_t* buf, uint8_t type, uint16_t length)
{
  put16(buf, DSS_MAGIC);
  buf[2] = type;
  buf[3] = DSS_VERSION;
  put16(buf + 4, 0);
  put16(buf + 6, length);
}

/* The launcher's HELLO: the game's hash, code and title, as the module sends
 * them once the game runs, with DSS_CAP_LAUNCHER so the phone answers READY
 * and waits for the game instead of pairing. With a recording to offer, the
 * recording's run, hash, code and title instead, and DSS_CAP_RECORD: the
 * phone may ask for it (REPLAY_REQ) before the game starts. */
static void make_hello(uint8_t* buf, const char* rom_path, const char* hash, const recording_t* rec)
{
  dss_hello_t h;
  char header[16];
  FILE* f;
  memset(&h, 0, sizeof(h));
  h.caps = DSS_CAP_LAUNCHER;
  h.max_table_words = DSS_MAX_TABLE_WORDS;
  if (rec) {
    h.caps |= DSS_CAP_RECORD;
    h.run = rec->h.run;
    memcpy(h.hash, rec->h.game_hash, sizeof(h.hash));
    memcpy(h.title, rec->h.title, sizeof(h.title));
    memcpy(h.game_code, rec->h.game_code, sizeof(h.game_code));
  } else {
    memcpy(h.hash, hash, strnlen(hash, sizeof(h.hash)));
    f = fopen(rom_path, "rb");
    if (f) {
      if (fread(header, 1, sizeof(header), f) == sizeof(header)) {
        memcpy(h.title, header, sizeof(h.title));
        memcpy(h.game_code, header + 0x0C, sizeof(h.game_code));
      }
      fclose(f);
    }
  }
  make_header(buf, DSS_T_HELLO, (uint16_t)sizeof(h));
  memcpy(buf + DSS_HEADER_SIZE, &h, sizeof(h));
}

/* Answers a REPLAY_REQ (or sends the piece after a REPLAY_DONE that isn't
 * the end) with one REPLAY_DATA to the phone. */
static void send_piece(int fd, recording_t* rec, const dss_replay_req_t* req, const struct sockaddr_in* to)
{
  static uint8_t packet[DSS_HEADER_SIZE + sizeof(dss_replay_data_t) + DSS_REPLAY_CHUNK];
  dss_replay_data_t reply;
  int len = recording_reply(rec, req, &reply, packet + DSS_HEADER_SIZE + sizeof(reply));
  make_header(packet, DSS_T_REPLAY_DATA, (uint16_t)(sizeof(reply) + len));
  memcpy(packet + DSS_HEADER_SIZE, &reply, sizeof(reply));
  sendto(fd, packet, DSS_HEADER_SIZE + sizeof(reply) + len, 0, (const struct sockaddr*)to, sizeof(*to));
}

int phone_find(const char* rom_path, const char* hash, uint32_t known_ip, int timeout_frames,
               int (*each_frame)(void), phone_t* out, recording_t* rec)
{
  uint8_t hello[DSS_HEADER_SIZE + sizeof(dss_hello_t)];
  uint8_t buf[256];
  struct sockaddr_in any, to, from;
  int fd, on = 1, frame, found = -1, stop = 0, told = 0;
  int replay_until = -1;   /* the frame by which the phone must ask for (more of) the recording; -1: not waiting */
  int replay_end = -1;     /* the frame the replay ends at the latest, from the phone's first word */
  int plain_until = -1;    /* >= 0: the HELLO out there is the game's own, after the recording's; its READY by then */
  int offered;

  if (rec && rec->done)
    rec = NULL;
  offered = rec != NULL;
  make_hello(hello, rom_path, hash, rec);
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    log_line("phone: no socket");
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
  memset(&any, 0, sizeof(any));
  any.sin_family = AF_INET;
  any.sin_port = htons(DSS_PORT);
  any.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr*)&any, sizeof(any)) != 0) {
    log_line("phone: can't bind %d", DSS_PORT);
    close(fd);
    return -1;
  }

  for (frame = 0; !stop; frame++) {
    if (plain_until >= 0) {
      /* the game's HELLO after the recording's: its READY, or the first one stands */
      if (found == 0 || frame >= plain_until) {
        found = 0;
        break;
      }
    } else if (found < 0 && timeout_frames >= 0 && frame >= timeout_frames) {
      break;
    } else if (found == 0 && (replay_until < 0 || frame >= replay_until)) {
      if (!offered)
        break;
      /* the READY so far answered the recording's game: say HELLO for the game
       * about to start, and take its READY (name, login) for the screen */
      rec = NULL;
      offered = 0;
      make_hello(hello, rom_path, hash, NULL);
      plain_until = frame + PLAIN_HELLO_FRAMES;
      found = -1;
    }
    if (rec && replay_until >= 0) {
      /* B cuts the replay short (held: a scan here must not eat the press
       * each_frame looks for) */
      scanKeys();
      if (keysHeld() & KEY_B) {
        log_line("phone: replay cut short by B");
        rec = NULL;
        replay_until = found == 0 ? frame : -1;
      }
    }
    if (found < 0 && frame % 15 == 0) {
      /* to everyone, and straight to the phone of last time: with its screen
       * off a phone drops broadcasts but still takes unicast */
      to = any;
      to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
      sendto(fd, hello, sizeof(hello), 0, (struct sockaddr*)&to, sizeof(to));
      if (known_ip) {
        to.sin_addr.s_addr = known_ip;
        sendto(fd, hello, sizeof(hello), 0, (struct sockaddr*)&to, sizeof(to));
      }
    }
    for (;;) {
      socklen_t from_len = sizeof(from);
      int n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&from, &from_len);
      const uint8_t* body = buf + DSS_HEADER_SIZE;
      uint16_t length;
      if (n < 0)
        break;
      if (n < DSS_HEADER_SIZE || get16(buf) != DSS_MAGIC || buf[3] != DSS_VERSION)
        continue;
      length = get16(buf + 6);
      if (DSS_HEADER_SIZE + length > n)
        continue;
      if (buf[2] == DSS_T_PROBE) {
        /* a phone looking for consoles: tell it about us directly */
        sendto(fd, hello, sizeof(hello), 0, (struct sockaddr*)&from, sizeof(from));
      } else if (buf[2] == DSS_T_READY && length >= 1) {
        if (found < 0) {
          memset(out, 0, sizeof(*out));
          out->ip = from.sin_addr.s_addr;
          out->state = body[0];
          if (length >= 4)
            memcpy(out->name, body + 4, length - 4 < 28 ? length - 4 : 28);
          found = 0;
          /* the phone may want the recording first: a moment for it to ask */
          if (rec && !rec->done && replay_until < 0) {
            if (replay_end < 0)
              replay_end = frame + REPLAY_MAX_FRAMES;
            replay_until = frame + REPLAY_GRACE_FRAMES;
          }
        }
      } else if (buf[2] == DSS_T_REPLAY_REQ && length >= sizeof(dss_replay_req_t) && rec) {
        dss_replay_req_t req;
        memcpy(&req, body, sizeof(req));
        if (!told && req.run == rec->h.run) {
          ui_status("Replaying last game to phone...");   /* 32 columns */
          told = 1;
        }
        send_piece(fd, rec, &req, &from);
        if (req.run == rec->h.run) {
          if (replay_end < 0)
            replay_end = frame + REPLAY_MAX_FRAMES;
          replay_until = rec->done ? -1 : frame + REPLAY_TIMEOUT_FRAMES;
        }
      } else if (buf[2] == DSS_T_REPLAY_DONE && length >= sizeof(dss_replay_done_t) && rec) {
        dss_replay_done_t done;
        int r;
        memcpy(&done, body, sizeof(done));
        r = recording_done(rec, &done);
        if (r == 1) {
          ui_status("\x1b[32;1mThe last game is on the phone\x1b[39;0m");
          replay_until = found == 0 ? frame + 1 : -1;
        } else if (r == 0) {
          dss_replay_req_t req;
          memset(&req, 0, sizeof(req));
          req.run = done.run;
          req.max_len = DSS_REPLAY_CHUNK;
          req.stream = done.stream;
          req.offset = done.offset;
          send_piece(fd, rec, &req, &from);
          replay_until = frame + REPLAY_TIMEOUT_FRAMES;
        }
      }
      /* however often the phone asks, the replay ends by replay_end */
      if (rec && replay_until >= 0 && replay_end >= 0 && replay_until > replay_end) {
        replay_until = replay_end;
        if (frame >= replay_end && !rec->done) {
          log_line("phone: replay cut short after %d s", REPLAY_MAX_FRAMES / 60);
          rec = NULL;
        }
      }
    }
    if (each_frame)
      stop = each_frame();
    cothread_yield_irq(IRQ_VBLANK);   /* lets DSWiFi's thread run (see net_connect) */
  }
  close(fd);
  if (found == 0) {
    char ip[16];
    phone_ip_text(out->ip, ip);
    log_line("phone: %s at %s, state %d, after %d frames", out->name, ip, out->state, frame);
  } else {
    log_line("phone: no answer in %d frames%s", frame, stop ? " (stopped)" : "");
  }
  return found;
}
