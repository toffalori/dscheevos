/*
 * DScheevos launcher. TWiLight Menu++ starts it in place of nds-bootstrap
 * (it is installed as _nds/nds-bootstrap-release.nds). It reads the game
 * from nds-bootstrap.ini, goes online to start a RetroAchievements session,
 * writes the session file, then boots the DScheevos nds-bootstrap, which
 * reads the same ini and loads the module with the session.
 */
#include "apfix.h"
#include "chainload.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "pending.h"
#include "phone.h"
#include "session.h"
#include "ui.h"
#include "ds_stream_protocol.h"

#include <dswifi9.h>
#include <fat.h>
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BOOTSTRAP_PATH DSC_DIR "nds-bootstrap.nds"

static dsc_settings_t settings;
static char rom_path[256];
static int wifi_started;   /* net_connect ran: DSWiFi is up and must be stopped */
static int saved_hardcore = -1;   /* hardcore turned off for this game only (AP-fix) */

/* Saves the settings, but not a hardcore setting changed for this game only. */
static void save_settings(void)
{
  dsc_settings_t s = settings;
  if (saved_hardcore >= 0)
    s.hardcore = saved_hardcore;
  settings_save(&s);
}

static const char* basename_of(const char* path)
{
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

/* With achievements, the game keeps the Wi-Fi connection to send unlocks:
 * the ARM7 stops DSWiFi without telling the chip, which stays associated. */
static void hand_over_wifi(void)
{
  int i;
  fifoSendValue32(FIFO_USER_01, 0x46524545u);           /* "FREE" */
  for (i = 0; i < 60; i++) {
    if (fifoCheckValue32(FIFO_USER_01) && fifoGetValue32(FIFO_USER_01) == 0x444F4E45u)   /* "DONE" */
      return;
    swiWaitForVBlank();
  }
}

#define TWLCFG     ((u8*)0x02000400)    /* DSi settings, 0x128 bytes */
#define HWINFO_N   ((u8*)0x02000600)    /* HWINFO_N.dat, 0x14 bytes */

/* The test nds-bootstrap's first stage uses (conf_sd.cpp, useTwlCfg). */
static int twlcfg_present(void)
{
  return TWLCFG[0] && !TWLCFG[1] && !TWLCFG[2] && !TWLCFG[4] && TWLCFG[0x48];
}

static int read_at(const char* path, long offset, void* dst, size_t size)
{
  FILE* f = fopen(path, "rb");
  int ok = f && fseek(f, offset, SEEK_SET) == 0 && fread(dst, 1, size, f) == size;
  if (f)
    fclose(f);
  return ok;
}

/* nds-bootstrap's first stage takes the DSi settings (TWLCFG) from
 * 0x02000400. When they aren't there it mounts the NAND to read them, and
 * that waits for its ARM7 to read the eMMC CID, which that ARM7 only does
 * later: started straight from the DSi Menu, both screens stay white for
 * ever. TWiLight Menu++ puts them there before starting us; started from the
 * menu, our binary's first 2 KB (zeros) cover them. Fill them in from the
 * SD-NAND copy (hiyaCFW) as nds-bootstrap would, Wi-Fi firmware fields and
 * HWINFO_N included. */
static void restore_twlcfg(void)
{
  u8 wl_type = 0;
  if (!isDSiMode() || twlcfg_present())
    return;
  if (!read_at("sd:/shared1/TWLCFG0.dat", 0x88, TWLCFG, 0x128)) {
    log_line("twlcfg: missing, and no sd:/shared1/TWLCFG0.dat");
    return;
  }
  readFirmware(0x1FD, &wl_type, 1);      /* WlFirm type (1=DWM-W015, 2=W024, 3=W028) */
  TWLCFG[0x1E0] = wl_type;
  *(u32*)(TWLCFG + 0x1E4) = (wl_type == 2 || wl_type == 3) ? 0x520000 : 0x500400;   /* RAM vars */
  *(u32*)(TWLCFG + 0x1E8) = (wl_type == 2 || wl_type == 3) ? 0x520000 : 0x500000;   /* RAM base */
  *(u32*)(TWLCFG + 0x1EC) = (wl_type == 2 || wl_type == 3) ? 0x020000 : 0x02E000;   /* RAM size */
  if (!read_at("sd:/sys/HWINFO_N.dat", 0x88, HWINFO_N, 0x14))
    log_line("twlcfg: no sd:/sys/HWINFO_N.dat");
  DC_FlushRange(TWLCFG, 0x214);
  log_line("twlcfg: restored from SD-NAND (%s), wlfirm %d", twlcfg_present() ? "valid" : "INVALID", wl_type);
}

/* Hands over to nds-bootstrap; only returns on failure. */
static void boot_game(int with_achievements)
{
  int keep_wifi = with_achievements && settings_keep_wifi(&settings);
  const char* argv[1] = { BOOTSTRAP_PATH };
  if (!with_achievements)
    remove(DSC_DIR "session.bin");
  ui_status(with_achievements ? "Starting with achievements..." : "Starting without achievements...");
  if (keep_wifi)
    hand_over_wifi();
  else if (wifi_started)
    Wifi_DisableWifi();   /* never started otherwise: DSWiFi's IPC isn't set up, it data-aborts */
  restore_twlcfg();
  log_line("chainload");
  int err = chainload(BOOTSTRAP_PATH, 1, argv);
  log_line("chainload failed: %d", err);
  ui_status("\x1b[31;1mCouldn't start nds-bootstrap (%d)\x1b[39;0m", err);
  ui_status("%s", err == CHAINLOAD_ERR_LOADER ? CHAINLOAD_LOADER_PATH : BOOTSTRAP_PATH);
  for (;;) swiWaitForVBlank();
}

#define HASH_CACHE DSC_DIR "hashes.txt"

/* The game's RetroAchievements hash, remembered by file, size and time
 * ("size time hash path" lines): hashing reads the header, both binaries and
 * more of the ROM from the SD card, at every start. */
static int hash_rom(const char* path, char hash[33])
{
  struct stat st;
  char line[320], cached[33], cached_path[260];
  unsigned long size, mtime;
  FILE* f;
  if (stat(path, &st) != 0)
    return session_hash_rom(path, hash);
  f = fopen(HASH_CACHE, "r");
  if (f) {
    while (fgets(line, sizeof(line), f)) {
      if (sscanf(line, "%lu %lu %32s %259[^\n]", &size, &mtime, cached, cached_path) == 4 && strlen(cached) == 32
          && size == (unsigned long)st.st_size && mtime == (unsigned long)st.st_mtime && !strcmp(cached_path, path)) {
        fclose(f);
        memcpy(hash, cached, 33);
        log_line("hash: remembered");
        return 1;
      }
    }
    fclose(f);
  }
  if (!session_hash_rom(path, hash))
    return 0;
  f = fopen(HASH_CACHE, "a");
  if (f) {
    fprintf(f, "%lu %lu %s %s\n", (unsigned long)st.st_size, (unsigned long)st.st_mtime, hash, path);
    fclose(f);
  }
  log_line("hash: computed");
  return 1;
}

static int game_is_dsi_mode(int ini_dsi_mode)
{
  unsigned char unit = 0;
  FILE* f = fopen(rom_path, "rb");
  if (f) {
    fseek(f, 0x12, SEEK_SET);
    if (fread(&unit, 1, 1, f) != 1) unit = 0;
    fclose(f);
  }
  return (unit & 2) && ini_dsi_mode;
}

static int log_in(void)
{
  char password[64];
  const char* error;
  for (;;) {
    int r;
    if (ui_choose("Log in to RetroAchievements to\n earn achievements.", "Log in", "Play without achievements") == 'B')
      return 0;
    if (!ui_input("Username", settings.username, sizeof(settings.username), 0))
      continue;
    password[0] = '\0';
    if (!ui_input("Password", password, sizeof(password), 1))
      continue;
    ui_status("Logging in as %s...", settings.username);
    wifi_started = 1;
    if (net_connect(60 * 20, ui_tick) != 0) {
      ui_status("\x1b[31;1m%s\x1b[39;0m", net_error());
      continue;
    }
    r = session_login(&settings, password, &error);
    memset(password, 0, sizeof(password));
    if (r == SESSION_OK) {
      save_settings();
      ui_status("Logged in.");
      return 1;
    }
    log_line("login: %s", error);
    ui_status("\x1b[31;1m%s\x1b[39;0m", error);
  }
}

/* Unlocks the last session couldn't send, now that we're online. Returns
 * how many were for this game: the session must be started again so its
 * cached unlock list includes them. */
static unsigned send_pending(const char* hash)
{
  pending_result_t p;
  pending_send(&settings, hash, &p);
  if (p.sent_unlocks)
    ui_status("Sent %u unlock%s from last time", p.sent_unlocks, p.sent_unlocks == 1 ? "" : "s");
  if (p.sent_entries)
    ui_status("Sent %u leaderboard entr%s", p.sent_entries, p.sent_entries == 1 ? "y" : "ies");
  if (p.failed)
    ui_status("\x1b[33;1m%u couldn't be sent; will retry\n next time\x1b[39;0m", p.failed);
  return p.this_game;
}

/* Hardcore uses only nds-bootstrap's built-in AP-fix patches: nds-bootstrap
 * ignores user files in _nds/nds-bootstrap/apFix/ then. If only a user file
 * would get this game past its anti-piracy checks, hardcore would boot it
 * unpatched, so it isn't offered: Casual (with the user file) or no
 * achievements. */
static void check_apfix(void)
{
  char name[64];
  int r = apfix_check(rom_path, BOOTSTRAP_PATH, name, sizeof(name));
  if (r == APFIX_NONE)
    return;
  if (r == APFIX_BUILTIN) {
    log_line("apfix: hardcore, built-in fix used, apFix/%s ignored", name);
    ui_status("Hardcore uses the built-in AP\n fix, not apFix/%.15s", name);
    return;
  }
  log_line("apfix: apFix/%s and no built-in fix: no hardcore", name);
  ui_status("\x1b[33;1mAP fix: apFix/%.16s\x1b[39;0m", name);
  if (ui_choose("This game has its own AP-fix\n file in nds-bootstrap/apFix/\n and no built-in fix. Hardcore\n can't use it: remove it to play\n in Hardcore.",
                "Play in Casual", "Play without achievements") == 'B')
    boot_game(0);
  if (saved_hardcore < 0)
    saved_hardcore = settings.hardcore;
  settings.hardcore = 0;
}

/* nds-bootstrap doesn't run a donor ROM's ARM7 binary in hardcore: any ROM
 * can be set as the donor. The build of the game that needs one isn't
 * offered hardcore while one is set. */
static void check_donor(void)
{
  char donor[256];
  if (!settings.hardcore || !donor20_needed(rom_path) || !bootstrap_ini_donor20(donor, sizeof(donor))
   || access(donor, F_OK) != 0)
    return;
  log_line("donor: this game uses a donor ROM: no hardcore");
  if (ui_choose("This game runs with a donor\n ROM's ARM7 code (set in\n TWiLight Menu++). Hardcore\n can't use it: unset it to play\n in Hardcore.",
                "Play in Casual", "Play without achievements") == 'B')
    boot_game(0);
  if (saved_hardcore < 0)
    saved_hardcore = settings.hardcore;
  settings.hardcore = 0;
}

static void settings_menu(void)
{
  for (;;) {
    int k;
    ui_clear_bottom();
    ui_bottom("\n Settings\n\n");
    ui_bottom(" \x1b[32;1mA\x1b[39;0m  Achievements: %s\n", settings.enabled ? "on" : "off");
    ui_bottom(" \x1b[32;1mX\x1b[39;0m  Hardcore: %s\n", settings.hardcore ? "on" : "off (Casual)");
    ui_bottom(" \x1b[32;1mR\x1b[39;0m  Send unlocks during play: %s\n",
              settings.hardcore ? "on (hardcore)" : settings.live ? "on" : "off");
    ui_bottom(" \x1b[32;1mL\x1b[39;0m  Skip unofficial achievements:\n     %s\n", settings.skip_unofficial ? "on" : "off");
    ui_bottom(" \x1b[32;1mY\x1b[39;0m  Log out%s%s\n", settings.username[0] ? " " : "", settings.username);
    ui_bottom("\n \x1b[31;1mB\x1b[39;0m  Done\n");
    do {
      swiWaitForVBlank();
      scanKeys();
      k = keysDown();
    } while (!(k & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_R | KEY_L)));
    if (k & KEY_A) settings.enabled = !settings.enabled;
    if (k & KEY_X) settings.hardcore = !settings.hardcore;
    if ((k & KEY_R) && !settings.hardcore) settings.live = !settings.live;
    if (k & KEY_L) settings.skip_unofficial = !settings.skip_unofficial;
    if (k & KEY_Y) { settings.token[0] = '\0'; settings.username[0] = '\0'; }
    if (k & KEY_B) { settings_save(&settings); return; }
  }
}

static int tick(void)
{
  ui_tick();
  return 0;
}

static int tick_until_b(void)
{
  ui_tick();
  scanKeys();
  return (keysDown() & KEY_B) != 0;
}

/* Cheevotron: the phone runs the achievements, so find it before the game
 * starts and say so: the player always knows whether this game counts. Keeps
 * looking until the phone answers or the player plays without it. */
static void find_phone(const char* hash)
{
  uint32_t known = phone_remembered();
  phone_t phone;
  settings.phone_ip = known;   /* playing without an answer, the game still tries it */
  ui_status("Looking for your phone...");
  for (;;) {
    int found = phone_find(rom_path, hash, known, 60 * 3, tick, &phone) == 0;
    if (!found) {
      ui_status("\x1b[33;1mPhone not found\x1b[39;0m");
      ui_clear_bottom();
      ui_bottom("\n Open Cheevotron on your phone.\n The phone and the DSi need the\n same Wi-Fi network.\n\n Still looking...\n\n");
      ui_bottom(" \x1b[31;1mB\x1b[39;0m  Play anyway (the phone can\n    join during the game)\n");
      found = phone_find(rom_path, hash, known, -1, tick_until_b, &phone) == 0;
      ui_clear_bottom();
      if (!found) {
        log_line("phone: playing without it");
        return;
      }
    }
    settings.phone_ip = known = phone.ip;
    phone_remember(phone.ip);
    if (phone.state == DSS_READY_LOGGED_OUT) {
      ui_status("\x1b[33;1mPhone found, not logged in\x1b[39;0m");
      if (ui_choose("Log in to RetroAchievements in\n the Cheevotron app.", "Try again", "Play anyway") == 'B') {
        ui_clear_bottom();
        return;
      }
      ui_clear_bottom();
      ui_status("Looking for your phone...");
      continue;
    }
    ui_status("\x1b[32;1mPhone ready%s%.18s\x1b[39;0m", phone.name[0] ? ": " : "", phone.name);
    {
      int i;
      for (i = 0; i < 30; i++) {   /* long enough to read */
        ui_tick();
        cothread_yield_irq(IRQ_VBLANK);
      }
    }
    return;
  }
}

/* How we were started: the DSi Menu (hiyaCFW icon) and TWiLight Menu++ leave
 * different SCFG states. The ARM7 reports its own (arm7/source/main.c). */
static void log_scfg(void)
{
  u32 r[5];
  int n = 0, t;
  if (!isDSiMode())
    return;
  log_line("scfg9: ext=%08lX a9rom=%02X clk=%04X", (unsigned long)REG_SCFG_EXT, *(vu8*)0x04004000, REG_SCFG_CLK);
  for (t = 0; t < 60 && n < 5; t++) {
    while (n < 5 && fifoCheckValue32(FIFO_USER_02))
      r[n++] = fifoGetValue32(FIFO_USER_02);
    if (n < 5)
      swiWaitForVBlank();
  }
  if (n < 5) {
    log_line("scfg7: no report from the ARM7 (%d)", n);
    return;
  }
  log_line("scfg7: rom=%04lX clk=%04lX ext=%08lX mc=%04lX delays=%08lX",
           (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2], (unsigned long)r[3], (unsigned long)r[4]);
}

int main(int argc, char** argv)
{
  int dsi_mode;
  char hash[33];
  (void)argc; (void)argv;

  ui_init();
  ui_splash(NULL);   /* the Cheevotron screen from the first frame; the game's name joins it */
  if (!fatInitDefault()) {
    ui_status("\x1b[31;1mCan't read the SD card\x1b[39;0m");
    for (;;) swiWaitForVBlank();
  }
  log_start();
  log_scfg();
  if (isDSiMode())
    log_line("twlcfg at start: %s (%02X %02X 448=%02X), cid@2FFD7BC=%08lX",
             twlcfg_present() ? "present" : "missing", TWLCFG[0], TWLCFG[1], TWLCFG[0x48],
             (unsigned long)*(vu32*)0x02FFD7BC);
  settings_load(&settings);
  if (settings.cart) settings.stream = 1;   /* a cart session is a stream session */
  if (!bootstrap_ini_game(rom_path, sizeof(rom_path), &dsi_mode)) {
    ui_header("", NULL);
    ui_status("No game in _nds/nds-bootstrap.ini.");
    ui_status("Start games from TWiLight Menu++.");
    for (;;) swiWaitForVBlank();
  }
  /* Cheevotron keeps the splash; on-console achievements list the session below a header */
  if (settings.stream)
    ui_splash(basename_of(rom_path));
  else
    ui_header(basename_of(rom_path), NULL);

  /* hold SELECT while the game starts for settings */
  scanKeys();
  if (keysHeld() & KEY_SELECT)
    settings_menu();

  if (!settings.enabled)
    boot_game(0);
  if (game_is_dsi_mode(dsi_mode)) {
    ui_wait("Achievements need DS mode. In\n TWiLight's game settings, set\n \"Run in\" to DS mode.", 240);
    boot_game(0);
  }

  ui_status("Checking the game...");
  if (!hash_rom(rom_path, hash)) {
    ui_wait("Couldn't read the game file.", 120);
    boot_game(0);
  }
  if (settings.hardcore)
    check_apfix();
  check_donor();

  if (settings.stream) {
    /* Cheevotron: no RetroAchievements on the console. Bring up the Wi-Fi the
     * game keeps for the phone, write a session with only the Wi-Fi config and
     * the game hash (no login, no set download, no placard), and boot. The
     * phone runs rcheevos over the streamed RAM. The splash is up already. */
    for (;;) {
      ui_status("Connecting to Wi-Fi...");
      wifi_started = 1;
      log_line("wifi: connecting");
      if (net_connect(60 * 20, ui_tick) != 0) {
        log_line("stream wifi: %s", net_error());
        ui_status("\x1b[31;1m%s\x1b[39;0m", net_error());
        if (ui_choose("Cheevotron needs Wi-Fi to reach\n the phone.", "Try again", "Play without achievements") == 'B')
          boot_game(0);
        continue;
      }
      log_line("wifi: connected");
      find_phone(hash);
      if (!session_write(&settings, rom_path, hash)) {
        log_line("stream: couldn't write the session file");
        if (ui_choose("Couldn't write the session file.", "Try again", "Play without achievements") == 'B')
          boot_game(0);
        continue;
      }
      boot_game(1);
    }
  }

  for (;;) {
    session_info_t info;
    const char* error;
    int r;

    if (!settings.token[0] && !log_in())
      boot_game(0);

    ui_status("Connecting to Wi-Fi...");
    wifi_started = 1;
    if (net_connect(60 * 20, ui_tick) != 0) {
      log_line("wifi: %s", net_error());
      ui_status("\x1b[31;1m%s\x1b[39;0m", net_error());
      if (ui_choose("Achievements need the Internet\n when the game starts.", "Try again", "Play without achievements") == 'B')
        boot_game(0);
      continue;
    }

    ui_status("Loading the achievements...");
    {
      bench_result_t bench;
      r = session_prepare(&settings, hash, &bench, &error);
    }
    if (r == SESSION_OK) {
      ui_status("Starting the session...");
      r = session_start(&settings, rom_path, hash, &info, &error);
    }
    if ((r == SESSION_OK || r == SESSION_NO_SET) && send_pending(hash) && r == SESSION_OK)
      r = session_start(&settings, rom_path, hash, &info, &error);
    if (r == SESSION_OK) {
      char line[40];
      snprintf(line, sizeof(line), "%u/%u achievements, %u/%u pts", info.unlocked, info.achievements,
               info.points_unlocked, info.points_total);
      ui_header(info.game_title, line);
      ui_status("%s%s (%u pts)%s", settings.hardcore ? "\x1b[31;1mHARDCORE\x1b[39;0m  " : "Casual  ",
                info.display_name, info.score,
                !settings.hardcore ? "" : info.hardcore_refused ? "\n Rules on, RA counts as Casual" : "\n Not yet confirmed by RA");
      if (settings.spectator)
        ui_status("\x1b[33;1mSpectator: nothing is sent\x1b[39;0m");
      log_line("session: started%s", settings.spectator ? ", spectator" : "");
      if (!pending_prepare(&settings) || !session_write(&settings, rom_path, hash)) {
        if (session_too_big()) {
          ui_wait("This game's achievement set is\n too big for DScheevos yet.\n\n Playing without achievements.", 240);
          boot_game(0);
        }
        log_line("session: couldn't write the session files");
        if (ui_choose("Couldn't write the session file.", "Try again", "Play without achievements") == 'B')
          boot_game(0);
        continue;
      }
      if (ui_wait("Have fun!\n\n B  Play without achievements", 90) & KEY_B)
        boot_game(0);
      boot_game(1);
    }
    if (r == SESSION_TOO_BIG) {
      ui_wait("This game's achievement set is\n too big for DScheevos yet.\n\n Playing without achievements.", 240);
      boot_game(0);
    }
    if (r == SESSION_NO_SET) {
      log_line("session: no set for this game");
      ui_wait("RetroAchievements doesn't know\n this version of the game.", 150);
      boot_game(0);
    }
    log_line("session: %s", error);
    if (r == SESSION_LOGIN_FAILED) {
      ui_status("\x1b[31;1m%s\x1b[39;0m", error);
      settings.token[0] = '\0';
      save_settings();
      continue;
    }
    ui_status("\x1b[31;1m%s\x1b[39;0m", error);
    if (ui_choose("Couldn't start the session.", "Try again", "Play without achievements") == 'B')
      boot_game(0);
  }
}
