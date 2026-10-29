/*
 *
 * NOTE: this is a copy of cheevotron/streamer/ds_stream_protocol.h and of the
 * phone-side DsStreamProtocol.kt; keep all three identical.
 * Cheevotron DSi RAM stream, protocol version 1: the contract between the
 * console module (a DScheevos fork, GPL-3) and the Cheevotron app.
 * Normative text: cheevotron/docs/dsi-stream-protocol.md. The Kotlin side is
 * core/src/main/kotlin/com/alagumna/cheevotron/core/ds/DsStreamProtocol.kt;
 * keep the three in step.
 *
 * All integers little-endian. Every packet: dss_header_t then the payload.
 */
#ifndef DS_STREAM_PROTOCOL_H
#define DS_STREAM_PROTOCOL_H

#include <stdint.h>

#define DSS_PORT            47300
#define DSS_MAGIC           0x5244u      /* 'D', 'R' */
#define DSS_VERSION         1
#define DSS_MAX_PAYLOAD     1400         /* one Ethernet frame through the ARM7 shim */
#define DSS_HEADER_SIZE     8            /* sizeof(dss_header_t); matches the Kotlin HEADER_SIZE */

/* console -> phone */
#define DSS_T_HELLO         0x01
#define DSS_T_FRAME         0x02
#define DSS_T_PEEK_REPLY    0x03
#define DSS_T_STATUS        0x04
#define DSS_T_POPUP_STATE   0x05   /* what became of a POPUP (dss_popup_state_t) */
#define DSS_T_CODE          0x06   /* a piece of the game's IRQ dispatcher, once a game (dss_code_t) */
/* phone -> console */
#define DSS_T_TABLE         0x81
#define DSS_T_CHAIN         0x82
#define DSS_T_KEEPALIVE     0x83
#define DSS_T_PEEK          0x84
#define DSS_T_CONTROL       0x85
#define DSS_T_NOTICE        0x86   /* GameCube: a text line the console shows on the TV */
#define DSS_T_CUE           0x87   /* phone tells the console an unlock/leaderboard/etc. fired, for the LED cue */
#define DSS_T_PROBE         0x88   /* phone looks for consoles (session 0, no payload): an unpaired one answers HELLO to it */
#define DSS_T_READY         0x89   /* phone answers a launcher's HELLO (dss_ready_t): it will pair once the game runs */
#define DSS_T_POPUP         0x8A   /* a pop-up for the console's own screen (dss_popup_t + strings) */
#define DSS_T_BADGE         0x8B   /* a chunk of a pop-up's badge pixels (dss_badge_t + data) */

#define DSS_MAX_TABLE_WORDS 1024
#define DSS_TABLE_CHUNK     256
#define DSS_PEEK_MAX        1024
#define DSS_HELLO_MS        500
#define DSS_PAIR_TIMEOUT_MS 3000
#define DSS_FULL_EVERY      60           /* frames between FULL keyframes in delta mode */

/* RetroAchievements DS addresses */
#define DSS_MAIN_RAM_END    0x003FFFFFu  /* 0x02000000 + a */
#define DSS_DTCM_START      0x01000000u  /* DTCM base + (a - DSS_DTCM_START) */
#define DSS_DTCM_END        0x01003FFFu

#define DSS_CAP_DTCM        0x01
#define DSS_CAP_LAUNCHER    0x02         /* HELLO from the launcher before the game: answer READY, don't pair */
#define DSS_CAP_POPUP       0x04         /* the console draws pop-ups on its screen (POPUP, BADGE, CTRL_DISPLAY) */

#define DSS_FRAME_FULL      0x01
#define DSS_FRAME_STALE     0x02
#define DSS_FRAME_PAUSED    0x04

#define DSS_CTRL_FULL       1
#define DSS_CTRL_DELTA      2
#define DSS_CTRL_RESET      3
#define DSS_CTRL_DISPLAY    4            /* argument: DSS_DISPLAY_* (off until the phone says) */

/* DSS_CTRL_DISPLAY argument: how far the pop-up strip goes */
#define DSS_DISPLAY_OFF     0
#define DSS_DISPLAY_DRY     1            /* guards and the IRQ wrapper only: no DMA, nothing on screen */
#define DSS_DISPLAY_NOOP    2            /* also the DISPCNT writer, with the game's own value on every line */
#define DSS_DISPLAY_ON      3            /* pop-ups on screen (line path: the game's CPU held through the band) */
#define DSS_DISPLAY_ON_REARM 4           /* test: on, and armed again in a frame an IRQ ended the band early */
#define DSS_DISPLAY_ON_STRICT 5          /* on, but every IRQ ends the band (no IPC interrupt passes) */
#define DSS_DISPLAY_ON_HOLD 6            /* the same as DSS_DISPLAY_ON since the hold became its default (it was the
                                            A/B test of holding the game's CPU in our IRQ through the band's lines) */

/* DSS_T_CUE body[0]: what the phone unlocked, so the console can blink its LED */
#define DSS_CUE_UNLOCK      0
#define DSS_CUE_LEADERBOARD 1
#define DSS_CUE_PROGRESS    2

typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t  type;
  uint8_t  version;
  uint16_t session;        /* 0 in HELLO */
  uint16_t length;         /* payload bytes */
} dss_header_t;            /* 8 */

typedef struct __attribute__((packed)) {
  uint8_t  caps;
  uint8_t  reserved;
  uint16_t max_table_words;
  uint16_t max_chain_nodes;
  uint16_t reserved2;
  uint32_t vblank;
  char     hash[32];       /* lowercase hex, no terminator */
  char     game_code[4];
  char     title[12];
} dss_hello_t;             /* 60 */

typedef struct __attribute__((packed)) {
  uint16_t generation;
  uint16_t total_words;
  uint16_t chunk_index;
  uint16_t chunk_words;
  /* uint32_t address[chunk_words] */
} dss_table_t;             /* 8 */

typedef struct __attribute__((packed)) {
  uint32_t vblank;
  uint16_t generation;
  uint8_t  flags;
  uint8_t  reserved;
  uint16_t count;
  /* FULL: uint8_t word[count][4] in table order
   * DELTA: struct { uint16_t index; uint8_t word[4]; } change[count] */
} dss_frame_t;             /* 10 */

typedef struct __attribute__((packed)) {
  uint16_t request;
  uint16_t length;
  uint32_t address;
} dss_peek_t;              /* 8 */

typedef struct __attribute__((packed)) {
  uint16_t request;
  uint16_t length;         /* 0: unreadable */
  uint32_t address;
  uint32_t vblank;
  /* uint8_t data[length] */
} dss_peek_reply_t;        /* 12 */

typedef struct __attribute__((packed)) {
  uint32_t last_frame_seen;
} dss_keepalive_t;         /* 4 */

typedef struct __attribute__((packed)) {
  uint8_t command;         /* DSS_CTRL_* */
  uint8_t argument;
} dss_control_t;           /* 2 */

/* DSS_T_READY body: the phone that will take the game's stream */
#define DSS_READY_OK        0            /* listening; the game will pair when it starts */
#define DSS_READY_BUSY      1            /* another console is playing: the phone will ask to switch */
#define DSS_READY_LOGGED_OUT 2           /* no RetroAchievements login on the phone */

typedef struct __attribute__((packed)) {
  uint8_t  state;          /* DSS_READY_* */
  uint8_t  reserved;
  uint16_t reserved2;
  char     name[28];       /* the phone's name for the screen, NUL-padded */
} dss_ready_t;             /* 32 */

typedef struct __attribute__((packed)) {
  uint32_t vblank;
  uint32_t frames_sent;
  uint32_t frames_stale;
  uint32_t peeks_answered;
  uint32_t net_rejoins;
  uint8_t  net_state;      /* DSCM_NET_STATE_* */
  uint8_t  shim_dead;
  uint16_t reserved;
  uint32_t idle_lines_max;
} dss_status_t;            /* 28; a console with DSS_CAP_POPUP appends dss_display_status_t */

/* ---- pop-ups on the console's screen (DSS_CAP_POPUP) ----
 * A full-width band at the bottom edge of the screen the game's main display
 * engine drives, while the game is idle. The phone sends a BADGE (if any),
 * then the POPUP, again every 300 ms until a POPUP_STATE for its id arrives;
 * one pop-up in flight at a time. Presentation only: nothing here gates an
 * unlock. */

#define DSS_POPUP_UNLOCK      0
#define DSS_POPUP_MASTERY     1
#define DSS_POPUP_LEADERBOARD 2
#define DSS_POPUP_PROGRESS    3
#define DSS_POPUP_STATUS      4
#define DSS_POPUP_TEST        5

#define DSS_POPUP_TOAST       0            /* 32 lines: badge, label and value, title, detail */
#define DSS_POPUP_SMALL       1            /* 16 lines: small badge, title and value */

#define DSS_POPUP_POS_TOP     0x01         /* top edge (not drawn yet: the bottom edge is used) */
#define DSS_POPUP_POS_RIGHT   0x02         /* badge and text on the right */

#define DSS_POPUP_FLAG_HARDCORE 0x01

#define DSS_POPUP_LABEL_MAX   32
#define DSS_POPUP_TITLE_MAX   64
#define DSS_POPUP_DETAIL_MAX  96
#define DSS_POPUP_VALUE_MAX   16

typedef struct __attribute__((packed)) {
  uint16_t popup_id;       /* nonzero, rising; the console drops repeats of recent ids */
  uint8_t  kind;           /* DSS_POPUP_UNLOCK.. */
  uint8_t  style;          /* DSS_POPUP_TOAST, DSS_POPUP_SMALL */
  uint8_t  position;       /* DSS_POPUP_POS_* */
  uint8_t  flags;          /* DSS_POPUP_FLAG_* */
  uint16_t hold_frames;    /* 0: 180 */
  uint16_t badge_id;       /* 0: the built-in icon for the kind */
  uint16_t reserved;
  /* then four strings, each a length byte and that many UTF-8 bytes:
   * label (at most 32: "Achievement unlocked"; empty: the kind's own),
   * title (64), detail (96), value (16: "10 pts", right-aligned) */
} dss_popup_t;             /* 12 */

#define DSS_BADGE_BGR555      0            /* row-major, bit 15 clear, blended onto DSS_POPUP_BACKGROUND */
#define DSS_BADGE_CHUNK_MAX   1024
#define DSS_POPUP_BACKGROUND  0x1C83       /* BGR555 of the band: RGB 3, 4, 7 of 31 */

typedef struct __attribute__((packed)) {
  uint16_t badge_id;
  uint8_t  side;           /* 32 (toast) or 16 (small) */
  uint8_t  format;         /* DSS_BADGE_BGR555 */
  uint16_t offset;         /* bytes into the badge */
  uint16_t total;          /* side * side * 2 */
  /* uint8_t data[] (at most DSS_BADGE_CHUNK_MAX) */
} dss_badge_t;             /* 8 */

#define DSS_POPUP_QUEUED      0
#define DSS_POPUP_SHOWN       1            /* the first frame it was on screen */
#define DSS_POPUP_DONE        2
#define DSS_POPUP_REFUSED     3
#define DSS_POPUP_DROPPED     4

#define DSS_POPUP_WHY_NONE        0
#define DSS_POPUP_WHY_OFF         1        /* pop-ups are off (CTRL_DISPLAY) */
#define DSS_POPUP_WHY_GAME        2        /* the game can't show them (display or IRQ setup) */
#define DSS_POPUP_WHY_VECTOR      3        /* no IRQ vector to hook (low vectors or none) */
#define DSS_POPUP_WHY_WAITED      4        /* nothing could be shown for 10 s */
#define DSS_POPUP_WHY_BUSY        5        /* the console's queue is full */
#define DSS_POPUP_WHY_NO_BADGE    6        /* shown with the built-in icon: the badge didn't arrive */
#define DSS_POPUP_WHY_BAD         7        /* malformed */

typedef struct __attribute__((packed)) {
  uint16_t popup_id;
  uint8_t  state;          /* DSS_POPUP_QUEUED.. */
  uint8_t  reason;         /* DSS_POPUP_WHY_* */
  uint16_t frames_shown;   /* frames the band was drawn whole */
  uint16_t frames_cut;     /* frames a game interrupt ended it early */
  uint16_t frames_waited;  /* frames it waited for a chance to show */
  uint8_t  screen;         /* bit 0: the game's main engine is on the top screen */
  uint8_t  edge;           /* 0 bottom, 1 top */
} dss_popup_state_t;       /* 12 */

/* After dss_status_t, from a console with DSS_CAP_POPUP: the strip's counters
 * since the game started. */
typedef struct __attribute__((packed)) {
  uint8_t  mode;           /* DSS_DISPLAY_* in force */
  uint8_t  screen;         /* bit 0: the main engine is on the top screen now */
  uint16_t last_cut_line;  /* VCOUNT of the last early end inside the band */
  uint32_t last_cut_irqs;  /* IE & IF then */
  uint32_t frames_wanted;  /* frames a pop-up was waiting or up */
  uint32_t frames_shown;   /* the band drawn whole (dry runs: would have been) */
  uint32_t frames_cut;     /* ended inside the band */
  uint32_t arms;           /* the band set up (re-arming after an early end counts again) */
  uint32_t early;          /* ended before the band's first line */
  uint32_t skip_late;      /* the game was busy until too late in the frame */
  uint32_t skip_pending;   /* an interrupt was due */
  uint32_t skip_display;   /* display mode, forced blank, fade or capture */
  uint32_t skip_dma;       /* a DMA or NDMA channel was busy */
  uint32_t skip_irq;       /* no VBlank interrupt, or an H-Blank one */
  uint32_t skip_vector;    /* the IRQ vector or the high-vector bit */
  uint32_t skip_screen;    /* the main engine changed screens recently */
  uint32_t vector_lost;    /* the IRQ vector changed under an armed band (a soft reset, or the game) */
  uint16_t hold_out;       /* CPU holds (DSS_DISPLAY_ON_HOLD) ended by their bound or an odd VCOUNT, the band
                              left up (saturates; was a FIFO depth measurement, then 0) */
  uint8_t  nwram;          /* the last band was fed from shared WRAM (else main RAM) */
  uint8_t  reserved3;
  /* while pop-ups are up, why frames went without the band */
  uint32_t shown_bits;     /* the last 32 frames, bit 0 the newest: 1 = the band was drawn (whole or cut) */
  uint32_t wake_early;     /* no band, though the game was idle in time (an IRQ, a skip reason) */
  uint32_t wake_late;      /* no band: the game first idled during the band's lines */
  uint32_t wake_vblank;    /* no band: the game idled only in VBlank */
  uint32_t wake_none;      /* no band: the game never idled in the frame */
  /* the IPC receive-FIFO interrupt with the band up (start.s dscm_irq_wrapper) */
  uint32_t irq_passed;     /* its handler ran and the band went on */
  uint32_t irq_woke;       /* its handler woke a thread or set a check flag: the band ended */
  uint8_t  pass_lost;      /* handlers that never came back to the wrapper (saturates; passing then stops for the game) */
  uint8_t  reserved6;
  uint8_t  pass_on;        /* the interrupt may pass now */
  uint8_t  dispatcher;     /* the game's IRQ dispatcher at the last arming: bits 0-3 its NitroSDK variant (1 V1,
                              2 V2, 0 not recognised), 0x10 its ThreadSwitch from SDK 3.x/4.x, 0x20 from SDK 2.0
                              (passing needs one of those two) */
  /* the line path (the band switched on by our own VCOUNT IRQ every frame, up
     through the game's IRQs and threads; frames_shown, frames_cut and arms count
     its bands too) */
  uint8_t  line;           /* 0 not installed, 1 installed, 2 left to the game for good (its VCOUNT IRQ) */
  uint8_t  line_stage;     /* 0 watch (our VCOUNT IRQ only), 1 dry (the feed, the game's mode written), 2 on */
  uint8_t  line_standdown; /* why the VCOUNT IRQ was left to the game: 1 LYC changed, 2 LYC bit 8, 3 its
                              table slot changed, 4 it kept turning ours off */
  uint8_t  line_skipwhy;   /* guards that held a band back (bits): 1 no VBlank IRQ, 2 an H-Blank IRQ, 4 display
                              mode or forced blank, 8 fade or capture, 16 the other screen, 32 NDMA2 busy, 64 a
                              legacy DMA feeding the FIFO */
  uint32_t line_vc;        /* our VCOUNT IRQs */
  uint32_t line_late;      /* too late in the line for that frame */
  uint32_t line_skip;      /* held back by a guard */
  uint32_t line_leak;      /* mode 3 with no band of ours up, put back to 1 (at an IRQ or an idle pass) */
  uint32_t line_mode0;     /* a band ended in display mode 0 (a GX_DispOff during it) */
  uint32_t line_reassert;  /* our VCOUNT IRQ's enables put back after the game cleared them */
  uint32_t line_ended;     /* bands ended early: IPC sync (nds-bootstrap's menu, reset), an IRQ in VBlank */
  uint16_t line_latency;   /* the most lines from LYC to our VCOUNT IRQ */
  uint16_t line_spin;      /* the most spin loops to the band's H-Blank */
  uint16_t dispmode_fixed; /* GX_DispOff's saved mode found 3 and put back to 1 */
  uint8_t  dispoff;        /* GX_DispOff in the game: 0 looking, 1 found, 2 not found */
  uint8_t  reserved7;
  /* the line path's feed: what else used the bus around a band (a FIFO
     underrun shifts the rest of the band to the right), and the hold test */
  uint32_t feed_dma_seen;  /* seen busy at a band's start or its VBlank (bits): 0-3 DMA0-3, 4-7 NDMA0-3 (6 never:
                              NDMA2 is ours), 8-11 DMA0-3 feeding the geometry FIFO, 12 the geometry engine,
                              13 its command FIFO not empty, 14 that FIFO half full or more */
  uint32_t feed_busy_start;/* bands that started with another DMA channel busy */
  uint32_t feed_busy_end;  /* bands that reached the VBlank with one busy */
  uint32_t feed_gx_start;  /* bands that started with the geometry FIFO half full or more */
  uint32_t feed_gx_end;    /* bands that reached the VBlank so */
  uint32_t feed_held;      /* bands with the game's CPU held to their end (DSS_DISPLAY_ON_HOLD) */
  uint32_t feed_hold_ipc;  /* holds ended early: an IPC sync was due (nds-bootstrap's menu or reset) */
} dss_display_status_t;    /* 164; the skips count frames, once per reason */

/* After dss_display_status_t: the Wi-Fi link through the ARM7 shim (the
 * module's net/link.c and the card engine's dscm_shim.h ring), to see what
 * fails in the seconds before the phone stops hearing the console. */
typedef struct __attribute__((packed)) {
  uint32_t shim_polls;     /* the ARM7 shim's polls of the chip (one a VBlank while it runs) */
  uint32_t shim_idle_polls;/* polls with nothing to do */
  uint32_t shim_tx;        /* frames the shim moved to the chip */
  uint32_t shim_rx;        /* and from it */
  uint32_t shim_rx_ring_full; /* polls that left data in the chip: the RX ring was full */
  uint32_t shim_sdio_errors;
  uint32_t shim_last_error;/* SDIO STAT1 error bits | step << 16 */
  uint32_t shim_last_int;  /* the chip's HOST_INT_STATUS at the last poll */
  uint32_t shim_paused_wifi; /* VBlanks skipped: the game used its DS wireless */
  uint32_t shim_paused_card; /* VBlanks skipped: the card engine had an SD read in flight */
  uint32_t arm9_rx;        /* frames the ARM9 took from the ring */
  uint32_t arm9_tx;        /* frames the ARM9 put into it */
  uint32_t arm9_tx_drops;  /* frames dropped: the TX ring was full */
  uint32_t rejoins;        /* associations the module started itself */
  uint8_t  tx_queued;      /* TX ring slots the ARM7 hasn't taken yet (of 4) */
  uint8_t  rx_queued;      /* RX ring slots the ARM9 hasn't taken yet (of 8) */
  uint8_t  shim_flags;     /* bit 0 attached (the ARM7 owns the chip), 1 dead (it gave up), 2 run */
  uint8_t  join;           /* the association: 0 up, 1 leaving, 2 waiting, 3 joining, 4 keying, 5 given up */
  uint32_t disc_events;    /* the chip's disconnect events */
  uint32_t connect_events; /* its connect events (our rejoins' associations) */
  uint16_t cause_proto;    /* the disconnect that took an up link down: its 802.11 reason or status code */
  uint16_t last_proto;     /* the latest disconnect's (rejoin attempts, our own commands) */
  uint8_t  cause_reason;   /* the firmware's reason for it: 1 no network, 2 link lost (beacons), 3 our command, 4 the
                              AP dropped us, 5 auth failed, 6 assoc failed, 7 no resources, 8 CSERV, 9 bad profile */
  uint8_t  last_reason;    /* the latest disconnect's */
  uint8_t  join_timeouts;  /* association attempts with no connect event in time (saturates) */
  uint8_t  key_timeouts;   /* 4-way handshakes that didn't finish in time (saturates) */
  uint8_t  need_ptk;       /* AP EAPOL we couldn't answer with the launcher's keys (a group rekey), so we left
                              to rejoin (saturates) */
  uint8_t  reserved[3];
} dss_link_status_t;       /* 80 */

/* DSS_T_CODE: 48 bytes of the game's IRQ dispatcher code, sent once a game
 * (the dispatcher and OS_IrqHandler_ThreadSwitch), so an unrecognised one
 * can be looked at from the phone's log. */
typedef struct __attribute__((packed)) {
  uint32_t address;
  uint8_t  bytes[48];
} dss_code_t;              /* 52 */

#endif
