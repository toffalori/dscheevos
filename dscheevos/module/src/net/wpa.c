/*
 * WPA2-PSK supplicant, EAPOL-Key handling only (see wpa.h). Frame layout
 * from the EAPOL version byte:
 *   0 version | 1 type (3: key) | 2 body length (BE) | 4 descriptor (2: RSN)
 *   5 key info (BE) | 7 key length | 9 replay counter | 17 nonce | 49 IV
 *   65 RSC | 73 reserved | 81 MIC | 97 key data length | 99 key data
 *
 * Message formats follow the launcher's dswifi (BlocksDS, MIT licence), which
 * the DSi's Atheros firmware already accepts; the checks it leaves out
 * (message 3 MIC, replay counters, nonce match) are done here.
 */
#include "wpa.h"

#include "mbedtls/md.h"
#include "mbedtls/nist_kw.h"

#include <string.h>

#define HDR_LEN 99
#define KI_VERSION   0x0007
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENC_DATA  0x1000

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

void wpa_prf(const uint8_t* key, uint32_t key_len, const char* label, const uint8_t* data, uint32_t data_len,
             uint8_t* out, uint32_t out_len)
{
  const mbedtls_md_info_t* sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  mbedtls_md_context_t md;
  uint8_t digest[20], zero = 0, i = 0;
  mbedtls_md_init(&md);
  if (mbedtls_md_setup(&md, sha1, 1) != 0) {
    memset(out, 0, out_len);
    return;
  }
  while (out_len) {
    uint32_t n = out_len < 20 ? out_len : 20;
    mbedtls_md_hmac_starts(&md, key, key_len);
    mbedtls_md_hmac_update(&md, (const uint8_t*)label, strlen(label));
    mbedtls_md_hmac_update(&md, &zero, 1);
    mbedtls_md_hmac_update(&md, data, data_len);
    mbedtls_md_hmac_update(&md, &i, 1);
    mbedtls_md_hmac_finish(&md, digest);
    memcpy(out, digest, n);
    out += n;
    out_len -= n;
    i++;
  }
  mbedtls_md_free(&md);
}

static void calc_mic(const uint8_t kck[16], const uint8_t* frame, uint32_t len, uint8_t mic[16])
{
  uint8_t digest[20];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), kck, 16, frame, len, digest);
  memcpy(mic, digest, 16);
}

/* MIC over a received frame, its MIC field read as zero */
static int mic_ok(const uint8_t kck[16], const uint8_t* frame, uint32_t len)
{
  uint8_t copy[HDR_LEN + 256], mic[16], diff = 0;
  int i;
  if (len > sizeof(copy))
    return 0;
  memcpy(copy, frame, len);
  memset(copy + 81, 0, 16);
  calc_mic(kck, copy, len, mic);
  for (i = 0; i < 16; i++)
    diff |= mic[i] ^ frame[81 + i];
  return diff == 0;
}

static void derive_ptk(wpa_ctx_t* w)
{
  uint8_t data[76];
  int ap_low = memcmp(w->ap, w->own, 6) < 0;
  int an_low = memcmp(w->anonce, w->snonce, 32) < 0;
  memcpy(data, ap_low ? w->ap : w->own, 6);
  memcpy(data + 6, ap_low ? w->own : w->ap, 6);
  memcpy(data + 12, an_low ? w->anonce : w->snonce, 32);
  memcpy(data + 44, an_low ? w->snonce : w->anonce, 32);
  wpa_prf(w->pmk, 32, "Pairwise key expansion", data, sizeof(data), w->pending, sizeof(w->pending));
  w->pending_valid = 1;
}

/* unwrap the key data and find the GTK KDE: DD len 00-0F-AC 01 keyid 00 GTK */
static int find_gtk(const uint8_t kek[16], const uint8_t* kd, uint32_t kd_len, wpa_out_t* out)
{
  mbedtls_nist_kw_context kw;
  uint8_t plain[256];
  size_t plain_len = 0;
  uint32_t i = 0;
  int ok;
  if (kd_len < 24 || kd_len > sizeof(plain) || kd_len % 8)
    return 0;
  mbedtls_nist_kw_init(&kw);
  ok = mbedtls_nist_kw_setkey(&kw, MBEDTLS_CIPHER_ID_AES, kek, 128, 0) == 0
    && mbedtls_nist_kw_unwrap(&kw, MBEDTLS_KW_MODE_KW, kd, kd_len, plain, &plain_len, sizeof(plain)) == 0;
  mbedtls_nist_kw_free(&kw);
  if (!ok)
    return 0;
  while (i + 2 <= plain_len) {
    uint8_t type = plain[i], n = plain[i + 1];
    if (type == 0xDD && n == 0)
      break;                                  /* padding */
    if (i + 2 + n > plain_len)
      break;
    if (type == 0xDD && n >= 6 + 16 && n <= 6 + 32 && plain[i + 2] == 0x00 && plain[i + 3] == 0x0F
        && plain[i + 4] == 0xAC && plain[i + 5] == 0x01) {
      out->gtk_index = plain[i + 6] & 3;
      out->gtk_len = (uint8_t)(n - 6);
      memcpy(out->gtk, plain + i + 8, out->gtk_len);
      out->install_gtk = 1;
      memset(plain, 0, sizeof(plain));
      return 1;
    }
    i += 2 + n;
  }
  memset(plain, 0, sizeof(plain));
  return 0;
}

/* reply skeleton: same EAPOL version as the AP, our key info, its replay counter */
static uint8_t* reply(const uint8_t* in, uint16_t info, uint32_t kd_len, wpa_out_t* out)
{
  uint8_t* r = out->eapol;
  memset(r, 0, HDR_LEN);
  r[0] = in[0];
  r[1] = 3;
  put16(r + 2, HDR_LEN - 4 + kd_len);
  r[4] = 2;
  put16(r + 5, info);
  memcpy(r + 9, in + 9, 8);
  put16(r + 97, kd_len);
  out->eapol_len = HDR_LEN + kd_len;
  return r;
}

static int replay_ok(const wpa_ctx_t* w, const uint8_t* counter)
{
  return !w->replay_valid || memcmp(counter, w->replay, 8) > 0;
}

void wpa_begin(wpa_ctx_t* w, const uint8_t pmk[32], const uint8_t own[6], const uint8_t ap[6], uint8_t group_cipher)
{
  memset(w, 0, sizeof(*w));
  memcpy(w->pmk, pmk, 32);
  memcpy(w->own, own, 6);
  memcpy(w->ap, ap, 6);
  w->group_cipher = group_cipher;
  w->pending_valid = 0;
}

int wpa_rx(wpa_ctx_t* w, const uint8_t* eapol, uint32_t len, const uint8_t snonce[32], wpa_out_t* out)
{
  uint32_t flen, kd_len;
  uint16_t info;
  memset(out, 0, sizeof(*out));
  if (len < HDR_LEN || eapol[1] != 3)
    return WPA_IGNORED;
  flen = 4 + be16(eapol + 2);
  if (flen < HDR_LEN || flen > len)
    return WPA_IGNORED;
  if (eapol[4] != 2)
    return WPA_UNSUPPORTED;                  /* 254: WPA1 */
  info = be16(eapol + 5);
  if ((info & KI_VERSION) != 2)
    return WPA_UNSUPPORTED;                  /* 1: TKIP pairwise (HMAC-MD5, RC4) */
  kd_len = be16(eapol + 97);
  if (HDR_LEN + kd_len > flen || !(info & KI_ACK))
    return WPA_IGNORED;
  if (!replay_ok(w, eapol + 9))
    return WPA_IGNORED;

  if ((info & KI_PAIRWISE) && !(info & KI_MIC)) {
    /* message 1: answer with our nonce and the RSN IE we associated with.
     * The AP repeats message 1 while it waits for message 2; our nonce stays
     * the same until message 3 ends the handshake, so whichever of our
     * answers the AP takes, its PTK is ours. */
    static const uint8_t rsn_ccmp[] = {0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F,
                                       0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00};
    uint8_t* r;
    memcpy(w->anonce, eapol + 17, 32);
    if (!w->pending_valid)
      memcpy(w->snonce, snonce, 32);
    derive_ptk(w);
    r = reply(eapol, 2 | KI_PAIRWISE | KI_MIC, sizeof(rsn_ccmp), out);
    memcpy(r + 17, w->snonce, 32);
    memcpy(r + HDR_LEN, rsn_ccmp, sizeof(rsn_ccmp));
    r[HDR_LEN + 7] = w->group_cipher == WPA_CIPHER_CCMP ? 0x04 : 0x02;
    calc_mic(w->pending, r, out->eapol_len, r + 81);
    return WPA_REPLY;
  }

  if (info & KI_PAIRWISE) {
    /* message 3: proves the AP has the PMK; carries the GTK */
    if (!(info & KI_INSTALL) || memcmp(eapol + 17, w->anonce, 32) != 0)
      return WPA_IGNORED;
    if (!w->pending_valid) {
      /* repeated after our message 4 was lost: answer again under the keys
       * we hold, which the AP will install when this answer arrives */
      if (!w->ptk_valid || !mic_ok(w->kck, eapol, flen))
        return WPA_IGNORED;
      memcpy(w->replay, eapol + 9, 8);
      reply(eapol, 2 | KI_PAIRWISE | KI_MIC | KI_SECURE, 0, out);
      calc_mic(w->kck, out->eapol, out->eapol_len, out->eapol + 81);
      return WPA_REPLY;
    }
    if (!mic_ok(w->pending, eapol, flen))
      return WPA_IGNORED;
    memcpy(w->kck, w->pending, 16);
    memcpy(w->kek, w->pending + 16, 16);
    memcpy(w->tk, w->pending + 32, 16);
    memset(w->pending, 0, sizeof(w->pending));
    w->pending_valid = 0;
    w->ptk_valid = 1;
    memcpy(w->replay, eapol + 9, 8);
    w->replay_valid = 1;
    if (info & KI_ENC_DATA)
      find_gtk(w->kek, eapol + HDR_LEN, kd_len, out);
    memcpy(out->rsc, eapol + 65, 8);
    reply(eapol, 2 | KI_PAIRWISE | KI_MIC | KI_SECURE, 0, out);
    calc_mic(w->kck, out->eapol, out->eapol_len, out->eapol + 81);
    out->install_ptk = 1;
    return WPA_REPLY;
  }

  /* group message 1: a new GTK under the PTK we hold */
  if (!(info & KI_MIC) || !(info & KI_SECURE))
    return WPA_IGNORED;
  if (!w->ptk_valid)
    return WPA_NEED_PTK;
  if (!mic_ok(w->kck, eapol, flen) || !(info & KI_ENC_DATA) || !find_gtk(w->kek, eapol + HDR_LEN, kd_len, out)) {
    memset(out, 0, sizeof(*out));
    return WPA_IGNORED;
  }
  memcpy(w->replay, eapol + 9, 8);
  w->replay_valid = 1;
  memcpy(out->rsc, eapol + 65, 8);
  reply(eapol, 2 | KI_MIC | KI_SECURE, 0, out);
  calc_mic(w->kck, out->eapol, out->eapol_len, out->eapol + 81);
  return WPA_REPLY;
}
