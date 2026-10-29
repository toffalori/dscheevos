/* Cheevotron: before the game starts, find the phone that will take its RAM
 * stream, so the player knows whether achievements will count. */
#ifndef DSC_PHONE_H
#define DSC_PHONE_H

#include <stdint.h>

struct recording_t;

typedef struct phone_t {
  uint32_t ip;      /* network byte order */
  int state;        /* DSS_READY_* */
  char name[29];
} phone_t;

/* Says HELLO (from the launcher) to the network and to known_ip (0: none)
 * for up to timeout_frames (-1: no limit), answers the phone's PROBEs, and
 * waits for its READY. each_frame runs once a frame; a nonzero return stops
 * the search. Returns 0 with *out filled when a phone answered, -1 otherwise.
 * With rec (a recording of an earlier run the phone hasn't replayed,
 * recording.h), the HELLO offers it and the phone's REPLAY_REQs are answered
 * from the file: after READY the search goes on while the phone asks for
 * pieces (a short grace for the first), until it reports the end. */
int phone_find(const char* rom_path, const char* hash, uint32_t known_ip, int timeout_frames,
               int (*each_frame)(void), phone_t* out, struct recording_t* rec);

/* "a.b.c.d" for a network-byte-order address, and back (0 when not one). */
void phone_ip_text(uint32_t ip, char out[16]);
uint32_t phone_ip_parse(const char* text);

/* The phone found last time (sd:/_nds/dscheevos/phone.txt), 0 if none; and
 * remembering a new one (written only when it changed). */
uint32_t phone_remembered(void);
void phone_remember(uint32_t ip);

#endif
