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

/* console -> phone */
#define DSS_T_HELLO         0x01
#define DSS_T_FRAME         0x02
#define DSS_T_PEEK_REPLY    0x03
#define DSS_T_STATUS        0x04
/* phone -> console */
#define DSS_T_TABLE         0x81
#define DSS_T_CHAIN         0x82
#define DSS_T_KEEPALIVE     0x83
#define DSS_T_PEEK          0x84
#define DSS_T_CONTROL       0x85

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

#define DSS_FRAME_FULL      0x01
#define DSS_FRAME_STALE     0x02
#define DSS_FRAME_PAUSED    0x04

#define DSS_CTRL_FULL       1
#define DSS_CTRL_DELTA      2
#define DSS_CTRL_RESET      3

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
} dss_status_t;            /* 28 */

#endif
