#include "session.h"
#include "bench.h"
#include "log.h"
#include "net.h"
#include "request.h"
#include "dscheevos_module.h"

#include "mbedtls/x509_crt.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

#include <dswifi9.h>
#include <nds.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/* replies kept for the session file, in DSCM_PRELOAD_* order */
static char* g_reply[DSCM_PRELOAD_COUNT];
static size_t g_reply_len[DSCM_PRELOAD_COUNT];

static int g_done, g_result;
/* session_prepare() fetched the login and set replies: session_start()
 * answers those two requests from them, so only startsession goes out */
static int g_replay;
static char g_error[128];
/* the set reply was bigger than a session file can be: the module could never
 * load it, so it isn't parsed here either (a 2.4 MB set crashed the launcher
 * in rcheevos' parse; Animal Crossing: Wild World) */
static size_t g_too_big;

/* Logs the request's API name and the answer (never the parameters: they hold the token). */
void session_log_request(const char* post, int status)
{
  char api[24];
  size_t n = 0;
  if (post && !strncmp(post, "r=", 2))
    for (post += 2; post[n] && post[n] != '&' && n < sizeof(api) - 1; n++)
      api[n] = post[n];
  api[n] = '\0';
  if (status > 0)
    log_line("%s: HTTP %d", n ? api : "request", status);
  else
    log_line("%s: network error: %s", n ? api : "request", net_error());
}

static int g_skip_unofficial;   /* the setting, for server_call */

static int request_kind(const char* post)
{
  if (!post) return -1;
  if (!strncmp(post, "r=login2&", 9)) return DSCM_PRELOAD_LOGIN;
  if (!strncmp(post, "r=achievementsets&", 18)) return DSCM_PRELOAD_SET;
  if (!strncmp(post, "r=startsession&", 15)) return DSCM_PRELOAD_SESSION;
  return -1;
}

/* rcheevos asks for a request; answer it right away (blocking). */
static void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback,
                                 void* callback_data, rc_client_t* client)
{
  http_response_t r;
  rc_api_server_response_t response;
  int kind = request_kind(request->post_data);
  char post[512];
  (void)client;

  if (g_replay && (kind == DSCM_PRELOAD_LOGIN || kind == DSCM_PRELOAD_SET) && g_reply[kind]) {
    memset(&response, 0, sizeof(response));
    response.http_status_code = 200;
    response.body = g_reply[kind];
    response.body_length = g_reply_len[kind];
    callback(&response, callback_data);
    return;
  }
  if (bench_active) {
    /* the benchmark's fake memory must never reach the server */
    memset(&response, 0, sizeof(response));
    response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    response.body = "";
    callback(&response, callback_data);
    return;
  }

  http_request(request->url, request_post(request->post_data, g_skip_unofficial, post, sizeof(post)),
               request->content_type, &r);
  session_log_request(request->post_data, r.status);
  memset(&response, 0, sizeof(response));
  response.http_status_code = r.status > 0 ? r.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
  response.body = r.body ? r.body : "";
  response.body_length = r.length;
  if (r.status < 0)
    snprintf(g_error, sizeof(g_error), "%s", net_error());

  if (kind == DSCM_PRELOAD_SET && r.status == 200 && r.length > DSCM_SESSION_MAX) {
    g_too_big = r.length;
    http_free(&r);
    memset(&response, 0, sizeof(response));
    response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    response.body = "";
    snprintf(g_error, sizeof(g_error), "The achievement set is too big");
    callback(&response, callback_data);
    return;
  }
  if (kind >= 0 && r.status == 200 && r.body) {
    free(g_reply[kind]);
    g_reply[kind] = r.body;          /* keep it; freed when replaced */
    g_reply_len[kind] = r.length;
    r.body = NULL;
    callback(&response, callback_data);
    return;
  }
  callback(&response, callback_data);
  http_free(&r);
}

static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
  int32_t n = bench_read(address, buffer, num_bytes);
  (void)client;
  return n >= 0 ? (uint32_t)n : 0;   /* no game memory here, but the benchmark's */
}

static void RC_CCONV on_done(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  (void)client; (void)userdata;
  g_result = result;
  if (result != RC_OK && error_message && !g_error[0])   /* a network error says more */
    snprintf(g_error, sizeof(g_error), "%s", error_message);
  g_done = 1;
}

static rc_client_t* make_client(const dsc_settings_t* settings)
{
  char ua[128], clause[64];
  rc_client_t* client = rc_client_create(read_memory, server_call);
  g_skip_unofficial = settings->skip_unofficial;
  if (!client)
    return NULL;
  if (strcmp(settings->host, "https://retroachievements.org") != 0)
    rc_client_set_host(client, settings->host);
  rc_client_set_hardcore_enabled(client, settings->hardcore);
  rc_client_set_spectator_mode_enabled(client, settings->spectator);
  rc_client_get_user_agent_clause(client, clause, sizeof(clause));
  snprintf(ua, sizeof(ua), "DScheevos/" DSC_VERSION " (Nintendo DSi) %s", clause);
  http_set_user_agent(ua);
  return client;
}

static int wait_done(void)
{
  /* server_call answers synchronously, so this is already done */
  return g_done ? g_result : RC_INVALID_STATE;
}

int session_login(dsc_settings_t* settings, const char* password, const char** error)
{
  rc_client_t* client = make_client(settings);
  const rc_client_user_t* user;
  int result;
  *error = g_error;
  g_error[0] = '\0';
  if (!client) { strcpy(g_error, "Out of memory"); return SESSION_ERROR; }
  g_done = 0;
  rc_client_begin_login_with_password(client, settings->username, password, on_done, NULL);
  result = wait_done();
  user = rc_client_get_user_info(client);
  if (result == RC_OK && user && user->token) {
    snprintf(settings->token, sizeof(settings->token), "%s", user->token);
    snprintf(settings->username, sizeof(settings->username), "%s", user->username);
  }
  rc_client_destroy(client);
  if (result == RC_OK) return SESSION_OK;
  if (!g_error[0]) strcpy(g_error, "Login failed");
  return (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) ? SESSION_LOGIN_FAILED : SESSION_ERROR;
}

int session_too_big(void)
{
  return g_too_big != 0;
}

int session_hash_rom(const char* rom_path, char hash[33])
{
  rc_hash_iterator_t it;
  int ok;
  rc_hash_initialize_iterator(&it, rom_path, NULL, 0);
  ok = rc_hash_generate(hash, RC_CONSOLE_NINTENDO_DS, &it);
  rc_hash_destroy_iterator(&it);
  return ok;
}

/* RetroAchievements adds a warning achievement (ID 101000001 and up) when it
 * doesn't recognise the client, and files hardcore unlocks as softcore. */
static int hardcore_refused(rc_client_t* client)
{
  rc_client_achievement_list_t* list = rc_client_create_achievement_list(client,
    RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  uint32_t b, i;
  int found = 0;
  if (!list)
    return 0;
  for (b = 0; b < list->num_buckets && !found; b++)
    for (i = 0; i < list->buckets[b].num_achievements; i++)
      if (list->buckets[b].achievements[i]->id >= 101000001u)
        found = 1;
  rc_client_destroy_achievement_list(list);
  return found;
}

static void clear_replies(void)
{
  int i;
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) { free(g_reply[i]); g_reply[i] = NULL; g_reply_len[i] = 0; }
  g_replay = 0;
}

int session_prepare(const dsc_settings_t* settings, const char* hash, bench_result_t* bench, const char** error)
{
  dsc_settings_t spectator = *settings;
  rc_client_t* client;
  int result;
  *error = g_error;
  g_error[0] = '\0';
  memset(bench, 0, sizeof(*bench));
  clear_replies();
  /* spectator from the start: rc_client logs in and fetches the set, but
   * starts no session on the server, whatever the benchmark decides */
  spectator.spectator = 1;
  client = make_client(&spectator);
  if (!client) { strcpy(g_error, "Out of memory"); return SESSION_ERROR; }

  g_done = 0;
  rc_client_begin_login_with_token(client, settings->username, settings->token, on_done, NULL);
  result = wait_done();
  if (result != RC_OK) {
    rc_client_destroy(client);
    clear_replies();
    if (!g_error[0]) strcpy(g_error, "Login failed");
    return (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED)
      ? SESSION_LOGIN_FAILED : SESSION_ERROR;
  }
  g_done = 0;
  g_too_big = 0;
  if (bench_begin() != 0)
    log_line("bench: no memory");
  rc_client_begin_load_game(client, hash, on_done, NULL);
  result = wait_done();
  if (result == RC_OK && !(rc_client_get_game_info(client) && rc_client_get_game_info(client)->id))
    result = RC_NO_GAME_LOADED;
  if (result == RC_OK) {
    if (bench_run(client, 60, 1500, bench) == 0)
      log_line("bench: %u frames, median %u max %u cycles at 67 MHz, %u ms",
               (unsigned)bench->frames, (unsigned)bench->median, (unsigned)bench->max, (unsigned)bench->ms);
  }
  rc_client_destroy(client);
  bench_end();
  if (g_too_big) {
    log_line("session: set reply %u bytes, over the %u-byte limit", (unsigned)g_too_big, (unsigned)DSCM_SESSION_MAX);
    clear_replies();
    return SESSION_TOO_BIG;
  }
  if (result == RC_NO_GAME_LOADED) {
    clear_replies();
    return SESSION_NO_SET;
  }
  if (result != RC_OK) {
    clear_replies();
    if (!g_error[0]) strcpy(g_error, "Couldn't load the game's achievements");
    return SESSION_ERROR;
  }
  g_replay = 1;
  return SESSION_OK;
}

int session_start(const dsc_settings_t* settings, const char* rom_path, const char* hash,
                  session_info_t* info, const char** error)
{
  rc_client_t* client = make_client(settings);
  int result, i;
  (void)rom_path;
  *error = g_error;
  g_error[0] = '\0';
  memset(info, 0, sizeof(*info));
  if (!g_replay)
    clear_replies();
  else {
    free(g_reply[DSCM_PRELOAD_SESSION]);
    g_reply[DSCM_PRELOAD_SESSION] = NULL;
    g_reply_len[DSCM_PRELOAD_SESSION] = 0;
  }
  if (!client) { strcpy(g_error, "Out of memory"); return SESSION_ERROR; }

  g_done = 0;
  rc_client_begin_login_with_token(client, settings->username, settings->token, on_done, NULL);
  result = wait_done();
  if (result != RC_OK) {
    rc_client_destroy(client);
    if (!g_error[0]) strcpy(g_error, "Login failed");
    return (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED)
      ? SESSION_LOGIN_FAILED : SESSION_ERROR;
  }
  {
    const rc_client_user_t* user = rc_client_get_user_info(client);
    if (user) {
      snprintf(info->display_name, sizeof(info->display_name), "%s", user->display_name);
      info->score = settings->hardcore ? user->score : user->score_casual;
    }
  }

  g_done = 0;
  rc_client_begin_load_game(client, hash, on_done, NULL);
  result = wait_done();
  if (result == RC_OK) {
    const rc_client_game_t* game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary;
    rc_client_get_user_game_summary(client, &summary);
    snprintf(info->game_title, sizeof(info->game_title), "%s", game ? game->title : "");
    info->game_id = game ? game->id : 0;
    info->achievements = summary.num_promoted_achievements;
    info->unlocked = summary.num_unlocked_achievements;
    info->points_total = summary.points_available;
    info->points_unlocked = summary.points_unlocked;
    info->hardcore_refused = settings->hardcore && hardcore_refused(client);
  }
  rc_client_destroy(client);
  g_replay = 0;   /* the next start fetches everything again */

  if (result == RC_NO_GAME_LOADED || (result == RC_OK && info->game_id == 0))
    return SESSION_NO_SET;
  if (result != RC_OK) {
    if (!g_error[0]) strcpy(g_error, "Couldn't load the game's achievements");
    return SESSION_ERROR;
  }
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) {
    /* a spectator starts no session on the server */
    if (!g_reply[i] && !(settings->spectator && i == DSCM_PRELOAD_SESSION)) {
      strcpy(g_error, "Server reply missing");
      return SESSION_ERROR;
    }
  }
  return SESSION_OK;
}

int session_write(const dsc_settings_t* settings, const char* rom_path, const char* hash)
{
  dscm_boot_t boot;
  uint32_t offset = (sizeof(boot) + 3) & ~3u;
  static const char pad[4];
  static char ca_pem[8192];
  static unsigned char ca_der[8192];
  size_t ca_len = 0;
  FILE* f;
  FILE* ca;
  int i;

  memset(&boot, 0, sizeof(boot));
  boot.magic = DSCM_SESSION_MAGIC;
  boot.size = sizeof(boot);
  snprintf(boot.rom_path, sizeof(boot.rom_path), "%s", rom_path);
  snprintf(boot.host, sizeof(boot.host), "%s", settings->host);
  snprintf(boot.username, sizeof(boot.username), "%s", settings->username);
  snprintf(boot.token, sizeof(boot.token), "%s", settings->token);
  snprintf(boot.game_hash, sizeof(boot.game_hash), "%s", hash);
  boot.hardcore = (uint8_t)(settings->hardcore != 0);
  boot.flags = (settings->spectator ? DSCM_BOOT_SPECTATOR : 0)
             | (settings->stream ? DSCM_BOOT_STREAM : 0);
  boot.unix_time = (uint32_t)time(NULL);
  if (net_clock_offset_q(&boot.utc_offset_q))
    boot.flags |= DSCM_BOOT_UTC_KNOWN;
  /* Each reply and the CA end in at least one NUL (DSCM_SESSION_PAD): the
   * module uses them in place. */
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) {
    boot.preload_offset[i] = offset;
    boot.preload_length[i] = (uint32_t)g_reply_len[i];
    offset += DSCM_SESSION_PAD(g_reply_len[i]);
  }

  /* The game keeps using this Wi-Fi connection: pass its settings (no keys;
   * the chip keeps those itself). */
  {
    struct in_addr gateway, netmask, dns1, dns2;
    struct in_addr ip = Wifi_GetIPInfo(&gateway, &netmask, &dns1, &dns2);
    if (settings_keep_wifi(settings) && ip.s_addr && Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, boot.mac) >= 0) {
      boot.net_valid = 1;
      boot.ip = ip.s_addr;
      boot.netmask = netmask.s_addr;
      boot.gateway = gateway.s_addr;
      boot.dns = dns1.s_addr;
    }
  }
  /* so the module can rejoin it; nds-bootstrap adds the PMK in RAM */
  {
    net_ap_t ap;
    if (boot.net_valid && net_current_ap(&ap) == 0) {
      memcpy(boot.ap_ssid, ap.ssid, sizeof(boot.ap_ssid));
      boot.ap_ssid_len = ap.ssid_len;
      memcpy(boot.ap_bssid, ap.bssid, 6);
      boot.ap_channel = ap.channel;
      boot.ap_security = ap.security;
      boot.ap_pair_cipher = ap.pair_cipher;
      boot.ap_group_cipher = ap.group_cipher;
    }
  }
  /* extra trusted CA for a self-hosted or test server: the module takes DER */
  ca = fopen(DSC_DIR "ca.pem", "rb");
  if (ca) {
    size_t n = fread(ca_pem, 1, sizeof(ca_pem) - 1, ca);
    mbedtls_x509_crt chain;
    const mbedtls_x509_crt* c;
    fclose(ca);
    ca_pem[n] = '\0';
    mbedtls_x509_crt_init(&chain);
    if (n > 0 && mbedtls_x509_crt_parse(&chain, (const unsigned char*)ca_pem, n + 1) == 0) {
      for (c = &chain; c && c->raw.len; c = c->next) {
        if (ca_len + c->raw.len > sizeof(ca_der)) { ca_len = 0; break; }
        memcpy(ca_der + ca_len, c->raw.p, c->raw.len);
        ca_len += c->raw.len;
      }
    }
    mbedtls_x509_crt_free(&chain);
    if (ca_len) {
      boot.ca_offset = offset;
      boot.ca_length = (uint32_t)ca_len;
      offset += DSCM_SESSION_PAD(ca_len);
    }
  }
  boot.file_size = offset;
  if (offset > DSCM_SESSION_MAX) {
    /* the set needs more memory than the module has (docs/architecture.md, "Big sets") */
    log_line("session: %u bytes, over the %u-byte limit", (unsigned)offset, (unsigned)DSCM_SESSION_MAX);
    g_too_big = offset;
    return 0;
  }

  f = fopen(DSC_DIR "session.bin", "wb");
  if (!f)
    return 0;
  fwrite(&boot, 1, sizeof(boot), f);
  fwrite(pad, 1, ((sizeof(boot) + 3) & ~3u) - sizeof(boot), f);
  for (i = 0; i < DSCM_PRELOAD_COUNT; i++) {
    if (g_reply_len[i])
      fwrite(g_reply[i], 1, g_reply_len[i], f);
    fwrite(pad, 1, DSCM_SESSION_PAD(g_reply_len[i]) - g_reply_len[i], f);
  }
  if (ca_len) {
    fwrite(ca_der, 1, ca_len, f);
    fwrite(pad, 1, DSCM_SESSION_PAD(ca_len) - ca_len, f);
  }
  i = ferror(f);
  fclose(f);
  return !i;
}
