/*
 * Host tests for the WPA2 supplicant (src/net/wpa.c):
 *  - the PRF against the IEEE 802.11 test vectors
 *  - a real handshake: Wireshark's wpa-Induction.pcap sample (SSID "Coherer",
 *    passphrase "Induction"); the station's MICs must check out with the keys
 *    we derive, and the AP's message 3 must pass our checks
 *  - a simulated AP for what the capture lacks: group rekeys, replays,
 *    forged MICs, wrong nonces, unsupported descriptors
 */
#include "wpa.h"

#include "mbedtls/md.h"
#include "mbedtls/nist_kw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
  (void)data;
  memset(output, 0x5A, len);
  *olen = len;
  return 0;
}

static void unhex(const char* h, uint8_t* out)
{
  while (h[0] && h[1]) {
    unsigned v;
    sscanf(h, "%2x", &v);
    *out++ = (uint8_t)v;
    h += 2;
  }
}

static void hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* in, size_t len, uint8_t out[20])
{
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, key_len, in, len, out);
}

/* the MIC a frame should carry under kck */
static int mic_matches(const uint8_t kck[16], const uint8_t* frame, uint32_t len)
{
  uint8_t copy[512], d[20];
  memcpy(copy, frame, len);
  memset(copy + 81, 0, 16);
  hmac_sha1(kck, 16, copy, len, d);
  return memcmp(d, frame + 81, 16) == 0;
}

static void test_prf(void)
{
  uint8_t key[20], out[64], want[64];
  memset(key, 0x0b, sizeof(key));
  wpa_prf(key, 20, "prefix", (const uint8_t*)"Hi There", 8, out, 64);
  unhex("bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
        "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a", want);
  CHECK(memcmp(out, want, 64) == 0);
  wpa_prf((const uint8_t*)"Jefe", 4, "prefix", (const uint8_t*)"what do ya want for nothing?", 28, out, 64);
  unhex("51f4de5b33f249adf81aeb713a3c20f4fe631446fabdfa58244759ae58ef9009"
        "a99abf4eac2ca5fa87e692c440eb40023e7babb206d61de7b92f41529092b8fc", want);
  CHECK(memcmp(out, want, 64) == 0);
}

/* wpa-Induction.pcap, frames 86, 88, 91, 93 (EAPOL payloads) */
static const uint8_t cap_m1[121] =
  "\x02\x03\x00\x75\x02\x00\x8a\x00\x10\x00\x00\x00\x00\x00\x00\x00\x00\x3e\x8e\x96\x7d\xac\xd9\x60"
  "\x32\x4c\xac\x5b\x6a\xa7\x21\x23\x5b\xf5\x7b\x94\x97\x71\xc8\x67\x98\x9f\x49\xd0\x4e\xd4\x7c\x69"
  "\x33\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x16\xdd\x14\x00\x0f\xac\x04\x59\x2d\xa8\x80\x96\xc4\x61\xda\x24\x6c\x69\x00\x1e\x87\x7f"
  "\x3d";
static const uint8_t cap_m2[121] =
  "\x02\x03\x00\x75\x02\x01\x0a\x00\x10\x00\x00\x00\x00\x00\x00\x00\x00\xcd\xf4\x05\xce\xb9\xd8\x89"
  "\xef\x3d\xec\x42\x60\x98\x28\xfa\xe5\x46\xb7\xad\xd7\xba\xec\xbb\x1a\x39\x4e\xac\x52\x14\xb1\xd3"
  "\x86\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\xa4\x62\xa7\x02\x9a\xd5\xba\x30\xb6\xaf\x0d\xf3\x91\x98\x8e"
  "\x45\x00\x16\x30\x14\x01\x00\x00\x0f\xac\x02\x01\x00\x00\x0f\xac\x04\x01\x00\x00\x0f\xac\x02\x00"
  "\x00";
static const uint8_t cap_m3[179] =
  "\x02\x03\x00\xaf\x02\x13\xca\x00\x10\x00\x00\x00\x00\x00\x00\x00\x01\x3e\x8e\x96\x7d\xac\xd9\x60"
  "\x32\x4c\xac\x5b\x6a\xa7\x21\x23\x5b\xf5\x7b\x94\x97\x71\xc8\x67\x98\x9f\x49\xd0\x4e\xd4\x7c\x69"
  "\x33\xf5\x7b\x94\x97\x71\xc8\x67\x98\x9f\x49\xd0\x4e\xd4\x7c\x69\x34\xcf\x02\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x7d\x0a\xf6\xdf\x51\xe9\x9c\xde\x7a\x18\x74\x53\xf0\xf9\x35"
  "\x37\x00\x50\xcf\xa7\x2c\xde\x35\xb2\xc1\xe2\x31\x92\x55\x80\x6a\xb3\x64\x17\x9f\xd9\x67\x30\x41"
  "\xb9\xa5\x93\x9f\xa1\xa2\x01\x0d\x2a\xc7\x94\xe2\x51\x68\x05\x5f\x79\x4d\xdc\x1f\xdf\xae\x35\x21"
  "\xf4\x44\x6b\xfd\x11\xda\x98\x34\x5f\x54\x3d\xf6\xce\x19\x9d\xf8\xfe\x48\xf8\xcd\xd1\x7a\xdc\xa8"
  "\x7b\xf4\x57\x11\x18\x3c\x49\x6d\x41\xaa\x0c";
static const uint8_t cap_m4[99] =
  "\x02\x03\x00\x5f\x02\x03\x0a\x00\x10\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x10\xbb\xa3\xbd\xfb\xcf\xde\x2b\xc5\x37\x50\x9d\x71\xf2\xec"
  "\xd1\x00\x00";

static void test_capture(void)
{
  static const uint8_t ap[6] = {0x00, 0x0c, 0x41, 0x82, 0xb2, 0x55};
  static const uint8_t sta[6] = {0x00, 0x0d, 0x93, 0x82, 0x36, 0x3a};
  uint8_t pmk[32];
  wpa_ctx_t w;
  wpa_out_t o;
  int r;
  /* PBKDF2-HMAC-SHA1("Induction", "Coherer", 4096, 32) */
  unhex("a288fcf0caaacda9a9f58633ff35e8992a01d9c10ba5e02efdf8cb5d730ce7bc", pmk);
  wpa_begin(&w, pmk, sta, ap, WPA_CIPHER_TKIP);   /* the capture's group cipher is TKIP */

  r = wpa_rx(&w, cap_m1, sizeof(cap_m1), cap_m2 + 17, &o);
  CHECK(r == WPA_REPLY);
  CHECK(o.eapol_len == sizeof(cap_m2));
  CHECK(!o.install_ptk && !o.install_gtk);
  /* the station's real message 2 checks out under the KCK we derived */
  CHECK(mic_matches(w.pending, cap_m2, sizeof(cap_m2)));
  /* ours: same nonce, replay counter and RSN IE; key length 0 as RSN wants */
  CHECK(memcmp(o.eapol + 9, cap_m2 + 9, 8 + 32) == 0);
  CHECK(memcmp(o.eapol + 97, cap_m2 + 97, 2 + 22) == 0);
  CHECK(o.eapol[5] == 0x01 && o.eapol[6] == 0x0a && o.eapol[7] == 0 && o.eapol[8] == 0);
  CHECK(mic_matches(w.pending, o.eapol, o.eapol_len));

  r = wpa_rx(&w, cap_m3, sizeof(cap_m3), NULL, &o);
  CHECK(r == WPA_REPLY);
  CHECK(w.ptk_valid);
  CHECK(o.install_ptk);
  CHECK(o.install_gtk && o.gtk_len == 32);        /* TKIP GTK, unwrapped under our KEK */
  CHECK(o.eapol_len == sizeof(cap_m4));
  CHECK(mic_matches(w.kck, cap_m4, sizeof(cap_m4)));
  CHECK(mic_matches(w.kck, o.eapol, o.eapol_len));
  CHECK(o.eapol[5] == 0x03 && o.eapol[6] == 0x0a);
  CHECK(memcmp(o.eapol + 9, cap_m4 + 9, 8) == 0);

  /* message 3 again: same replay counter, dropped */
  CHECK(wpa_rx(&w, cap_m3, sizeof(cap_m3), NULL, &o) == WPA_IGNORED);
}

/* the AP repeating itself: message 1 while it waits for message 2, message 3
 * when our message 4 was lost */
static void test_repeats(void)
{
  static const uint8_t ap[6] = {0x00, 0x0c, 0x41, 0x82, 0xb2, 0x55};
  static const uint8_t sta[6] = {0x00, 0x0d, 0x93, 0x82, 0x36, 0x3a};
  uint8_t pmk[32], m1[121], m3[179], other[32], kck[16];
  wpa_ctx_t w;
  wpa_out_t o;
  unhex("a288fcf0caaacda9a9f58633ff35e8992a01d9c10ba5e02efdf8cb5d730ce7bc", pmk);
  memset(other, 0x5C, 32);
  wpa_begin(&w, pmk, sta, ap, WPA_CIPHER_TKIP);

  /* message 1, then the same message 1 with a higher replay counter and a
   * different random nonce on offer: the answer keeps the first nonce */
  CHECK(wpa_rx(&w, cap_m1, sizeof(cap_m1), cap_m2 + 17, &o) == WPA_REPLY);
  memcpy(m1, cap_m1, sizeof(m1));
  m1[16] = 1;
  CHECK(wpa_rx(&w, m1, sizeof(m1), other, &o) == WPA_REPLY);
  CHECK(memcmp(o.eapol + 17, cap_m2 + 17, 32) == 0);
  CHECK(mic_matches(w.pending, cap_m2, sizeof(cap_m2)));   /* the capture's PTK still */

  /* message 3 as captured (its counter is 1: above the repeat's) */
  CHECK(wpa_rx(&w, cap_m3, sizeof(cap_m3), NULL, &o) == WPA_REPLY);
  CHECK(o.install_ptk && w.ptk_valid);
  memcpy(kck, w.kck, 16);
  /* message 3 again with the next counter, our message 4 having been lost:
   * message 4 again under the same keys, nothing to install */
  memcpy(m3, cap_m3, sizeof(m3));
  m3[16] = 2;
  hmac_sha1(kck, 16, m3, sizeof(m3), o.eapol);   /* scratch: recompute the AP's MIC */
  memset(m3 + 81, 0, 16);
  hmac_sha1(kck, 16, m3, sizeof(m3), o.eapol);
  memcpy(m3 + 81, o.eapol, 16);
  CHECK(wpa_rx(&w, m3, sizeof(m3), NULL, &o) == WPA_REPLY);
  CHECK(!o.install_ptk && !o.install_gtk && o.eapol_len == 99);
  CHECK(o.eapol[5] == 0x03 && o.eapol[6] == 0x0a && o.eapol[16] == 2);
  CHECK(mic_matches(kck, o.eapol, o.eapol_len));
  CHECK(memcmp(w.kck, kck, 16) == 0);
  /* the same repeat once more: a replay, dropped; a forged one: dropped */
  CHECK(wpa_rx(&w, m3, sizeof(m3), NULL, &o) == WPA_IGNORED);
  m3[16] = 3;
  CHECK(wpa_rx(&w, m3, sizeof(m3), NULL, &o) == WPA_IGNORED);
}

/* ---- simulated AP ---- */

typedef struct ap_t {
  uint8_t pmk[32], mac[6], sta[6], anonce[32], kck[16], kek[16], tk[16], gtk[16];
  uint64_t replay;
} ap_t;

static void put64(uint8_t* p, uint64_t v) { for (int i = 7; i >= 0; i--, v >>= 8) p[i] = (uint8_t)v; }

/* the AP's own PTK derivation, written out independently of wpa.c */
static void ap_ptk(ap_t* a, const uint8_t snonce[32])
{
  uint8_t data[100], ptk[60];
  size_t n = 0;
  const char* label = "Pairwise key expansion";
  memcpy(data, label, strlen(label) + 1);
  n = strlen(label) + 1;
  memcpy(data + n, memcmp(a->mac, a->sta, 6) < 0 ? a->mac : a->sta, 6);
  memcpy(data + n + 6, memcmp(a->mac, a->sta, 6) < 0 ? a->sta : a->mac, 6);
  memcpy(data + n + 12, memcmp(a->anonce, snonce, 32) < 0 ? a->anonce : snonce, 32);
  memcpy(data + n + 44, memcmp(a->anonce, snonce, 32) < 0 ? snonce : a->anonce, 32);
  n += 76;
  for (int i = 0; i < 3; i++) {
    data[n] = (uint8_t)i;
    hmac_sha1(a->pmk, 32, data, n + 1, ptk + i * 20);
  }
  memcpy(a->kck, ptk, 16);
  memcpy(a->kek, ptk + 16, 16);
  memcpy(a->tk, ptk + 32, 16);
}

static uint32_t ap_frame(ap_t* a, uint8_t* f, uint16_t info, const uint8_t* nonce, const uint8_t* plain_kd,
                         uint32_t plain_len, const uint8_t* mic_key)
{
  uint32_t kd_len = 0;
  memset(f, 0, 400);
  f[0] = 2;
  f[1] = 3;
  f[4] = 2;
  f[5] = (uint8_t)(info >> 8);
  f[6] = (uint8_t)info;
  f[8] = 16;
  put64(f + 9, ++a->replay);
  if (nonce)
    memcpy(f + 17, nonce, 32);
  f[65] = 0x42;                         /* RSC */
  if (plain_kd) {
    mbedtls_nist_kw_context kw;
    size_t out_len = 0;
    mbedtls_nist_kw_init(&kw);
    mbedtls_nist_kw_setkey(&kw, MBEDTLS_CIPHER_ID_AES, a->kek, 128, 1);
    mbedtls_nist_kw_wrap(&kw, MBEDTLS_KW_MODE_KW, plain_kd, plain_len, f + 99, &out_len, 300);
    mbedtls_nist_kw_free(&kw);
    kd_len = (uint32_t)out_len;
  }
  f[97] = (uint8_t)(kd_len >> 8);
  f[98] = (uint8_t)kd_len;
  f[2] = (uint8_t)((95 + kd_len) >> 8);
  f[3] = (uint8_t)(95 + kd_len);
  if (mic_key) {
    uint8_t d[20];
    hmac_sha1(mic_key, 16, f, 99 + kd_len, d);
    memcpy(f + 81, d, 16);
  }
  return 99 + kd_len;
}

/* key data: the AP's RSN IE (message 3 only), the GTK KDE, padding to 8 */
static uint32_t gtk_kde(uint8_t* kd, const uint8_t gtk[16], int idx, int with_ie)
{
  static const uint8_t rsn_ie[] = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F,
                                   0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00};
  uint32_t n = 0;
  if (with_ie) {
    memcpy(kd, rsn_ie, sizeof(rsn_ie));
    n = sizeof(rsn_ie);
  }
  kd[n++] = 0xDD; kd[n++] = 22; kd[n++] = 0x00; kd[n++] = 0x0F; kd[n++] = 0xAC; kd[n++] = 0x01;
  kd[n++] = (uint8_t)idx; kd[n++] = 0;
  memcpy(kd + n, gtk, 16);
  n += 16;
  if (n % 8) { kd[n++] = 0xDD; while (n % 8) kd[n++] = 0; }
  return n;
}

static void test_simulated_ap(void)
{
  ap_t a;
  wpa_ctx_t w;
  wpa_out_t o;
  uint8_t f[400], kd[64], snonce[32], gtk2[16], saved[400];
  uint32_t n, kn, saved_len;
  int r;

  memset(&a, 0, sizeof(a));
  for (int i = 0; i < 32; i++) { a.pmk[i] = (uint8_t)(i * 7 + 1); a.anonce[i] = (uint8_t)(0xA0 + i); snonce[i] = (uint8_t)(0x10 + i); }
  memcpy(a.mac, "\x02\x11\x22\x33\x44\x55", 6);
  memcpy(a.sta, "\x00\x09\xbf\x01\x02\x03", 6);
  memset(a.gtk, 0x77, 16);
  memset(gtk2, 0x99, 16);
  wpa_begin(&w, a.pmk, a.sta, a.mac, WPA_CIPHER_CCMP);

  /* group message before any PTK of ours: we must reassociate */
  n = ap_frame(&a, f, 0x1382, NULL, NULL, 0, NULL);
  CHECK(wpa_rx(&w, f, n, snonce, &o) == WPA_NEED_PTK);

  /* 4-way handshake */
  n = ap_frame(&a, f, 0x008A, a.anonce, NULL, 0, NULL);
  CHECK(wpa_rx(&w, f, n, snonce, &o) == WPA_REPLY);
  ap_ptk(&a, o.eapol + 17);
  CHECK(memcmp(o.eapol + 17, snonce, 32) == 0);
  CHECK(mic_matches(a.kck, o.eapol, o.eapol_len));     /* the AP accepts message 2 */
  CHECK(o.eapol[0] == 2);                              /* EAPOL version echoed */
  CHECK(o.eapol[99 + 7] == 0x04);                      /* group cipher CCMP in the RSN IE */

  /* message 3 with a different ANonce: dropped */
  kn = gtk_kde(kd, a.gtk, 1, 1);
  {
    uint8_t other[32];
    memset(other, 0xEE, 32);
    n = ap_frame(&a, f, 0x13CA, other, kd, kn, a.kck);
    CHECK(wpa_rx(&w, f, n, NULL, &o) == WPA_IGNORED);
  }
  /* message 3 with a forged MIC: dropped */
  n = ap_frame(&a, f, 0x13CA, a.anonce, kd, kn, a.kck);
  f[81] ^= 1;
  CHECK(wpa_rx(&w, f, n, NULL, &o) == WPA_IGNORED);
  CHECK(!w.ptk_valid);
  /* the real one */
  n = ap_frame(&a, f, 0x13CA, a.anonce, kd, kn, a.kck);
  r = wpa_rx(&w, f, n, NULL, &o);
  CHECK(r == WPA_REPLY);
  CHECK(o.install_ptk && memcmp(w.tk, a.tk, 16) == 0);
  CHECK(o.install_gtk && o.gtk_index == 1 && o.gtk_len == 16 && memcmp(o.gtk, a.gtk, 16) == 0);
  CHECK(o.rsc[0] == 0x42);
  CHECK(mic_matches(a.kck, o.eapol, o.eapol_len));     /* the AP accepts message 4 */
  CHECK(o.eapol[5] == 0x03 && o.eapol[6] == 0x0a);

  /* group rekey to key index 2 */
  kn = gtk_kde(kd, gtk2, 2, 0);
  n = ap_frame(&a, f, 0x1382, NULL, kd, kn, a.kck);
  memcpy(saved, f, n);
  saved_len = n;
  r = wpa_rx(&w, f, n, NULL, &o);
  CHECK(r == WPA_REPLY);
  CHECK(!o.install_ptk && o.install_gtk && o.gtk_index == 2 && memcmp(o.gtk, gtk2, 16) == 0);
  CHECK(mic_matches(a.kck, o.eapol, o.eapol_len));
  CHECK(o.eapol[5] == 0x03 && o.eapol[6] == 0x02);
  CHECK(o.eapol_len == 99);
  /* the same frame replayed: dropped */
  CHECK(wpa_rx(&w, saved, saved_len, NULL, &o) == WPA_IGNORED);
  /* a group message under the wrong KCK: dropped, keys kept */
  {
    uint8_t bad[16];
    memset(bad, 1, 16);
    n = ap_frame(&a, f, 0x1382, NULL, kd, kn, bad);
    CHECK(wpa_rx(&w, f, n, NULL, &o) == WPA_IGNORED);
    CHECK(!o.install_gtk && o.eapol_len == 0);
  }
  /* PTK rekey: a new message 1 on the same association */
  memset(a.anonce, 0x3C, 32);
  n = ap_frame(&a, f, 0x008A, a.anonce, NULL, 0, NULL);
  memset(snonce, 0xC3, 32);
  CHECK(wpa_rx(&w, f, n, snonce, &o) == WPA_REPLY);
  ap_ptk(&a, snonce);
  CHECK(mic_matches(a.kck, o.eapol, o.eapol_len));
  CHECK(w.ptk_valid);                                  /* old PTK still answers until message 3 */
  kn = gtk_kde(kd, gtk2, 2, 1);
  n = ap_frame(&a, f, 0x13CA, a.anonce, kd, kn, a.kck);
  CHECK(wpa_rx(&w, f, n, NULL, &o) == WPA_REPLY);
  CHECK(o.install_ptk && memcmp(w.tk, a.tk, 16) == 0);

  /* things we don't speak */
  n = ap_frame(&a, f, 0x0089, a.anonce, NULL, 0, NULL);     /* descriptor version 1: TKIP pairwise */
  CHECK(wpa_rx(&w, f, n, snonce, &o) == WPA_UNSUPPORTED);
  n = ap_frame(&a, f, 0x008A, a.anonce, NULL, 0, NULL);
  f[4] = 254;                                               /* WPA1 descriptor */
  CHECK(wpa_rx(&w, f, n, snonce, &o) == WPA_UNSUPPORTED);
  /* truncated and malformed */
  CHECK(wpa_rx(&w, f, 50, snonce, &o) == WPA_IGNORED);
  n = ap_frame(&a, f, 0x008A, a.anonce, NULL, 0, NULL);
  f[3] = 0xFF;                                              /* body longer than the frame */
  CHECK(wpa_rx(&w, f, n, snonce, &o) == WPA_IGNORED);
  n = ap_frame(&a, f, 0x008A, a.anonce, NULL, 0, NULL);
  f[98] = 200;                                              /* key data past the body */
  CHECK(wpa_rx(&w, f, n, snonce, &o) == WPA_IGNORED);
  n = ap_frame(&a, f, 0x000A, a.anonce, NULL, 0, NULL);     /* no Ack bit: not from an AP */
  CHECK(wpa_rx(&w, f, n, snonce, &o) == WPA_IGNORED);
}

int main(void)
{
  test_prf();
  test_capture();
  test_repeats();
  test_simulated_ap();
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
