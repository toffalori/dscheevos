/*
 * The module's network stack: Ethernet frames over the Atheros chip
 * through the ARM7 SDIO shim (link.c), lwIP for TCP/IP, and HTTPS with
 * mbedTLS (https.c). Runs only in the game's idle time, never in an IRQ.
 */
#ifndef DSCM_NET_H
#define DSCM_NET_H

#include <stdint.h>
#include "dscheevos_module.h"
#include "ds_stream_protocol.h"

typedef struct net_config_t {
  uint8_t mac[6];
  uint32_t ip, netmask, gateway, dns;   /* network byte order */
  const char* host;                     /* "https://retroachievements.org" */
  const char* user_agent;
  /* the access point, to rejoin it after a drop or a group rekey */
  uint8_t ap_ssid[32];
  uint8_t ap_ssid_len;
  uint8_t ap_bssid[6];
  uint8_t ap_channel;
  uint8_t ap_security;                  /* DSCM_AP_* */
  uint8_t ap_pair_cipher, ap_group_cipher;
  uint8_t pmk_valid;
  uint8_t pmk[32];
} net_config_t;

/* Returns the ring for the module header (0 if the network can't start). */
void* net_init(const net_config_t* config);
/* One slice of network work: frames in, lwIP timers, HTTPS requests. */
void net_service(uint32_t now_ms);
uint32_t net_state(void);   /* DSCM_NET_STATE_* */
uint32_t net_detail(void);  /* last error, for the in-game menu */
uint32_t net_handshake_ms(void);
/* Nonzero while there is network work in flight (a request, a connection
 * being set up, frames waiting, an association): net_service() is then
 * wanted every idle pass, else once a frame is enough. */
int net_busy(void);

/* link.c, used by https.c */
int link_up(void);
int link_rejoining(void);
int link_busy(void);        /* frames or control messages waiting, or joining */   /* down, but working on getting back */
uint32_t link_rejoins(void);  /* associations we started ourselves */
uint32_t link_paused_count(void);  /* VBlanks the chip was left to the game's own DS wireless */
void link_reconnect(void);    /* the player asked: a fresh association now (stream build, in-game menu) */
void link_diag(dss_link_status_t* out);   /* the ring's and the ARM7 shim's counters (stream STATUS) */
void link_stir(void);       /* mix event timing into the TLS entropy pool */

/* entropy.c */
void entropy_stir(const void* data, uint32_t len);
void entropy_bytes(uint8_t* out, uint32_t len);   /* up to 32 */

/* start.s */
void dscm_dc_clean(const void* addr, uint32_t len);
void dscm_dc_invalidate(const void* addr, uint32_t len);

#endif
