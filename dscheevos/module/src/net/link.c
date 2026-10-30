/*
 * Link layer: Ethernet frames to and from the Atheros chip.
 *
 * The launcher left the chip associated; nds-bootstrap's ARM7 card engine
 * moves raw HTC frames between the chip's mailbox and the ring in
 * dscm_shim.h once per VBlank. Here the frames become Ethernet for lwIP:
 *   HTC header (6) | WMI data header (2) | 802.3: dst, src, length |
 *   LLC/SNAP (6) | ethertype | payload
 * WMI events (endpoint 1) tell us when the AP drops us.
 *
 * Rejoining: the launcher's dswifi did the WPA2 handshake, so its keys are
 * unknown here. When the AP drops us, or starts a group rekey we can't
 * answer, we settle the firmware with WMI_DISCONNECT, reassociate with
 * WMI_CONNECT and run the 4-way handshake ourselves (wpa.c) with the PMK
 * nds-bootstrap read from the console's saved settings; later group rekeys
 * are then answered in place (the state machine is described above
 * can_rejoin). Commands and EAPOL frames wait in a small queue, since the
 * shim moves one frame per VBlank and the ring has room for four.
 *
 * Cache: the ring sits in cached RAM. Lines the ARM9 writes are cleaned
 * after writing; lines the ARM7 writes are invalidated before reading.
 */
#include "net.h"
#include "dscm_shim.h"
#include "wpa.h"

#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#include <stddef.h>
#include <string.h>

#define WMI_CONNECT_CMD            0x0001
#define WMI_DISCONNECT_CMD         0x0003
#define WMI_SYNCHRONIZE_CMD        0x0004
#define WMI_SET_SCAN_PARAMS_CMD    0x0008
#define WMI_SET_BSS_FILTER_CMD     0x0009
#define WMI_SET_CHANNEL_PARAMS_CMD 0x0011
#define WMI_ADD_CIPHER_KEY_CMD     0x0016
#define WMI_CONNECT_EVENT          0x1002
#define WMI_DISCONNECT_EVENT       0x1003
#define WMI_CMD_ERROR_EVENT        0x1005
#define DISC_REASON_CMD 3        /* the firmware's reason for a disconnect we asked for */
#define HTC_EP_WMI  1
#define HTC_EP_DATA 2

#define JOIN_TIMEOUT_MS  8000    /* WMI_CONNECT to connect event */
#define KEY_TIMEOUT_MS   5000    /* connect event to message 3 */
#define LEAVE_TIMEOUT_MS 1000    /* our WMI_DISCONNECT to its event */
#define BACKOFF_MIN_MS   1000
#define BACKOFF_MAX_MS   10000   /* a router back from a restart is found within seconds */

/* Association bookkeeping runs rarely: out of the hot code (hot-layout.ref). */
#define DSCM_COLD __attribute__((section(".text_cold"), noinline))

static shim_shared_t g_ring;     /* aligned to cache lines by SHIM_LINE */
static struct netif g_netif;
static net_config_t g_config;
static int g_connected = 1;      /* the launcher left us associated */
static uint32_t g_rx_frames, g_tx_frames, g_tx_drops;
/* why the association went, and how rejoining fares (link_diag) */
static uint32_t g_disc_events, g_connect_events;
static uint16_t g_cause_proto, g_last_proto;
static uint8_t  g_cause_reason, g_last_reason, g_join_timeouts, g_key_timeouts, g_need_ptk;
/* the firmware refusing a command (WMI_CMD_ERROR_EVENT): command id | code << 16; no
 * field in dss_link_status_t yet, a refused WMI_CONNECT counts as a join timeout */
static uint32_t g_cmd_errors, g_last_cmd_error;

/* association: the launcher's to start with, ours after a rejoin */
enum { J_UP, J_LEAVING, J_WAITING, J_JOINING, J_KEYING, J_GIVEN_UP };
static int g_join = J_UP;
static uint32_t g_now, g_deadline, g_backoff = BACKOFF_MIN_MS;
static uint32_t g_rejoins;
static wpa_ctx_t g_wpa;

/* frames we make ourselves (WMI commands, EAPOL), ahead of lwIP's: a join
 * queues four, message 3's answer six, and the shim takes one a VBlank */
#define CTRL_SLOTS 16
#define CTRL_MAX   256
static struct { uint32_t len; uint8_t data[CTRL_MAX]; } g_ctrl[CTRL_SLOTS];
static uint32_t g_ctrl_head, g_ctrl_tail;

static void publish_arm9_line(void)
{
  dscm_dc_clean(&g_ring, 32);
}

static uint32_t arm7_rx_head(void)
{
  dscm_dc_invalidate((const void*)&g_ring.attached, 32);
  return g_ring.rx_head;
}

static uint32_t arm7_tx_tail(void)
{
  dscm_dc_invalidate((const void*)&g_ring.attached, 32);
  return g_ring.tx_tail;
}

int link_up(void)
{
  uint32_t dead;
  dscm_dc_invalidate((const void*)&g_ring.attached, 32);
  dead = g_ring.dead;
  return g_connected && !dead && g_ring.attached;
}

/* Entropy from our own state only: VCOUNT and the hardware timers are the
 * game's (we never read game hardware), so the sample is the ARM7's
 * VBlank count (module memory), our frame counters and a call count. The
 * secret part of the pool is the session token (module.c, at init); the
 * first bytes of the RX slot at the ring's tail are stirred in too. */
extern uint32_t dscm_vblank_count_read(void);
void link_stir(void)
{
  static uint32_t calls;
  uint32_t sample[4];
  sample[0] = dscm_vblank_count_read();
  sample[1] = g_rx_frames;
  sample[2] = g_tx_frames;
  sample[3] = ++calls;
  entropy_stir(sample, sizeof(sample));
  /* and the first bytes of the RX slot at the ring's tail: in pump_rx, the frame being received
   * (stirred here, not in pump_rx, so the hot service loop keeps its size and layout) */
  entropy_stir(g_ring.rx[g_ring.rx_tail % SHIM_RX_SLOTS].data, 64);
}

/* ---- TX ---- */

static int ring_full(void)
{
  return g_ring.tx_head - arm7_tx_tail() >= SHIM_TX_SLOTS;
}

static void ring_push(shim_slot_tx_t* slot, uint32_t len)
{
  slot->len = len;
  dscm_dc_clean(slot, sizeof(*slot));
  g_ring.tx_head++;
  publish_arm9_line();
  g_tx_frames++;
}

static void ctrl_flush(void)
{
  while (g_ctrl_tail != g_ctrl_head && !ring_full()) {
    shim_slot_tx_t* slot = &g_ring.tx[g_ring.tx_head % SHIM_TX_SLOTS];
    uint32_t i = g_ctrl_tail % CTRL_SLOTS;
    memcpy(slot->data, g_ctrl[i].data, g_ctrl[i].len);
    ring_push(slot, g_ctrl[i].len);
    memset(g_ctrl[i].data, 0, g_ctrl[i].len);   /* key material passes through here */
    g_ctrl_tail++;
  }
}

/* HTC header: endpoint, flags, payload length, control bytes */
static uint8_t* ctrl_frame(uint8_t ep, uint8_t flags, uint32_t payload)
{
  uint8_t* d;
  if (g_ctrl_head - g_ctrl_tail >= CTRL_SLOTS || 6 + payload > CTRL_MAX)
    return NULL;
  d = g_ctrl[g_ctrl_head % CTRL_SLOTS].data;
  memset(d, 0, 6 + payload);
  d[0] = ep;
  d[1] = flags;
  d[2] = (uint8_t)payload;
  d[3] = (uint8_t)(payload >> 8);
  g_ctrl[g_ctrl_head % CTRL_SLOTS].len = 6 + payload;
  return d + 6;
}

static void ctrl_commit(void)
{
  g_ctrl_head++;
  ctrl_flush();
}

static void wmi_cmd(uint16_t id, const void* params, uint32_t len)
{
  uint8_t* p = ctrl_frame(HTC_EP_WMI, 1 /* ask for a credit report */, 2 + len);
  if (!p)
    return;
  p[0] = (uint8_t)id;
  p[1] = (uint8_t)(id >> 8);
  if (len)
    memcpy(p + 2, params, len);
  ctrl_commit();
}

/* data frame with our own LLC/SNAP body, as dswifi sends EAPOL */
static void send_eapol(const uint8_t* dst, const uint8_t* eapol, uint32_t len)
{
  uint8_t* m = ctrl_frame(HTC_EP_DATA, 0, 24 + len);
  if (!m)
    return;
  m[1] = 0x1C;
  memcpy(m + 2, dst, 6);
  memcpy(m + 8, g_config.mac, 6);
  m[14] = (uint8_t)((len + 8) >> 8);
  m[15] = (uint8_t)(len + 8);
  m[16] = 0xAA; m[17] = 0xAA; m[18] = 0x03;
  m[22] = 0x88; m[23] = 0x8E;
  memcpy(m + 24, eapol, len);
  ctrl_commit();
}

static void data_sync(void)
{
  uint8_t* m = ctrl_frame(HTC_EP_DATA, 0, 2);
  if (!m)
    return;
  m[1] = 0x02;
  ctrl_commit();
}

static void add_key(uint8_t index, uint8_t usage, uint8_t cipher, const uint8_t* key, uint8_t key_len, const uint8_t* rsc)
{
  uint8_t k[45];
  memset(k, 0, sizeof(k));
  k[0] = index;
  k[1] = cipher;
  k[2] = usage;                   /* 0 pairwise, 1 group */
  k[3] = key_len;
  if (rsc)
    memcpy(k + 4, rsc, 8);
  memcpy(k + 12, key, key_len < 16 ? key_len : 16);
  if (key_len == 32) {            /* TKIP: the chip wants the MIC keys swapped */
    memcpy(k + 12 + 16, key + 24, 8);
    memcpy(k + 12 + 24, key + 16, 8);
  }
  k[44] = 3;                      /* key_op_ctrl */
  wmi_cmd(WMI_ADD_CIPHER_KEY_CMD, k, sizeof(k));
  memset(k, 0, sizeof(k));
}

static err_t linkoutput(struct netif* netif, struct pbuf* p)
{
  static uint8_t frame[1536] __attribute__((aligned(4)));
  shim_slot_tx_t* slot;
  uint32_t n, body;
  uint8_t* m;
  (void)netif;

  if (!link_up())
    return ERR_IF;
  n = pbuf_copy_partial(p, frame, sizeof(frame), 0);
  if (n < 14)
    return ERR_ARG;
  body = n - 14;
  if (6 + 24 + body > SHIM_TX_MAX)
    return ERR_MEM;

  ctrl_flush();
  if (g_ctrl_tail != g_ctrl_head || ring_full()) {
    g_tx_drops++;                 /* TCP retransmits */
    return ERR_MEM;
  }
  slot = &g_ring.tx[g_ring.tx_head % SHIM_TX_SLOTS];
  memset(slot->data, 0, 6 + 24);
  slot->data[0] = HTC_EP_DATA;
  slot->data[2] = (uint8_t)(24 + body);
  slot->data[3] = (uint8_t)((24 + body) >> 8);
  slot->data[4] = 0x08;           /* the control bytes dswifi uses for data */
  slot->data[5] = 0x20;
  m = slot->data + 6;
  memcpy(m + 2, frame, 6);        /* destination */
  memcpy(m + 8, frame + 6, 6);    /* source (our MAC) */
  m[14] = (uint8_t)((body + 8) >> 8);
  m[15] = (uint8_t)(body + 8);
  m[16] = 0xAA; m[17] = 0xAA; m[18] = 0x03;
  m[22] = frame[12];              /* ethertype */
  m[23] = frame[13];
  memcpy(m + 24, frame + 14, body);
  ring_push(slot, 6 + 24 + body);
  link_stir();
  return ERR_OK;
}

static err_t netif_setup(struct netif* netif)
{
  netif->name[0] = 'w';
  netif->name[1] = 'l';
  netif->output = etharp_output;
  netif->linkoutput = linkoutput;
  netif->mtu = 1500;
  netif->hwaddr_len = 6;
  memcpy(netif->hwaddr, g_config.mac, 6);
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  return ERR_OK;
}

/* ---- association ----
 *
 * The firmware's side of it (ath6kl, which drives the same WMI): after a
 * disconnect it did not ask for (lost beacons, a deauth from the AP, a failed
 * attempt) it keeps trying to reconnect by itself, and a WMI_CONNECT sent
 * meanwhile is refused (WMI_CMD_ERROR_EVENT, illegal state). Only a
 * WMI_DISCONNECT settles it, answered by one more disconnect event with
 * reason 3. So every path to a new association goes through J_LEAVING, and
 * every disconnect event we did not ask for is answered with a disconnect
 * command. The connect itself repeats the settings dswifi sends before its
 * WMI_CONNECT (the AP's channel, a connect scan, the beacon filter) in case
 * the firmware dropped them with the association.
 *
 *   J_UP ----drop / group rekey----> J_LEAVING --event 3--> J_WAITING
 *   J_WAITING --backoff--> J_JOINING --connect event--> J_KEYING --m3--> J_UP
 *   J_JOINING / J_KEYING --timeout, refusal, drop--> J_LEAVING (backoff doubled)
 */

static int can_rejoin(void)
{
  if (g_config.ap_ssid_len == 0 || g_config.ap_ssid_len > 32)
    return 0;
  if (g_config.ap_security == DSCM_AP_OPEN)
    return 1;
  return g_config.ap_security == DSCM_AP_WPA2 && g_config.pmk_valid;
}

static DSCM_COLD void set_up(int up)
{
  if (up == g_connected)
    return;
  g_connected = up;
  if (up) {
    if (g_join != J_UP)
      g_rejoins++;
    g_join = J_UP;
    g_backoff = BACKOFF_MIN_MS;
    netif_set_link_up(&g_netif);
  } else {
    netif_set_link_down(&g_netif);
  }
}

/* an attempt failed: the next one waits twice as long */
static DSCM_COLD void attempt_failed(void)
{
  g_backoff = g_backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : g_backoff * 2;
}

/* join after the current pause */
static DSCM_COLD void wait_then_join(void)
{
  set_up(0);
  if (!can_rejoin()) {
    g_join = J_GIVEN_UP;
    return;
  }
  g_join = J_WAITING;
  g_deadline = g_now + g_backoff;
}

static DSCM_COLD void join(void)
{
  uint8_t c[55];
  uint32_t mhz = g_config.ap_channel == 14 ? 2484 : 2407 + 5u * g_config.ap_channel;
  int wpa2 = g_config.ap_security == DSCM_AP_WPA2;

  /* as dswifi before its WMI_CONNECT: beacons of the current BSS only, a
   * connect scan (flags 5: for a connection, active), the AP's channel */
  memset(c, 0, sizeof(c));
  c[0] = 4;
  wmi_cmd(WMI_SET_BSS_FILTER_CMD, c, 8);
  memset(c, 0, sizeof(c));
  c[0] = c[1] = c[2] = c[3] = c[4] = c[5] = 0xFF;    /* fg start, fg end, bg period: never */
  c[6] = 200; c[8] = 200;                            /* max active, passive dwell (ms) */
  c[10] = 3;                                         /* short scan ratio */
  c[11] = 5;                                         /* scan control flags */
  c[12] = 200;                                       /* min active dwell */
  wmi_cmd(WMI_SET_SCAN_PARAMS_CMD, c, 20);
  memset(c, 0, sizeof(c));
  c[2] = 3;                                          /* 11a/g phy mode, as dswifi */
  c[3] = 1;
  c[4] = (uint8_t)mhz;
  c[5] = (uint8_t)(mhz >> 8);
  wmi_cmd(WMI_SET_CHANNEL_PARAMS_CMD, c, 6);

  memset(c, 0, sizeof(c));
  c[0] = 1;                                 /* infrastructure */
  c[1] = 1;                                 /* open system 802.11 auth */
  c[2] = wpa2 ? 5 : 1;                      /* WPA2-PSK or none */
  c[3] = wpa2 ? g_config.ap_pair_cipher : DSCM_CIPHER_NONE;
  c[5] = wpa2 ? g_config.ap_group_cipher : DSCM_CIPHER_NONE;
  c[7] = g_config.ap_ssid_len;
  memcpy(c + 8, g_config.ap_ssid, g_config.ap_ssid_len);
  c[40] = (uint8_t)mhz;
  c[41] = (uint8_t)(mhz >> 8);
  memcpy(c + 42, g_config.ap_bssid, 6);
  c[48] = 0x20;                             /* follow the AP's channel switches */
  wmi_cmd(WMI_CONNECT_CMD, c, 52);
  g_join = J_JOINING;
  g_deadline = g_now + JOIN_TIMEOUT_MS;
}

/* settle the firmware with a disconnect; its event (reason 3) starts the pause */
static DSCM_COLD void leave_then_join(void)
{
  set_up(0);
  if (!can_rejoin()) {
    g_join = J_GIVEN_UP;
    return;
  }
  wmi_cmd(WMI_DISCONNECT_CMD, NULL, 0);
  g_join = J_LEAVING;
  g_deadline = g_now + LEAVE_TIMEOUT_MS;
}

/* an attempt that stalled or was refused: drop it, pause longer, try again */
static DSCM_COLD void abandon_attempt(void)
{
  attempt_failed();
  leave_then_join();
}

/* The player asked for it (the in-game menu's "Reconnect"): the pause back to
 * its shortest, and a fresh association now, unless one is under way. */
DSCM_COLD void link_reconnect(void)
{
  g_backoff = BACKOFF_MIN_MS;
  switch (g_join) {
  case J_WAITING:
    g_deadline = g_now;           /* join at the next service */
    break;
  case J_LEAVING:
  case J_JOINING:
  case J_KEYING:
    break;
  default:                        /* up, or given up */
    leave_then_join();
    break;
  }
}

/* keys in, as dswifi does after its handshake */
static DSCM_COLD void keys_done(const wpa_out_t* o, int pairwise)
{
  uint8_t one = 1;
  if (pairwise) {
    wmi_cmd(WMI_SYNCHRONIZE_CMD, &one, 1);
    data_sync();
    data_sync();
    add_key(0, 0, DSCM_CIPHER_CCMP, g_wpa.tk, 16, NULL);
  }
  if (o->install_gtk)
    add_key(o->gtk_index, 1, g_config.ap_group_cipher, o->gtk, o->gtk_len, o->rsc);
  if (pairwise)
    wmi_cmd(WMI_SYNCHRONIZE_CMD, &one, 1);
}

static DSCM_COLD void handle_eapol(const uint8_t* src, const uint8_t* eapol, uint32_t len)
{
  static wpa_out_t o;
  uint8_t snonce[32];
  int r;
  if (g_config.ap_security != DSCM_AP_WPA2 || !g_config.pmk_valid)
    return;                       /* no key: leave the launcher's association as it is */
  if (g_join == J_LEAVING || g_join == J_WAITING || g_join == J_JOINING) {
    /* the AP talks to us, so an association exists whose connect event we
     * missed (or the firmware made it by itself): start the handshake fresh */
    wpa_begin(&g_wpa, g_config.pmk, g_config.mac, src, g_config.ap_group_cipher);
    g_join = J_KEYING;
    g_deadline = g_now + KEY_TIMEOUT_MS;
  }
  entropy_bytes(snonce, 32);
  r = wpa_rx(&g_wpa, eapol, len, snonce, &o);
  if (r == WPA_REPLY) {
    send_eapol(src, o.eapol, o.eapol_len);
    if (o.install_ptk || o.install_gtk)
      keys_done(&o, o.install_ptk);
    if (o.install_ptk && !g_connected)
      set_up(1);
  } else if (r == WPA_NEED_PTK && g_join == J_UP) {
    if (g_need_ptk < 0xFF)
      g_need_ptk++;
    leave_then_join();            /* the launcher's keys: only a new association gets us ours */
  } else if (r == WPA_UNSUPPORTED && g_join == J_KEYING) {
    g_join = J_GIVEN_UP;          /* a handshake we can't finish */
  }
  memset(&o, 0, sizeof(o));
  memset(snonce, 0, sizeof(snonce));
}

/* a deadline passed (link_service) */
static DSCM_COLD void join_deadline(void)
{
  switch (g_join) {
  case J_WAITING:
    join();
    break;
  case J_LEAVING:                 /* no event for our disconnect: go on anyway */
    wait_then_join();
    break;
  case J_JOINING:
    if (g_join_timeouts < 0xFF)
      g_join_timeouts++;
    abandon_attempt();
    break;
  case J_KEYING:
    if (g_key_timeouts < 0xFF)
      g_key_timeouts++;
    abandon_attempt();
    break;
  default:
    break;
  }
}

/* ---- RX ---- */

static DSCM_COLD void handle_wmi(uint16_t id, const uint8_t* p, uint32_t len)
{
  if (id == WMI_DISCONNECT_EVENT) {
    /* protocol reason/status (2), BSSID (6), the firmware's reason (1) */
    int asked = 0;
    g_disc_events++;
    if (len >= 9) {
      g_last_proto = (uint16_t)(p[0] | (p[1] << 8));
      g_last_reason = p[8];
      asked = g_last_reason == DISC_REASON_CMD;
      if (g_join == J_UP && g_connected) {
        g_cause_proto = g_last_proto;   /* this one took the link down */
        g_cause_reason = g_last_reason;
      }
    }
    switch (g_join) {
    case J_LEAVING:               /* settled (ours, or one that crossed ours) */
      wait_then_join();
      break;
    case J_JOINING:
    case J_KEYING:                /* the attempt failed */
      attempt_failed();
      if (asked)
        wait_then_join();
      else
        leave_then_join();
      break;
    case J_WAITING:
    case J_GIVEN_UP:
      set_up(0);
      if (!asked && g_join == J_WAITING)
        leave_then_join();        /* the firmware's own attempts: settle it, keep the pause */
      break;
    default:                      /* J_UP: the association went */
      if (asked)
        wait_then_join();
      else
        leave_then_join();
      break;
    }
  } else if (id == WMI_CONNECT_EVENT) {
    const uint8_t* bssid = len >= 8 ? p + 2 : g_config.ap_bssid;
    g_connect_events++;
    if (g_config.ap_security == DSCM_AP_WPA2) {
      if (!g_config.pmk_valid) {
        g_join = J_GIVEN_UP;
        return;
      }
      set_up(0);
      wpa_begin(&g_wpa, g_config.pmk, g_config.mac, bssid, g_config.ap_group_cipher);
      g_join = J_KEYING;
      g_deadline = g_now + KEY_TIMEOUT_MS;
    } else {
      uint8_t one = 1;
      wmi_cmd(WMI_SYNCHRONIZE_CMD, &one, 1);
      data_sync();
      data_sync();
      wmi_cmd(WMI_SYNCHRONIZE_CMD, &one, 1);
      set_up(1);
    }
  } else if (id == WMI_CMD_ERROR_EVENT && len >= 3) {
    /* command id (2), error code (1): 1 bad parameter, 2 illegal state */
    uint16_t cmd = (uint16_t)(p[0] | (p[1] << 8));
    g_cmd_errors++;
    g_last_cmd_error = cmd | ((uint32_t)p[2] << 16);
    if (cmd == WMI_CONNECT_CMD && g_join == J_JOINING) {
      if (g_join_timeouts < 0xFF)
        g_join_timeouts++;        /* refused: no connect event will come */
      abandon_attempt();
    }
  }
}

static void handle_frame(const uint8_t* d, uint32_t avail)
{
  uint32_t len, trailer, plen, dlen;
  const uint8_t* p = d + 6;
  struct pbuf* pb;

  if (avail < 6)
    return;
  len = d[2] | ((uint32_t)d[3] << 8);
  trailer = (d[1] & 2) ? d[4] : 0;
  if (len + 6 > avail || trailer > len)
    return;
  plen = len - trailer;

  if (d[0] == HTC_EP_WMI && plen >= 2) {
    handle_wmi((uint16_t)(p[0] | (p[1] << 8)), p + 2, plen - 2);
    return;
  }
  if (d[0] != HTC_EP_DATA || plen < 24)
    return;
  dlen = ((uint32_t)p[14] << 8) | p[15];
  if (dlen < 8 || 16 + dlen > plen)
    return;
  if (p[22] == 0x88 && p[23] == 0x8E) {
    handle_eapol(p + 8, p + 24, dlen - 8);
    return;
  }

  /* rebuild an Ethernet II frame: dst, src, ethertype, payload */
  pb = pbuf_alloc(PBUF_RAW, (u16_t)(14 + dlen - 8), PBUF_POOL);
  if (!pb)
    return;
  pbuf_take_at(pb, p + 2, 6, 0);
  pbuf_take_at(pb, p + 8, 6, 6);
  pbuf_take_at(pb, p + 22, 2, 12);
  pbuf_take_at(pb, p + 24, (u16_t)(dlen - 8), 14);
  if (g_netif.input(pb, &g_netif) != ERR_OK)
    pbuf_free(pb);
  g_rx_frames++;
}

static void pump_rx(void)
{
  uint32_t head = arm7_rx_head();
  while (g_ring.rx_tail != head) {
    shim_slot_rx_t* slot = &g_ring.rx[g_ring.rx_tail % SHIM_RX_SLOTS];
    uint32_t n;
    dscm_dc_invalidate(slot, sizeof(*slot));
    n = slot->len <= SHIM_RX_MAX ? slot->len : SHIM_RX_MAX;
    link_stir();
    handle_frame(slot->data, n);
    g_ring.rx_tail++;
    publish_arm9_line();
  }
}

/* ---- setup and service ---- */

void* link_init(const net_config_t* config)
{
  ip4_addr_t ip, mask, gw;
  g_config = *config;
  memset(&g_ring, 0, sizeof(g_ring));
  g_ring.magic = SHIM_MAGIC;
  g_ring.run = 1;
  dscm_dc_clean(&g_ring, sizeof(g_ring));

  lwip_init();
  ip.addr = config->ip;
  mask.addr = config->netmask;
  gw.addr = config->gateway;
  netif_add(&g_netif, &ip, &mask, &gw, NULL, netif_setup, ethernet_input);
  netif_set_default(&g_netif);
  netif_set_up(&g_netif);
  netif_set_link_up(&g_netif);
  entropy_stir(config->mac, 6);
  /* answers a PTK rekey on the launcher's association; a group rekey needs a rejoin */
  if (config->ap_security == DSCM_AP_WPA2 && config->pmk_valid)
    wpa_begin(&g_wpa, config->pmk, config->mac, config->ap_bssid, config->ap_group_cipher);
  return &g_ring;
}

/* VBlanks the card engine left the chip alone for the game's own (DS-mode)
 * wireless, since the start (the stream's pairing). Cold, apart from
 * link_paused: that one is hot code (hot.ld, hot-layout.ref). */
DSCM_COLD uint32_t link_paused_count(void)
{
  dscm_dc_invalidate((const void*)&g_ring.attached, offsetof(shim_shared_t, tx) - offsetof(shim_shared_t, attached));
  return g_ring.stats.paused_ntr_wifi;
}

/* Paused: the game is using its own (DS-mode) wireless, so the card engine
 * left the chip alone this VBlank. */
int link_paused(uint32_t* paused_count)
{
  static uint32_t last;
  uint32_t now;
  dscm_dc_invalidate((const void*)&g_ring.attached, offsetof(shim_shared_t, tx) - offsetof(shim_shared_t, attached));
  now = g_ring.stats.paused_ntr_wifi;
  *paused_count = now;
  if (now != last) {
    last = now;
    return 1;
  }
  return 0;
}

int link_rejoining(void)
{
  return g_join != J_UP && g_join != J_GIVEN_UP;
}

void link_service(uint32_t now_ms)
{
  g_now = now_ms;
  pump_rx();
  ctrl_flush();
  if (g_join != J_UP && g_join != J_GIVEN_UP && (int32_t)(g_now - g_deadline) >= 0)
    join_deadline();
  sys_check_timeouts();
}

/* Work that can't wait for the next frame: frames from the ARM7, control
 * messages for it, or an association in progress. */
int link_busy(void)
{
  return arm7_rx_head() != g_ring.rx_tail || g_ctrl_tail != g_ctrl_head
      || (g_join != J_UP && g_join != J_GIVEN_UP);
}

/* rejoins that got us back online */
uint32_t link_rejoins(void)
{
  return g_rejoins;
}

void link_diag(dss_link_status_t* out)
{
  dscm_dc_invalidate((const void*)&g_ring.attached, offsetof(shim_shared_t, tx) - offsetof(shim_shared_t, attached));
  out->shim_polls = g_ring.stats.polls;
  out->shim_idle_polls = g_ring.stats.idle_polls;
  out->shim_tx = g_ring.stats.tx_frames;
  out->shim_rx = g_ring.stats.rx_frames;
  out->shim_rx_ring_full = g_ring.stats.rx_ring_full;
  out->shim_sdio_errors = g_ring.stats.sdio_errors;
  out->shim_last_error = g_ring.stats.last_error;
  out->shim_last_int = g_ring.stats.last_int_status;
  out->shim_paused_wifi = g_ring.stats.paused_ntr_wifi;
  out->shim_paused_card = g_ring.stats.paused_card_read;
  out->arm9_rx = g_rx_frames;
  out->arm9_tx = g_tx_frames;
  out->arm9_tx_drops = g_tx_drops;
  out->rejoins = g_rejoins;
  out->tx_queued = (uint8_t)(g_ring.tx_head - g_ring.tx_tail);
  out->rx_queued = (uint8_t)(g_ring.rx_head - g_ring.rx_tail);
  out->shim_flags = (uint8_t)((g_ring.attached ? 1 : 0) | (g_ring.dead ? 2 : 0) | (g_ring.run ? 4 : 0));
  out->join = (uint8_t)g_join;
  out->disc_events = g_disc_events;
  out->connect_events = g_connect_events;
  out->cause_proto = g_cause_proto;
  out->last_proto = g_last_proto;
  out->cause_reason = g_cause_reason;
  out->last_reason = g_last_reason;
  out->join_timeouts = g_join_timeouts;
  out->key_timeouts = g_key_timeouts;
  out->need_ptk = g_need_ptk;
  memset(out->reserved, 0, sizeof(out->reserved));
  (void)g_cmd_errors;             /* for a later field: how often the firmware refused a command */
  (void)g_last_cmd_error;
}

void link_stats(uint32_t* rx, uint32_t* tx, uint32_t* drops)
{
  *rx = g_rx_frames;
  *tx = g_tx_frames;
  *drops = g_tx_drops;
}
