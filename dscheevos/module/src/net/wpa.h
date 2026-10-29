/*
 * WPA2-PSK supplicant: the EAPOL-Key side of the 4-way and group key
 * handshakes (IEEE 802.11-2016 12.7). Pure functions over byte buffers, so
 * the host tests run it against a captured handshake and a simulated AP;
 * link.c moves the frames and loads the keys into the Atheros chip.
 *
 * Only key descriptor version 2 (HMAC-SHA1 MIC, AES key wrap) is handled:
 * the pairwise cipher is always CCMP, as in the launcher's dswifi.
 */
#ifndef DSCM_WPA_H
#define DSCM_WPA_H

#include <stdint.h>

#define WPA_CIPHER_TKIP 3   /* dswifi's AP_CRYPT_* numbering, as WMI takes it */
#define WPA_CIPHER_CCMP 4

typedef struct wpa_ctx_t {
  uint8_t pmk[32];
  uint8_t own[6], ap[6];
  uint8_t group_cipher;          /* WPA_CIPHER_* */
  uint8_t anonce[32], snonce[32];
  uint8_t kck[16], kek[16], tk[16];
  uint8_t pending[48];           /* message 1's PTK, until message 3 proves it */
  uint8_t pending_valid;
  uint8_t ptk_valid;             /* kck/kek/tk confirmed by a good message 3 */
  uint8_t replay_valid;
  uint8_t replay[8];             /* last accepted replay counter */
} wpa_ctx_t;

typedef struct wpa_out_t {
  uint8_t eapol[160];            /* reply, from the EAPOL version byte on */
  uint32_t eapol_len;            /* 0: nothing to send */
  uint8_t install_ptk;           /* load tk as the pairwise key */
  uint8_t install_gtk;           /* load gtk as group key gtk_index */
  uint8_t gtk_index;
  uint8_t gtk_len;
  uint8_t gtk[32];
  uint8_t rsc[8];                /* receive sequence counter for the group key */
} wpa_out_t;

enum {
  WPA_IGNORED = 0,      /* not ours, a replay, or a bad MIC: dropped */
  WPA_REPLY,            /* send out->eapol, then load any keys it names */
  WPA_NEED_PTK,         /* a group rekey we can't answer: reassociate */
  WPA_UNSUPPORTED,      /* key descriptor we don't speak (WPA1, TKIP pairwise) */
};

/* Start a fresh 4-way handshake with this AP (after a WMI connect). */
void wpa_begin(wpa_ctx_t* w, const uint8_t pmk[32], const uint8_t own[6], const uint8_t ap[6], uint8_t group_cipher);

/* One EAPOL frame from the AP (ethertype 0x888E payload). snonce is fresh
 * randomness, used only if this is message 1 of a new handshake (a repeat
 * of message 1 keeps the nonce already answered with). A repeat of message 3
 * after our message 4 was lost gets message 4 again, with no keys to load. */
int wpa_rx(wpa_ctx_t* w, const uint8_t* eapol, uint32_t len, const uint8_t snonce[32], wpa_out_t* out);

/* Exposed for the tests. */
void wpa_prf(const uint8_t* key, uint32_t key_len, const char* label, const uint8_t* data, uint32_t data_len,
             uint8_t* out, uint32_t out_len);

#endif
