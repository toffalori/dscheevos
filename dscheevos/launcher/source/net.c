#include "net.h"
#include "clock.h"
#include "tls.h"
#include "inflate.h"

#include <nds.h>
#include <dswifi9.h>
#include <ctype.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_BODY (1024 * 1024)

static char g_user_agent[128] = "DScheevos";
static const char* g_error = "";

const char* net_error(void) { return g_error; }

/* The console clock's offset from UTC, from the last server reply's Date. */
static int8_t g_offset_q;
static int g_offset_valid;

int net_clock_offset_q(int8_t* offset_q)
{
  if (g_offset_valid)
    *offset_q = g_offset_q;
  return g_offset_valid;
}

static void note_date(const char* headers)
{
  const char* d = strstr(headers, "\r\ndate:");
  uint32_t utc;
  int8_t q;
  if (d && clock_parse_http_date(d + 7, &utc) && clock_offset_q((uint32_t)time(NULL), utc, &q)) {
    g_offset_q = q;
    g_offset_valid = 1;
  }
}

static net_ap_t g_ap;
static int g_ap_valid;

/* dswifi joins the first scanned AP that matches a saved setting; take the
 * same one, right away, before old beacons age out of the list. */
static void remember_ap(void)
{
  int i, n = Wifi_GetNumAP();
  for (i = 0; i < n; i++) {
    Wifi_AccessPoint ap;
    memset(&ap, 0, sizeof(ap));
    if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK || !(ap.flags & WFLAG_APDATA_CONFIG_IN_WFC))
      continue;
    if (ap.ssid_len == 0 || ap.ssid_len > 32)
      continue;
    memset(&g_ap, 0, sizeof(g_ap));
    memcpy(g_ap.ssid, ap.ssid, ap.ssid_len);
    g_ap.ssid_len = ap.ssid_len;
    memcpy(g_ap.bssid, ap.bssid, 6);
    g_ap.channel = ap.channel;
    g_ap.security = (unsigned char)ap.security_type;
    g_ap.pair_cipher = (unsigned char)ap.pair_crypt_type;
    g_ap.group_cipher = (unsigned char)ap.group_crypt_type;
    g_ap_valid = 1;
    return;
  }
}

int net_current_ap(net_ap_t* out)
{
  if (!g_ap_valid)
    return -1;
  *out = g_ap;
  return 0;
}

/* DSWiFi starts once; a retry only searches again. (Wifi_InitDefault with
 * WFC_CONNECT also waits for the connection and fails when no saved access
 * point answers; calling it again then waits for ever for an ARM7 that is
 * already running.) */
int net_connect(int timeout_frames, void (*each_frame)(void))
{
  static int state;   /* 0 not started, 1 started, -1 failed to start */
  int i;
  if (state < 0) {
    g_error = "Wi-Fi didn't start: turn the console off and on";
    return -1;
  }
  if (state == 0) {
    if (!Wifi_InitDefault(WIFI_ATTEMPT_DSI_MODE)) {
      state = -1;
      g_error = "Wi-Fi didn't start: turn the console off and on";
      return -1;
    }
    state = 1;
  }
  Wifi_AutoConnect();   /* also restarts the search after "cannot connect" */
  for (i = 0; i < timeout_frames; i++) {
    int status = Wifi_AssocStatus();
    if (status == ASSOCSTATUS_ASSOCIATED) {
      remember_ap();
      tls_stir();
      return 0;
    }
    if (status == ASSOCSTATUS_CANNOTCONNECT) {
      g_error = "Couldn't connect to a saved access point";
      return -1;
    }
    if (each_frame)
      each_frame();
    /* not swiWaitForVBlank(): it halts every thread, and DSWiFi's lwIP thread
     * has to run for DHCP (the first attempt timed out) */
    cothread_yield_irq(IRQ_VBLANK);
  }
  g_error = "No saved access point answered";
  return -1;
}

void http_set_user_agent(const char* user_agent)
{
  strncpy(g_user_agent, user_agent, sizeof(g_user_agent) - 1);
}

/* ---- connection: plain socket or TLS ---- */

typedef struct conn_t {
  int fd;
  tls_t* tls;
} conn_t;

static int conn_send(conn_t* c, const char* data, size_t len)
{
  while (len) {
    int n = c->tls ? tls_write(c->tls, data, len) : (int)send(c->fd, data, len, 0);
    if (n <= 0)
      return -1;
    data += n;
    len -= (size_t)n;
  }
  return 0;
}

static int conn_recv(conn_t* c, char* buf, size_t len)
{
  return c->tls ? tls_read(c->tls, buf, len) : (int)recv(c->fd, buf, len, 0);
}

static int parse_url(const char* url, int* https, char* host, size_t host_size, char* port, const char** path)
{
  const char* p;
  size_t n;
  if (!strncmp(url, "https://", 8)) { *https = 1; url += 8; strcpy(port, "443"); }
  else if (!strncmp(url, "http://", 7)) { *https = 0; url += 7; strcpy(port, "80"); }
  else return -1;
  p = url;
  while (*p && *p != '/' && *p != ':') p++;
  n = (size_t)(p - url);
  if (n == 0 || n >= host_size) return -1;
  memcpy(host, url, n);
  host[n] = '\0';
  if (*p == ':') {
    const char* q = ++p;
    while (*p >= '0' && *p <= '9') p++;
    if (p == q || p - q > 5) return -1;
    memcpy(port, q, (size_t)(p - q));
    port[p - q] = '\0';
  }
  *path = *p ? p : "/";
  return 0;
}

static int open_socket(const char* host, const char* port)
{
  struct addrinfo hints, *res = NULL, *ai;
  int fd = -1;
  struct timeval tv = { 15, 0 };
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  tls_stir();
  if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
    g_error = "Couldn't look up the server";
    return -1;
  }
  for (ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
      continue;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      tls_stir();
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0)
    g_error = "Couldn't reach the server";
  return fd;
}

/* Reads the whole response (Connection: close) into a growing buffer. */
static char* read_all(conn_t* c, size_t* total)
{
  size_t cap = 16384, len = 0;
  char* buf = (char*)malloc(cap + 1);
  if (!buf)
    return NULL;
  for (;;) {
    int n;
    if (cap - len < 4096) {
      char* bigger;
      if (cap >= MAX_BODY) break;
      cap *= 2;
      bigger = (char*)realloc(buf, cap + 1);
      if (!bigger) break;
      buf = bigger;
    }
    n = conn_recv(c, buf + len, cap - len);
    if (n <= 0)
      break;
    len += (size_t)n;
  }
  buf[len] = '\0';
  *total = len;
  return buf;
}

/* Decodes a chunked body in place; returns its length. */
static size_t dechunk(char* body, size_t len)
{
  char* in = body;
  char* end = body + len;
  char* out = body;
  while (in < end) {
    char* next;
    unsigned long size = strtoul(in, &next, 16);
    char* line_end = strstr(in, "\r\n");
    if (!line_end || next == in) break;
    in = line_end + 2;
    if (size == 0 || in + size > end) break;
    memmove(out, in, size);
    out += size;
    in += size + 2;
  }
  *out = '\0';
  return (size_t)(out - body);
}

void http_request(const char* url, const char* post, const char* content_type, http_response_t* out)
{
  char host[96], port[8], request[512];
  const char* path;
  int https, header_len;
  conn_t c = { -1, NULL };
  char* raw;
  char* body;
  size_t raw_len;

  memset(out, 0, sizeof(*out));
  out->status = -1;
  if (parse_url(url, &https, host, sizeof(host), port, &path) != 0) {
    g_error = "Bad server address";
    return;
  }
  c.fd = open_socket(host, port);
  if (c.fd < 0)
    return;
  if (https) {
    c.tls = tls_open(c.fd, host, &g_error);
    if (!c.tls) {
      close(c.fd);
      return;
    }
  }

  header_len = snprintf(request, sizeof(request),
    "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nAccept-Encoding: gzip\r\nConnection: close\r\n",
    post ? "POST" : "GET", path, host, g_user_agent);
  if (post)
    header_len += snprintf(request + header_len, sizeof(request) - (size_t)header_len,
      "Content-Type: %s\r\nContent-Length: %u\r\n", content_type ? content_type : "application/x-www-form-urlencoded",
      (unsigned)strlen(post));
  header_len += snprintf(request + header_len, sizeof(request) - (size_t)header_len, "\r\n");

  if (header_len >= (int)sizeof(request) || conn_send(&c, request, (size_t)header_len) != 0
      || (post && conn_send(&c, post, strlen(post)) != 0)) {
    g_error = "Couldn't send the request";
    goto done;
  }

  raw = read_all(&c, &raw_len);
  if (!raw) {
    g_error = "Out of memory";
    goto done;
  }
  body = strstr(raw, "\r\n\r\n");
  if (strncmp(raw, "HTTP/1.", 7) != 0 || !body) {
    g_error = "Bad reply from the server";
    free(raw);
    goto done;
  }
  *body = '\0';
  body += 4;
  out->status = atoi(raw + 9);
  out->length = raw_len - (size_t)(body - raw);
  {
    /* headers are case-insensitive */
    char* h;
    for (h = raw; *h; h++) *h = (char)tolower((unsigned char)*h);
    note_date(raw);
    if (strstr(raw, "\r\ntransfer-encoding: chunked"))
      out->length = dechunk(body, out->length);
    /* asked for: an achievement set compresses about tenfold */
    if (strstr(raw, "\r\ncontent-encoding: gzip")) {
      size_t len;
      char* plain = gunzip((const uint8_t*)body, out->length, &len);
      free(raw);
      if (!plain) {
        g_error = "Bad compressed reply from the server";
        out->status = -1;
        goto done;
      }
      out->body = plain;
      out->length = len;
      goto done;
    }
  }
  memmove(raw, body, out->length + 1);
  out->body = raw;

done:
  if (c.tls)
    tls_close(c.tls);
  close(c.fd);
}

void http_free(http_response_t* r)
{
  free(r->body);
  r->body = NULL;
}
