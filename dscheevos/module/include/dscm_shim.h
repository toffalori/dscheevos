/*
 * DScheevos network ring: shared memory between the module's network stack
 * (ARM9) and the SDIO shim in nds-bootstrap's ARM7 card engine. A copy of
 * this file lives in the nds-bootstrap fork (retail/common/include); keep
 * them identical.
 *
 * The shim is a dumb pipe between these rings and the Atheros mailbox 0:
 *  - TX: the ARM9 queues complete HTC frames (6-byte HTC header + payload,
 *        any endpoint: 1 = WMI control, 2 = best-effort data). The shim pads
 *        to the 128-byte SDIO block size and writes them to mailbox 0.
 *  - RX: the shim copies every HTC frame the chip has for the host (WMI
 *        events, data frames, credit reports) into an RX slot, unparsed;
 *        with the ring full it reads them out and throws them away.
 * All protocol work (Ethernet, WMI, EAPOL, credits) happens on the ARM9.
 *
 * Single producer / single consumer per ring; indices only ever increase
 * (slot = index % N). Fields written by the ARM9 and fields written by the
 * ARM7 never share a 32-byte cache line, so a cached ARM9 can clean its own
 * lines and invalidate the ARM7's without losing data.
 */
#ifndef SHIM_SHARED_H
#define SHIM_SHARED_H

#include <stdint.h>

#define SHIM_MAGIC        0x4D494853u  /* "SHIM" */
#define SHIM_BLOCK        128u
#define SHIM_TX_MAX       0x600u       /* 12 blocks: what the chip's mailbox takes */
#define SHIM_RX_MAX       0x680u       /* 13 blocks: 1500-byte frame + headers + trailer */
#define SHIM_TX_SLOTS     4u
#define SHIM_RX_SLOTS     8u

#define SHIM_LINE __attribute__((aligned(32)))

typedef struct shim_slot_tx_t {
  uint32_t len;                    /* HTC frame length incl. 6-byte header, unpadded */
  uint8_t  data[SHIM_TX_MAX] __attribute__((aligned(4)));
} SHIM_LINE shim_slot_tx_t;

typedef struct shim_slot_rx_t {
  uint32_t len;                    /* bytes read from the mailbox (padded) */
  uint32_t lookahead;              /* the 4-byte lookahead seen before reading */
  uint8_t  data[SHIM_RX_MAX] __attribute__((aligned(4)));
} SHIM_LINE shim_slot_rx_t;

typedef struct shim_stats_t {
  uint32_t polls;                  /* shim_poll() calls while running */
  uint32_t idle_polls;             /* polls that found nothing to do */
  uint32_t tx_frames, rx_frames;
  uint32_t rx_ring_full;           /* RX frames thrown away: the ring was full (read out of the chip all the same) */
  uint32_t sdio_errors;
  uint32_t last_error;             /* STAT1 error bits | step << 16 */
  uint32_t last_int_status;        /* F1 HOST_INT_STATUS of the last poll */
  /* Cost of shim_poll() inside the game, in scanlines (REG_VCOUNT deltas) */
  uint32_t lines_idle_max, lines_busy_max, lines_busy_sum;
  uint32_t paused_ntr_wifi;        /* VBlanks skipped: the game was using its NTR (DS) Wi-Fi */
  uint32_t ntr_powcnt2, ntr_power_us;  /* last seen POWCNT2 and W_POWER_US (diagnostics) */
  uint32_t paused_card_read;       /* VBlanks skipped: the card engine had an SD read in flight */
} shim_stats_t;

typedef struct shim_shared_t {
  /* ARM9-owned line */
  uint32_t magic;
  volatile uint32_t run;           /* 0 pauses the shim */
  volatile uint32_t tx_head;
  volatile uint32_t rx_tail;
  uint32_t arm9_pad[4];
  /* ARM7-owned lines */
  volatile uint32_t attached SHIM_LINE;  /* 1 once the ARM7 owns the chip */
  volatile uint32_t tx_tail;
  volatile uint32_t rx_head;
  volatile uint32_t dead;                /* 1: the chip stopped answering (polls go on, a second apart) */
  volatile shim_stats_t stats;
  /* slots */
  shim_slot_tx_t tx[SHIM_TX_SLOTS];      /* ARM9 writes */
  shim_slot_rx_t rx[SHIM_RX_SLOTS];      /* ARM7 writes */
} SHIM_LINE shim_shared_t;

#endif
