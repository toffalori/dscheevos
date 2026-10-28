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
#define DSS_DISPLAY_ON      3            /* pop-ups on screen */

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
} dss_display_status_t;    /* 64 */

#endif
