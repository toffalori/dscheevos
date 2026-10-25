/* DScheevos settings (sd:/_nds/dscheevos/dscheevos.ini) and the game to boot
 * (sd:/_nds/nds-bootstrap.ini, written by TWiLight Menu++). */
#ifndef DSC_CONFIG_H
#define DSC_CONFIG_H

#define DSC_DIR "sd:/_nds/dscheevos/"

typedef struct dsc_settings_t {
  int enabled;          /* 0: boot games without achievements */
  int hardcore;
  int live;             /* send unlocks during play over the session's Wi-Fi connection */
  int spectator;        /* SPECTATOR = 1 (ini only): play and show, send nothing */
  int skip_unofficial;  /* SKIP_UNOFFICIAL = 1: ask the server for the promoted achievements only (f=3) */
  int stream;           /* STREAM = 1 (Cheevotron): no on-console RA; stream RAM to the phone, which evaluates */
  char username[32];
  char token[64];       /* from the server at login; the password is never stored */
  char host[64];
} dsc_settings_t;

/* The game keeps the launcher's Wi-Fi during play for hardcore, live unlocks,
 * or the Cheevotron stream (the phone needs the connection). */
#define settings_keep_wifi(s) ((s)->stream || (s)->live || (s)->hardcore)

void settings_load(dsc_settings_t* s);
int settings_save(const dsc_settings_t* s);

/* NDS_PATH and DSI_MODE from nds-bootstrap.ini; returns 0 if NDS_PATH is missing. */
int bootstrap_ini_game(char* path, int path_size, int* dsi_mode);
/* DONOR20_NDS_PATH: the ROM whose ARM7 binary nds-bootstrap runs for one
 * build of Yoshi's Island DS. Returns 0 if it isn't set. */
int bootstrap_ini_donor20(char* path, int path_size);

#endif
