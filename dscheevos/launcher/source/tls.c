/* HTTPS through mbedTLS: TLS 1.2, certificate chain and host name checked
 * against the bundled roots (tls/cacerts.c) plus sd:/_nds/dscheevos/ca.pem
 * if present (for a self-hosted or test server). */
#include "tls.h"
#include "config.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <dswifi9.h>
#include <nds.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "dsc_cacerts.h"

struct tls_t {
  int fd;
  mbedtls_ssl_context ssl;
};

static int g_ready;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_cas;
static mbedtls_ssl_config g_conf;

/* ---- entropy ----
 * The DS has no random number generator. Network events (DNS, TCP connect,
 * packet arrival) land at times the ARM9 can't predict, so the launcher
 * stirs a fast timer into a pool at each one (tls_stir), and on top of
 * that the RTC and the console's MAC address. */
static mbedtls_sha256_context g_pool;
static int g_pool_ready;

void tls_stir(void)
{
  u32 sample[3];
  if (!g_pool_ready) {
    u8 mac[6] = {0};
    mbedtls_sha256_init(&g_pool);
    mbedtls_sha256_starts(&g_pool, 0);
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);
    mbedtls_sha256_update(&g_pool, mac, sizeof(mac));
    if (!(TIMER_CR(0) & TIMER_ENABLE))   /* log.c's clock may run already: keep it */
      cpuStartTiming(0);
    g_pool_ready = 1;
  }
  sample[0] = cpuGetTiming();
  sample[1] = (u32)time(NULL);
  sample[2] = REG_VCOUNT | ((u32)TIMER_DATA(0) << 16);
  mbedtls_sha256_update(&g_pool, (const unsigned char*)sample, sizeof(sample));
}

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
  mbedtls_sha256_context copy;
  unsigned char digest[32];
  (void)data;
  tls_stir();
  mbedtls_sha256_clone(&copy, &g_pool);
  mbedtls_sha256_finish(&copy, digest);
  mbedtls_sha256_free(&copy);
  tls_stir();   /* never hand out the same state twice */
  if (len > sizeof(digest)) len = sizeof(digest);
  memcpy(output, digest, len);
  *olen = len;
  return 0;
}

/* ---- BIO over the lwIP socket ---- */

static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
  int n = (int)send(((tls_t*)ctx)->fd, buf, len, 0);
  tls_stir();
  return n < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : n;
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
  int n = (int)recv(((tls_t*)ctx)->fd, buf, len, 0);
  tls_stir();
  return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
}

static int setup(const char** error)
{
  static const char pers[] = "DScheevos launcher";
  FILE* f;
  if (g_ready)
    return 0;
  mbedtls_entropy_init(&g_entropy);
  mbedtls_ctr_drbg_init(&g_drbg);
  mbedtls_x509_crt_init(&g_cas);
  mbedtls_ssl_config_init(&g_conf);
  if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char*)pers, sizeof(pers) - 1) != 0) {
    *error = "TLS: random number setup failed";
    return -1;
  }
  if (dsc_parse_der_list(&g_cas, dsc_cacerts, dsc_cacerts_size, 1) != 0) {
    *error = "TLS: bad built-in certificates";
    return -1;
  }
  f = fopen(DSC_DIR "ca.pem", "rb");
  if (f) {
    static char extra[8192];
    size_t n = fread(extra, 1, sizeof(extra) - 1, f);
    fclose(f);
    extra[n] = '\0';
    mbedtls_x509_crt_parse(&g_cas, (const unsigned char*)extra, n + 1);
  }
  if (mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    *error = "TLS: config failed";
    return -1;
  }
  mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&g_conf, &g_cas, NULL);
  mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
  g_ready = 1;
  return 0;
}

tls_t* tls_open(int fd, const char* host, const char** error)
{
  tls_t* t;
  int r;
  if (setup(error) != 0)
    return NULL;
  t = (tls_t*)calloc(1, sizeof(*t));
  if (!t) {
    *error = "Out of memory";
    return NULL;
  }
  t->fd = fd;
  mbedtls_ssl_init(&t->ssl);
  if (mbedtls_ssl_setup(&t->ssl, &g_conf) != 0 || mbedtls_ssl_set_hostname(&t->ssl, host) != 0) {
    *error = "TLS: out of memory";
    goto fail;
  }
  mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
  while ((r = mbedtls_ssl_handshake(&t->ssl)) != 0) {
    if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
      *error = (mbedtls_ssl_get_verify_result(&t->ssl) != 0)
        ? "TLS: the server's certificate isn't trusted" : "TLS: secure connection failed";
      goto fail;
    }
  }
  return t;
fail:
  mbedtls_ssl_free(&t->ssl);
  free(t);
  return NULL;
}

int tls_write(tls_t* t, const char* data, size_t len)
{
  int r;
  do r = mbedtls_ssl_write(&t->ssl, (const unsigned char*)data, len);
  while (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ);
  return r;
}

int tls_read(tls_t* t, char* buf, size_t len)
{
  int r;
  do r = mbedtls_ssl_read(&t->ssl, (unsigned char*)buf, len);
  while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
  if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    return 0;
  return r;
}

void tls_close(tls_t* t)
{
  mbedtls_ssl_close_notify(&t->ssl);
  mbedtls_ssl_free(&t->ssl);
  free(t);
}
