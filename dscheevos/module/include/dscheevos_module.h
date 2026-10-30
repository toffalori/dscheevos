/*
 * DScheevos ARM9 module ABI: the contract between the module binary
 * (dscheevos9.bin) and the nds-bootstrap hook points that load and call it.
 *
 * Memory (the DSi's extra RAM, reserved by shrinking nds-bootstrap's ROM
 * cache; code runs at DSCM_BASE, data through DSCM_DATA_ADDR's view):
 *
 *   DSCM_BASE           module image: header, code, data, bss
 *   DSCM_SESSION        session file: dscm_boot_t, then server responses
 *   DSCM_HEAP           heap (rcheevos game data, snapshots)
 *   DSCM_IRQ_STACK_TOP  spare (was the VBlank call's stack)
 *   DSCM_STACK_TOP      stack for header->halt (grows down)
 *
 * Before the game boots, nds-bootstrap's ARM7 copies dscheevos9.bin to
 * DSCM_BASE and the session file (written by the DScheevos launcher) to
 * DSCM_SESSION, then its ARM9 calls header->init(DSCM_SESSION). After that
 * only hooks call in:
 *   header->vblank()  no longer called (a stub, kept for the layout). The
 *                     ARM7 VBlank handler bumps header->vblank_count; the
 *                     module reads it in the game's idle time, before every
 *                     snapshot, and counts a frame per new count. Nothing of
 *                     DScheevos runs in the game's ARM9 interrupts.
 *   header->halt()    replaces the game's OS_Halt: runs evaluation work,
 *                     then waits for an interrupt as OS_Halt did.
 *   header->reset()   from nds-bootstrap's game reset (in-game menu, soft
 *                     reset), IRQs off: achievement state resets in idle.
 * The ARM7 also reads header->notify_count (kept in RAM, past the ARM9's
 * cache) to signal unlocks with the Wi-Fi LED, and header->net_ring, the
 * frame ring its SDIO shim fills and drains (dscm_shim.h).
 */
#ifndef DSCHEEVOS_MODULE_H
#define DSCHEEVOS_MODULE_H

#include <stdint.h>

#ifndef DSCM_BASE
#define DSCM_BASE        0x02D80000u
#endif
#define DSCM_SIZE        0x00200000u
#define DSCM_IMAGE_MAX   0x00080000u   /* code incl. TCP/IP and TLS, data, bss */
#define DSCM_SESSION     (DSCM_BASE + DSCM_IMAGE_MAX)
/* The session file: the snapshot arena's place (256 KB) holds it. A bigger
 * one (a set reply over ~255 KB: Pokemon Platinum, SoulSilver) runs on into
 * the heap's high piece; the module moves its replies to the top of the heap
 * for the load and then gives that memory back (heap.h). Nothing else in the
 * region changes: the game loses no memory to it. 384 KB: while it loads,
 * a set needs its reply until rc_api has parsed it, then rc_api's parse plus
 * rcheevos' game data in the heap; Platinum (312 KB) peaks at 946 KB of the
 * high piece's 1,297 KB. Pearl (409 KB) and SoulSilver (612 KB) have more
 * game data alone than the heap holds, so the launcher refuses them up front
 * rather than starting a load that fails after a long wait
 * (docs/architecture.md, "Big sets"; tests/perf/fitmem.sh).
 * nds-bootstrap's DSCHEEVOS_SESSION_MAX must match. */
#define DSCM_SESSION_MAX 0x00060000u
#define DSCM_HEAP        (DSCM_SESSION + 0x00040000u)
#define DSCM_STACK_SIZE  0x00002000u
#define DSCM_STACK_TOP   (DSCM_BASE + DSCM_SIZE)
#define DSCM_IRQ_STACK_SIZE 0x00001000u
#define DSCM_IRQ_STACK_TOP  (DSCM_STACK_TOP - DSCM_STACK_SIZE)
#define DSCM_HEAP_END    (DSCM_IRQ_STACK_TOP - DSCM_IRQ_STACK_SIZE)
/* The spare 4 KB above the heap holds the boot's timeline: nds-bootstrap's
 * stage 1 and bootloader write it (u32 magic, u32 length, then the text),
 * the stream sends it to the phone. nds-bootstrap's DSCHEEVOS_BOOTLOG must
 * match. */
#define DSCM_BOOT_LOG       DSCM_HEAP_END
#define DSCM_BOOT_LOG_SIZE  DSCM_IRQ_STACK_SIZE
#define DSCM_BOOT_LOG_MAGIC 0x4C425344u  /* "DSBL" */

/* Where the module's code sees its own data, heap, arena and session:
 * the cached view (DSCM_BASE, 0x02xxxxxx) or, with DSCM_DATA_MIRROR, the
 * 0x0C000000 mirror of the same RAM, which most games' MPUs leave uncached
 * (SDK5 games cache it: then nothing changes). Code always runs at
 * DSCM_BASE. Pointers handed to the ARM7 or the in-game menu are physical
 * (DSCM_PHYS). */
#ifdef DSCM_DATA_MIRROR
#define DSCM_DATA_HI 0x0C000000u
#else
#define DSCM_DATA_HI 0x02000000u
#endif
#define DSCM_DATA_ADDR(a) ((((uint32_t)(a)) & 0x00FFFFFFu) | DSCM_DATA_HI)
#define DSCM_PHYS(a)      ((((uint32_t)(a)) & 0x00FFFFFFu) | 0x02000000u)

#define DSCM_MAGIC   0x56435344u  /* "DSCV" */
#define DSCM_VERSION 7u
#define DSCM_SESSION_MAGIC 0x53435344u  /* "DSCS" */

/* Server responses fetched by the launcher at session start. */
enum {
  DSCM_PRELOAD_LOGIN = 0,      /* r=login2 */
  DSCM_PRELOAD_SET,            /* r=achievementsets */
  DSCM_PRELOAD_SESSION,        /* r=startsession */
  DSCM_PRELOAD_COUNT
};

/* One request or response in the shared network queue. The module fills
 * requests; the network layer (ARM7 or loader) answers them. */
#define DSCM_NET_SLOTS      4
#define DSCM_NET_URL_MAX    192
#define DSCM_NET_POST_MAX   768
#define DSCM_NET_REPLY_MAX  4096
enum { DSCM_NET_FREE = 0, DSCM_NET_REQUEST, DSCM_NET_IN_FLIGHT, DSCM_NET_REPLIED };

typedef struct dscm_net_slot_t {
  volatile uint32_t state;
  uint32_t sequence;
  int32_t http_status;
  uint32_t reply_length;
  char url[DSCM_NET_URL_MAX];
  char post[DSCM_NET_POST_MAX];
  char reply[DSCM_NET_REPLY_MAX];
} dscm_net_slot_t;

/* Unlocks and health, readable by the in-game menu and the network layer. */
#define DSCM_RECENT_MAX 8
typedef struct dscm_unlock_t {
  uint32_t id;
  uint32_t points;
  uint32_t frame;
  char title[48];
} dscm_unlock_t;

#define DSCM_HARDCORE_REFUSED 2

typedef struct dscm_status_t {
  volatile uint32_t active;          /* set loaded, frames being evaluated */
  volatile uint32_t hardcore;        /* 0 softcore, 1 hardcore, DSCM_HARDCORE_REFUSED: hardcore rules, but the server files unlocks as softcore */
  volatile uint32_t frames_snapshotted;
  volatile uint32_t frames_evaluated;
  volatile uint32_t frames_dropped;   /* snapshot taken, no slot free to evaluate it */
  volatile uint32_t frames_missed;    /* VBlanks that passed with no snapshot (IRQs held off) */
  volatile uint32_t eval_lines;       /* average wall time of one idle pass, in scanlines (x16) */
  volatile uint32_t unlock_count;     /* total; recent[] holds the last DSCM_RECENT_MAX */
  dscm_unlock_t recent[DSCM_RECENT_MAX];
  volatile uint32_t pending_requests;
  volatile uint32_t error;            /* nonzero: DScheevos stopped (see dscm_error_t) */
  char game_title[48];
  /* the core set, for the in-game menu */
  volatile uint32_t achievements;
  volatile uint32_t achievements_unlocked;
  volatile uint32_t points;
  volatile uint32_t points_unlocked;
  /* network, for the in-game menu */
  volatile uint32_t net_state;        /* DSCM_NET_STATE_* */
  volatile uint32_t net_detail;       /* last error code, 0 if none */
  volatile uint32_t unlocks_sent;     /* award requests the server answered */
  volatile uint32_t tls_handshake_ms; /* last HTTPS handshake, wall time (diagnostics) */
  volatile uint32_t net_rejoins;      /* times we reassociated after a drop or a group rekey */
  /* Self-timing, in scanlines read off REG_VCOUNT (the timers are the
   * game's). 263 lines a frame; one line is about 4,260 ARM9 cycles at
   * 67 MHz. A single reading is whole lines, but the start falls anywhere in
   * a line, so total / timed_frames is a fair average below one line too. */
  volatile uint32_t timed_frames;     /* VBlanks timed below */
  volatile uint32_t irq_lines_last;   /* the module's VBlank work, inside the game's IPC IRQ */
  volatile uint32_t irq_lines_max;
  volatile uint32_t irq_lines_total;
  /* the idle lines are added up in idle time: they lag while no pass runs */
  volatile uint32_t idle_lines_last;  /* idle work (halt path) in the last frame */
  volatile uint32_t idle_lines_max;
  volatile uint32_t idle_lines_total;
  volatile uint32_t idle_none;        /* frames in which no idle pass started or ended */
  volatile uint32_t idle_cut;         /* idle passes a VBlank preempted; the rest of such a pass is not counted */
  /* Snapshot when the game halts (appended after the self-timing). */
  volatile uint32_t captures;         /* snapshots taken at a halt */
  volatile uint32_t captures_stopped; /* ... stopped for an interrupt (taken again at the next halt) */
  volatile uint32_t frames_stale;     /* VBlanks with no snapshot since the last: the game didn't halt */
  volatile uint32_t halts_idle;       /* OS_Halt calls from NitroSDK's idle thread loop: the module's time */
  volatile uint32_t halts_other;      /* OS_Halt calls from elsewhere (a game thread waiting): left as stock */
  /* Appended: 1 if the game's MPU caches the 0x0C000000 mirror (SDK5), so
   * shared words use explicit cache maintenance instead of the mirror. */
  volatile uint32_t alias_cached;
  volatile uint32_t clock;            /* DSCM_CLOCK_*: the session's speed-ups (appended) */
} dscm_status_t;

enum {
  DSCM_NET_STATE_NONE = 0,            /* no network in this session */
  DSCM_NET_STATE_CONNECTING,
  DSCM_NET_STATE_ONLINE,
  DSCM_NET_STATE_SENDING,
  DSCM_NET_STATE_PAUSED,              /* the game is using its own wireless */
  DSCM_NET_STATE_OFFLINE,             /* the access point dropped us */
  DSCM_NET_STATE_ERROR,               /* last connection attempt failed */
};

enum {
  DSCM_ERROR_NONE = 0,
  DSCM_ERROR_BAD_BOOT,
  DSCM_ERROR_NO_MEMORY,
  DSCM_ERROR_LOGIN,
  DSCM_ERROR_GAME_LOAD,
};

/* Start of the session file (sd:/_nds/dscheevos/session.bin). The launcher
 * writes it after going online; nds-bootstrap uses it only for the ROM it
 * names, then deletes it, so every play session starts online. */
typedef struct dscm_boot_t {
  uint32_t magic;                     /* DSCM_SESSION_MAGIC */
  uint32_t size;                      /* sizeof(dscm_boot_t) */
  uint32_t file_size;                 /* whole session file, <= DSCM_SESSION_MAX */
  char rom_path[256];                 /* as in nds-bootstrap.ini NDS_PATH */
  char host[64];
  char username[32];
  char token[64];
  char game_hash[33];
  uint8_t hardcore;
  uint8_t flags;                      /* DSCM_BOOT_* */
  int8_t utc_offset_q;                /* DSCM_BOOT_UTC_KNOWN: console clock minus UTC, in quarter hours */
  uint32_t dtcm_base;                 /* 0: read the game's DTCM base from CP15 */
  uint32_t unix_time;                 /* console clock (local time read as UTC) when the launcher wrote the file */
  uint32_t preload_offset[DSCM_PRELOAD_COUNT];  /* from the start of this struct; each reply is followed by DSCM_SESSION_PAD's NULs */
  uint32_t preload_length[DSCM_PRELOAD_COUNT];
  /* the launcher's Wi-Fi connection, which the game keeps using (no secrets) */
  uint8_t mac[6];
  uint8_t net_valid;                  /* 0: no network during play */
  uint8_t clock;                      /* DSCM_CLOCK_*: 0 in the file, nds-bootstrap sets it in RAM */
  uint32_t ip, netmask, gateway, dns; /* network byte order */
  uint32_t ca_offset, ca_length;      /* extra trusted CAs (DER, one after the other), from the start of this struct */
  /* the access point, so the module can rejoin it (no secrets in the file) */
  char ap_ssid[32];
  uint8_t ap_ssid_len;
  uint8_t ap_bssid[6];
  uint8_t ap_channel;
  uint8_t ap_security;                /* DSCM_AP_* */
  uint8_t ap_pair_cipher;             /* DSCM_CIPHER_* */
  uint8_t ap_group_cipher;
  uint8_t pmk_valid;                  /* the two pmk fields are 0 in the file: nds-bootstrap */
  uint8_t pmk[32];                    /* fills them in RAM from the console's own Wi-Fi settings */
  uint32_t phone_ip;                  /* Cheevotron: the phone the launcher found (network byte order), 0 none */
} dscm_boot_t;

/* Bytes a reply (or the CA) of n bytes takes in the session file: itself,
 * then 1 to 4 NULs up to a multiple of 4. The module uses the replies in
 * place: the NUL ends them as C strings. */
#define DSCM_SESSION_PAD(n) (((uint32_t)(n) + 4u) & ~3u)

/* dscm_boot_t.flags */
#define DSCM_BOOT_SPECTATOR 0x01      /* rc_client spectator mode: evaluate and show, send no unlocks or entries (bench runs on a real account) */
#define DSCM_BOOT_STREAM    0x04      /* Cheevotron: the console streams RAM to the phone and runs no on-console RA (no placard) */
#define DSCM_BOOT_CART      0x08      /* Cheevotron: boot the inserted Slot-1 cartridge natively, not the SD ROM */
#define DSCM_BOOT_CART_NATIVE 0x10    /* with CART: the game reads its own cartridge; without it nds-bootstrap's card engine serves its reads */
/* dscm_boot_t.clock, dscm_status_t.clock, dscm_journal_t.clock: the speed-ups
 * the player chose in nds-bootstrap before launch (kept for the session) */
#define DSCM_CLOCK_CPU_133  0x01      /* ARM9 at 133 MHz (BOOST_CPU, or DSI_MODE = 2 on a DS game) */
#define DSCM_CLOCK_VRAM     0x02      /* BOOST_VRAM */
#define DSCM_BOOT_UTC_KNOWN 0x02      /* utc_offset_q is valid: the launcher measured it against the server's clock */

/* The DS clock keeps local time with no time zone; read as UTC it is off by
 * the zone's offset. The launcher measures that offset against the server's
 * Date header, rounded to a quarter hour (zones are whole quarter hours). */
#define DSCM_UTC_QUARTER 900
static inline uint32_t dscm_console_to_utc(uint32_t console_time, int8_t offset_q)
{
  return console_time - (uint32_t)((int32_t)offset_q * DSCM_UTC_QUARTER);
}

/* dswifi's numbering, which the launcher reads off its connection */
enum { DSCM_AP_OPEN = 0, DSCM_AP_WEP, DSCM_AP_WPA, DSCM_AP_WPA2 };
enum { DSCM_CIPHER_NONE = 1, DSCM_CIPHER_WEP, DSCM_CIPHER_TKIP, DSCM_CIPHER_CCMP };

/* ---- ABI v6: achievement list, notices, offline journal ---- */

/* Every achievement of the core set, for the in-game menu. The module builds
 * a complete copy in idle time and then publishes it by swapping
 * header->list, so a reader never sees a half-written list. Readers on the
 * ARM9 (the in-game menu) read it while the game is paused. */
#define DSCM_LIST_MAGIC 0x4C435344u   /* "DSCL" */
enum {
  DSCM_ACH_LOCKED = 0,
  DSCM_ACH_UNLOCKED,                  /* unlocked in this session's mode (hardcore in hardcore) */
  DSCM_ACH_UNLOCKED_SOFTCORE,         /* hardcore session, unlocked earlier in softcore only */
  DSCM_ACH_DISABLED,                  /* rcheevos can't evaluate it (bad address, unsupported) */
};
#define DSCM_ACH_FLAG_MEASURED   0x01 /* measured[] holds progress, e.g. "12/30" or "40%" */
#define DSCM_ACH_FLAG_CHALLENGE  0x02 /* its challenge indicator is showing now */
#define DSCM_ACH_FLAG_MISSABLE   0x04
#define DSCM_ACH_FLAG_PROGRESSION 0x08
#define DSCM_ACH_FLAG_WIN        0x10

typedef struct dscm_list_entry_t {
  uint32_t id;
  uint32_t unlock_time;               /* unix time, 0 if locked */
  uint16_t points;
  uint8_t state;                      /* DSCM_ACH_* */
  uint8_t flags;                      /* DSCM_ACH_FLAG_* */
  char title[48];                     /* UTF-8, NUL-terminated, cut to fit */
  char description[100];
  char measured[16];
} dscm_list_entry_t;                  /* 176 bytes */

typedef struct dscm_list_t {
  uint32_t magic;                     /* DSCM_LIST_MAGIC */
  uint32_t generation;                /* bumped on every rebuild */
  uint32_t count;                     /* entries in e[] */
  uint32_t entry_size;                /* sizeof(dscm_list_entry_t), for readers to check */
  dscm_list_entry_t e[];              /* game's display order */
} dscm_list_t;

/* Notices for a display layer (overlay, in-game menu): what rcheevos asks a
 * client to show during play. A ring of one-shot notices plus the indicators
 * that stay up while active. Written by the module in idle time; head is
 * bumped after a notice is complete. */
#define DSCM_NOTICES_MAGIC 0x4E435344u /* "DSCN" */
#define DSCM_NOTICE_RING 16
#define DSCM_INDICATOR_MAX 8
enum {
  DSCM_NOTICE_NONE = 0,
  DSCM_NOTICE_SESSION_START,          /* value: 1 hardcore, 0 softcore; text: game title; detail: "N/M achievements" */
  DSCM_NOTICE_UNLOCK,                 /* id, value: points, text: title, detail: description */
  DSCM_NOTICE_MASTERY,                /* text: game title */
  DSCM_NOTICE_PROGRESS,               /* id, text: title, detail: measured progress */
  DSCM_NOTICE_LEADERBOARD_STARTED,    /* id, text: title */
  DSCM_NOTICE_LEADERBOARD_FAILED,     /* id, text: title */
  DSCM_NOTICE_LEADERBOARD_SUBMITTED,  /* id, value: score, text: title, detail: formatted score */
  DSCM_NOTICE_LEADERBOARD_RANK,       /* id, value: rank, text: title, detail: "rank/entries" (server reply) */
  DSCM_NOTICE_OFFLINE,                /* unlocks are waiting: the server can't be reached */
  DSCM_NOTICE_ONLINE,                 /* everything waiting was sent */
};

typedef struct dscm_notice_t {
  uint32_t seq;                       /* 1, 2, ...: ring position is seq % DSCM_NOTICE_RING */
  uint16_t kind;                      /* DSCM_NOTICE_* */
  uint16_t reserved;
  uint32_t id;
  int32_t value;
  uint32_t frame;                     /* module frame count when raised */
  char text[48];
  char detail[48];
} dscm_notice_t;                      /* 116 bytes */

typedef struct dscm_indicator_t {
  uint32_t id;                        /* achievement (challenge) or leaderboard (tracker) id; 0 = slot free */
  char text[24];                      /* challenge: title; tracker: formatted value */
} dscm_indicator_t;

typedef struct dscm_notices_t {
  uint32_t magic;                     /* DSCM_NOTICES_MAGIC */
  volatile uint32_t head;             /* seq of the newest complete notice, 0 = none */
  dscm_notice_t ring[DSCM_NOTICE_RING];
  volatile uint32_t indicators_generation;   /* bumped whenever challenges[] or trackers[] change */
  dscm_indicator_t challenges[DSCM_INDICATOR_MAX];   /* challenge indicators showing now */
  dscm_indicator_t trackers[DSCM_INDICATOR_MAX];     /* leaderboard trackers showing now */
} dscm_notices_t;

/* Offline journal: every unlock and leaderboard entry of the session, with
 * whether the server has answered it. The module writes it through the data
 * cache and cleans the lines at every step of an update (the core's
 * journal_sync), so RAM always holds a consistent copy (the 0x0C000000
 * mirror is cached write-back on SDK5 games, so it can't be used); at safe
 * points (in-game menu opening, reset, exit) nds-bootstrap's ARM7 copies
 * DSCM_JOURNAL_FILE_SIZE bytes from header->journal to
 * sd:/_nds/dscheevos/pending.bin, a file the launcher creates at that size.
 * The launcher sends unsent entries at its next start, with the time since
 * the unlock (rcheevos' seconds_since_unlock), then clears the file. */
#define DSCM_JOURNAL_MAGIC 0x4A435344u /* "DSCJ" */
#define DSCM_JOURNAL_MAX 96
#define DSCM_JOURNAL_FILE_SIZE 4096u
/* The 0x0C000000 mirror of an address: uncached under most games' MPU, but
 * cached write-back under SDK5's, so only for reads that tolerate either. */
#define DSCM_UNCACHED(p) ((void*)(((uint32_t)(p) & 0x00FFFFFFu) | 0x0C000000u))
enum { DSCM_JOURNAL_AWARD = 1, DSCM_JOURNAL_LEADERBOARD };

typedef struct dscm_journal_entry_t {
  uint8_t kind;                       /* DSCM_JOURNAL_* */
  uint8_t hardcore;
  uint8_t sent;                       /* 1: the server answered (accepted or refused for good) */
  uint8_t reserved;
  uint32_t id;                        /* achievement or leaderboard */
  int32_t score;                      /* leaderboard entries */
  uint32_t unix_time;                 /* when it happened: see dscm_journal_t.time_base */
} dscm_journal_entry_t;               /* 16 bytes */

/* dscm_journal_t.time_base */
#define DSCM_JOURNAL_TIME_CONSOLE 0   /* console clock, its offset from UTC unknown: only differences are meaningful */
#define DSCM_JOURNAL_TIME_UTC     1   /* UTC */

typedef struct dscm_journal_t {
  uint32_t magic;                     /* DSCM_JOURNAL_MAGIC; 0 in a cleared file */
  uint32_t size;                      /* sizeof(dscm_journal_t) */
  uint32_t count;                     /* entries used */
  uint32_t game_id;
  char username[32];                  /* entries belong to this account only */
  char game_hash[33];
  uint8_t time_base;                  /* DSCM_JOURNAL_TIME_*: what the entries' unix_time is */
  uint8_t clock;                      /* DSCM_CLOCK_*: the session's speed-ups */
  uint8_t reserved;
  dscm_journal_entry_t e[DSCM_JOURNAL_MAX];
} dscm_journal_t;                     /* 1620 bytes, fits DSCM_JOURNAL_FILE_SIZE */

/* header->cues: what the ARM7 turns into Wi-Fi LED cues. Counters wrap;
 * the ARM7 acts on a change, not on the value. */
#define DSCM_CUE_PROGRESS_MASK    0x000000FFu   /* measured progress updates */
#define DSCM_CUE_LEADERBOARD_MASK 0x0000FF00u   /* leaderboard attempts started or finished */
#define DSCM_CUE_LEADERBOARD_SHIFT 8
#define DSCM_CUE_CHALLENGE        0x00010000u   /* a challenge indicator is showing */

/* Cheevotron streamer: the recording's ring (DSS_CAP_RECORD; the record
 * format is in ds_stream_protocol.h), for the ARM7 to mirror onto the SD
 * card, and the file it mirrors it into: dscm_rec_t and the file layout are
 * in dscm_record.h (shared with nds-bootstrap, like dscm_shim.h). The module
 * appends records and advances `written`, cleaning the ring's lines to RAM
 * as sectors complete; the ARM7 copies every completed DSS_REC_SECTOR of
 * the stream (sector k = stream bytes k*512..k*512+511, at
 * ring + (k*512 % ring_size)) before the ring overwrites it. */
#include "dscm_record.h"

typedef struct dscm_header_t {
  uint32_t magic;
  uint32_t version;
  uint32_t image_size;
  int  (*init)(const dscm_boot_t* boot);
  void (*vblank)(void);
  void (*halt)(void);                 /* ARM code: the OS_Halt patch jumps here */
  void (*shutdown)(void);
  dscm_status_t* status;
  dscm_net_slot_t* net;               /* DSCM_NET_SLOTS entries */
  volatile uint32_t vblank_count;     /* 0x24: written by the ARM7 VBlank hook */
  void (*reset)(void);                /* 0x28 */
  dscm_list_t* volatile list;         /* 0x2C: published achievement list, 0 until the set is loaded */
  dscm_notices_t* notices;            /* 0x30 */
  dscm_journal_t* journal;            /* 0x34: cached-mirror address; the ARM7 reads RAM directly */
  /* 0x38: ARM code. Call with the game stopped, before the ARM7 copies the
   * journal at a safe point (in-game menu, reset): evaluates the frames still
   * queued, so an unlock in the last moments is in the journal. Runs on the
   * module's stack; does nothing if idle work is running or parked there. */
  void (*flush)(void);
  uint32_t reserved;
  volatile uint32_t notify_count;     /* 0x40: unlocks so far; own cache line, cleaned to RAM */
  void* net_ring;                     /* 0x44: dscm_shim.h ring, 0 if none; same line */
  volatile uint32_t cues;             /* 0x48: LED cues for the ARM7 (DSCM_CUE_*); same cache line, cleaned to RAM */
  dscm_rec_t* volatile rec;           /* 0x4C: the streamer's recording (physical address), 0 if none */
  /* 0x50: ARM code, streamer only (0 in the on-console build). The in-game
   * menu calls it with the game stopped: reconnect at the next idle pass. */
  void (*reconnect)(void);
  uint32_t reserved2[3];
  /* 0x60: the VBlank IRQ's own state, then its code (module internals) */
} dscm_header_t;

#define DSCM_HEADER ((dscm_header_t*)DSCM_BASE)

#endif
