/*
 * Cheevotron streamer: the recording's ring in RAM and its copy on the SD
 * card. A copy of this file lives in the nds-bootstrap fork
 * (retail/common/include); keep the two identical.
 *
 * The module (ARM9, src/record.c) records every frame's changes of the
 * watched words into a ring (the record format is in ds_stream_protocol.h)
 * and keeps dscm_rec_t current, pointed to by dscm_header_t.rec. During
 * play nds-bootstrap's ARM7 card engine mirrors the ring into
 * sd:/_nds/dscheevos/recording.bin, one sector a VBlank at most: every
 * completed DSS_REC_SECTOR of the stream to its data sector, and now and
 * then the slot's header sector. The launcher creates the file at its full
 * size, so its clusters never change: no FAT or directory sector is written
 * while the game runs, and a power-off costs at most the seconds since the
 * last header. At its next start the launcher offers a run the phone hasn't
 * replayed (HELLO with DSS_CAP_RECORD) and answers REPLAY_REQ from the file.
 *
 * The file: sector 0 is dscm_recfile_t (the launcher writes it before every
 * stream session, naming the game it starts), then DSCM_REC_SLOTS slots of
 * DSCM_REC_SLOT_SECTORS sectors: a header sector (dscm_recslot_t) and
 * DSCM_REC_DATA_SECTORS data sectors, data sector j holding stream sector k
 * (bytes k*512 .. k*512+511) with k % DSCM_REC_DATA_SECTORS == j. Before the
 * game, nds-bootstrap's bootloader claims a slot for the run (the slot that
 * holds no recording, else one the phone has replayed, else the older one:
 * dscm_rec_pick_slot) and writes its header with the next seq. The launcher
 * offers the newest unreplayed slot (dscm_rec_offer_slot) and sets replayed
 * once the phone evaluated it to its end. Two slots: a run the phone missed
 * survives one more run; after that the older one is overwritten.
 */
#ifndef DSCM_RECORD_H
#define DSCM_RECORD_H

#include <stdint.h>

/* ---- the ring, shared between the module and the ARM7 ---- */

#define DSCM_REC_MAGIC      0x43455244u  /* "DREC" */
/* The module's fields end here: it cleans only these bytes of the struct to
 * RAM after init (record.c publish), so its cache never writes stale copies
 * over the ARM7's fields below. The module reads the ARM7's fields through
 * DSCM_UNCACHED or after invalidating their lines, and never writes them. */
#define DSCM_REC_ARM9_BYTES 64u

typedef struct dscm_rec_t {
  /* the module's lines */
  uint32_t magic;                     /* DSCM_REC_MAGIC */
  uint32_t ring;                      /* physical address of the ring */
  uint32_t ring_size;                 /* bytes, a multiple of DSS_REC_SECTOR */
  uint16_t run;                       /* the run id (HELLO's run), nonzero */
  uint16_t reserved;
  char     game_hash[32];             /* lowercase hex, no terminator */
  volatile uint32_t written;          /* stream bytes recorded so far (the ring holds the last ring_size of them) */
  volatile uint32_t segment;          /* stream offset of the latest segment */
  volatile uint32_t end_frame;        /* the frame of the last record */
  uint32_t reserved2;
  /* the ARM7's lines, from offset 64: its progress and costs, for the status */
  volatile uint32_t sd_written;       /* stream bytes on the SD card (whole sectors, each confirmed by the controller) */
  volatile uint32_t sd_valid_from;    /* the file's copy is continuous from this stream offset to sd_written */
  volatile uint32_t sd_skipped;       /* sectors the ring overwrote before they were written: lost from the file */
  volatile uint32_t sd_writes;        /* data sector writes started */
  volatile uint32_t sd_header_writes; /* header sector writes started */
  volatile uint32_t sd_errors;        /* writes the controller reported an error for (started again) */
  volatile uint32_t sd_lost;          /* writes whose end wasn't seen: another SD command cleared the controller (started again) */
  volatile uint32_t sd_busy;          /* VBlanks skipped: the SD card was in use (a ROM read, a save, the menu) */
  volatile uint32_t sd_lines_max;     /* the longest write start inside the VBlank handler, in scanlines */
  volatile uint32_t sd_lines_sum;     /* all write starts, to average over sd_writes + sd_header_writes */
  volatile uint32_t sd_finish_max;    /* VBlanks from a write's start to its end, the most */
  volatile uint32_t sd_last_error;    /* SD STATUS1 of the last failed write | its step << 16 */
  volatile uint32_t sd_state;         /* DSCM_REC_SD_* */
  uint32_t sd_reserved[3];
  /* offset 128: the slot's header sector as the ARM7 writes it (dscm_recslot_t, then zeros) */
  uint8_t  sd_header[512];
} dscm_rec_t;                         /* 640 bytes; record.c aligns it to 32 */

typedef char dscm_rec_size_check[sizeof(dscm_rec_t) == 640 ? 1 : -1];

/* The header sector after its dscm_recslot_t is the module's: DSCM_REC_NOTES
 * lines of text about the run (the Wi-Fi link's state, record.c dscm_rec_note),
 * for whoever reads the SD card after a run the phone stopped hearing. The
 * ARM7 clears them with the rest at a run's start and never writes them after;
 * they sit in cache lines of their own (dscm_rec_t offset 128 + 96 = 224). */
#define DSCM_REC_NOTE_OFFSET 96u
#define DSCM_REC_NOTE_BYTES  192u
#define DSCM_REC_NOTES       2u

#define DSCM_REC_SD_OFF     0         /* the ARM7 hasn't taken the ring up: no file (none made, fragmented, not a
                                         stream session), or not yet */
#define DSCM_REC_SD_ON      2         /* mirroring */
#define DSCM_REC_SD_FLIGHT  3         /* mirroring, a sector write in flight */

/* ---- the file ---- */

#define DSCM_REC_FILE         "sd:/_nds/dscheevos/recording.bin"
#define DSCM_RECFILE_MAGIC    0x46525344u  /* "DSRF" */
#define DSCM_RECFILE_VERSION  1u
#define DSCM_RECSLOT_MAGIC    0x53525344u  /* "DSRS" */
#define DSCM_REC_SECTOR_BYTES 512u
#define DSCM_REC_SLOTS        2u
#define DSCM_REC_DATA_SECTORS 512u         /* 256 KB a slot: at least what the ring holds */
#define DSCM_REC_SLOT_SECTORS (1u + DSCM_REC_DATA_SECTORS)
#define DSCM_REC_FILE_SECTORS (1u + DSCM_REC_SLOTS * DSCM_REC_SLOT_SECTORS)
#define DSCM_REC_FILE_SIZE    (DSCM_REC_FILE_SECTORS * DSCM_REC_SECTOR_BYTES)
#define DSCM_REC_SLOT_SECTOR(s) (1u + (uint32_t)(s) * DSCM_REC_SLOT_SECTORS)   /* slot s's header sector in the file */

/* sector 0 */
typedef struct dscm_recfile_t {
  uint32_t magic;                     /* DSCM_RECFILE_MAGIC */
  uint32_t version;                   /* DSCM_RECFILE_VERSION */
  uint32_t data_sectors;              /* DSCM_REC_DATA_SECTORS */
  uint32_t slots;                     /* DSCM_REC_SLOTS */
  /* the game the launcher is starting: the claimed slot's header takes them */
  char     game_code[4];
  char     title[12];
  char     game_hash[32];
} dscm_recfile_t;                     /* 64 bytes */

/* dscm_recslot_t.flags */
#define DSCM_RECSLOT_CLAIMED 0x01     /* the bootloader claimed it for a run (set in every header since) */
#define DSCM_RECSLOT_FLUSHED 0x02     /* the last header was written at a safe point: the game ended normally */

/* a slot's header sector */
typedef struct dscm_recslot_t {
  uint32_t magic;                     /* DSCM_RECSLOT_MAGIC */
  uint32_t size;                      /* sizeof(dscm_recslot_t) */
  uint32_t seq;                       /* claim order: a run takes the highest seq + 1 */
  uint16_t run;                       /* HELLO's run; 0 until the module published its ring */
  uint8_t  replayed;                  /* 1: the phone evaluated it to its end (the launcher sets it) */
  uint8_t  flags;                     /* DSCM_RECSLOT_* */
  char     game_code[4];
  char     title[12];
  char     game_hash[32];             /* lowercase hex, no terminator */
  uint32_t data_sectors;              /* DSCM_REC_DATA_SECTORS */
  uint32_t written;                   /* stream bytes in the data sectors (whole sectors) */
  uint32_t valid_from;                /* continuous from this stream offset to written: before it the ring overtook
                                         the writer (or the slot wrapped) */
  uint32_t segment;                   /* stream offset of the latest segment the module reported */
  uint32_t end_frame;                 /* the frame of the last record the module reported */
  uint32_t skipped;                   /* sectors lost to the ring overtaking the writer */
  uint32_t errors;                    /* SD write errors seen */
  uint32_t reserved;
} dscm_recslot_t;                     /* 96 bytes */

typedef char dscm_recslot_size_check[sizeof(dscm_recslot_t) == 96 ? 1 : -1];

static inline int dscm_recslot_valid(const dscm_recslot_t* s)
{
  return s->magic == DSCM_RECSLOT_MAGIC && s->size == sizeof(*s) && s->data_sectors == DSCM_REC_DATA_SECTORS;
}

/* A recording the phone hasn't replayed: a run that got as far as a sector. */
static inline int dscm_recslot_holds(const dscm_recslot_t* s)
{
  return dscm_recslot_valid(s) && s->run != 0 && s->written != 0 && !s->replayed;
}

/* The slot a new run takes: one that holds no recording, else the older. */
static inline int dscm_rec_pick_slot(const dscm_recslot_t* s0, const dscm_recslot_t* s1)
{
  const uint32_t k0 = dscm_recslot_holds(s0) ? s0->seq : 0;
  const uint32_t k1 = dscm_recslot_holds(s1) ? s1->seq : 0;
  if (k0 != k1)
    return k0 < k1 ? 0 : 1;            /* one holds a recording: the other, or the older recording */
  {
    const uint32_t q0 = dscm_recslot_valid(s0) ? s0->seq : 0;
    const uint32_t q1 = dscm_recslot_valid(s1) ? s1->seq : 0;
    return q0 <= q1 ? 0 : 1;           /* neither holds one: the older claim */
  }
}

/* The slot the launcher offers: the newest recording not replayed, -1 if none. */
static inline int dscm_rec_offer_slot(const dscm_recslot_t* s0, const dscm_recslot_t* s1)
{
  const int h0 = dscm_recslot_holds(s0), h1 = dscm_recslot_holds(s1);
  if (h0 && h1)
    return s0->seq >= s1->seq ? 0 : 1;
  return h0 ? 0 : h1 ? 1 : -1;
}

static inline uint32_t dscm_rec_next_seq(const dscm_recslot_t* s0, const dscm_recslot_t* s1)
{
  const uint32_t q0 = dscm_recslot_valid(s0) ? s0->seq : 0;
  const uint32_t q1 = dscm_recslot_valid(s1) ? s1->seq : 0;
  return (q0 > q1 ? q0 : q1) + 1;
}

#endif
