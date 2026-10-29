/* Cheevotron: the recording of the last runs on the SD card
 * (sd:/_nds/dscheevos/recording.bin, dscm_record.h). During play,
 * nds-bootstrap's ARM7 mirrors the module's recording of the watched words
 * into one of the file's two slots. Before a stream session the file is made
 * ready (its full size, the game named in its first sector); at the start, a
 * run the phone hasn't replayed is offered to it in the launcher's HELLO and
 * served over REPLAY_REQ / REPLAY_DATA / REPLAY_DONE (phone.c), with the
 * packet formats of ds_stream_protocol.h. */
#ifndef DSC_RECORDING_H
#define DSC_RECORDING_H

#include "dscm_record.h"
#include "ds_stream_protocol.h"

#include <stdint.h>

typedef struct recording_t {
  int slot;                 /* 0 or 1 */
  dscm_recslot_t h;         /* its header as read */
  uint8_t* data;            /* the slot's data sectors once loaded (recording_load), else NULL */
  uint32_t lo;              /* stream bytes [lo, h.written) are in data */
  /* the segments found in [lo, h.written): stream offset and base frame */
  uint32_t seg_offset[1024];
  uint32_t seg_frame[1024];
  unsigned segments;
  uint32_t stream;          /* the replay in progress: its first segment's stream offset; 0 none */
  int done;                 /* the phone evaluated it to the end: marked replayed */
} recording_t;

/* Before booting a stream session: the file exists at its full size and its
 * first sector names the game. Returns 0 when it couldn't be written. */
int recording_prepare(const char* rom_path, const char* hash);

/* A recording the phone hasn't replayed, the newest; 0 if none. */
int recording_offer(recording_t* out);

/* Reads the slot's data and finds its segments; 0 if that fails. Done by
 * recording_reply on the first request. */
int recording_load(recording_t* r);
void recording_free(recording_t* r);

/* Answers a REPLAY_REQ: fills the REPLAY_DATA header and up to
 * DSS_REPLAY_CHUNK bytes of data; returns the data's length (0 with
 * DSS_REPLAY_NONE for a run this isn't). */
int recording_reply(recording_t* r, const dss_replay_req_t* req, dss_replay_data_t* out, uint8_t* data);

/* Takes a REPLAY_DONE: 1 when it reaches the end of the recording (the slot
 * is marked replayed), 0 when more is to be sent (recording_reply with a
 * request for `stream` from `offset`), -1 when it is for another run. */
int recording_done(recording_t* r, const dss_replay_done_t* done);

/* The pieces, also for the host test: the segment to replay from (the
 * latest with a base frame before from_frame; 0: the oldest), (uint32_t)-1
 * if there is none, *gap set when frames the phone wants are gone; and up to
 * len stream bytes from abs_offset. */
uint32_t recording_find(const recording_t* r, uint32_t from_frame, int* gap);
uint32_t recording_read(const recording_t* r, uint32_t abs_offset, uint8_t* out, uint32_t len);
/* Finds the segments in a loaded slot (recording_load does it). */
void recording_scan(recording_t* r);

#endif
