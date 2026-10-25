#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_PATH DSC_DIR "dscheevos.ini"
#define DEFAULT_HOST "https://retroachievements.org"

static char* trim(char* s)
{
  char* e;
  while (isspace((unsigned char)*s)) s++;
  e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
  return s;
}

/* Calls fn(key, value) for each "KEY = VALUE" line. */
static int read_ini(const char* path, void (*fn)(const char*, const char*, void*), void* ud)
{
  char line[320];
  FILE* f = fopen(path, "r");
  if (!f)
    return 0;
  while (fgets(line, sizeof(line), f)) {
    char* eq = strchr(line, '=');
    if (!eq || line[0] == '[' || line[0] == ';' || line[0] == '#')
      continue;
    *eq = '\0';
    fn(trim(line), trim(eq + 1), ud);
  }
  fclose(f);
  return 1;
}

static void copy(char* dst, size_t size, const char* src)
{
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

static void on_setting(const char* key, const char* value, void* ud)
{
  dsc_settings_t* s = (dsc_settings_t*)ud;
  if (!strcmp(key, "ENABLED")) s->enabled = atoi(value) != 0;
  else if (!strcmp(key, "HARDCORE")) s->hardcore = atoi(value) != 0;
  else if (!strcmp(key, "SEND_DURING_PLAY")) s->live = atoi(value) != 0;
  else if (!strcmp(key, "USERNAME")) copy(s->username, sizeof(s->username), value);
  else if (!strcmp(key, "TOKEN")) copy(s->token, sizeof(s->token), value);
  else if (!strcmp(key, "SPECTATOR")) s->spectator = atoi(value) != 0;
  else if (!strcmp(key, "SKIP_UNOFFICIAL")) s->skip_unofficial = atoi(value) != 0;
  else if (!strcmp(key, "STREAM")) s->stream = atoi(value) != 0;
  else if (!strcmp(key, "HOST")) copy(s->host, sizeof(s->host), value);
}

void settings_load(dsc_settings_t* s)
{
  memset(s, 0, sizeof(*s));
  s->enabled = 1;
  s->hardcore = 1;
  s->live = 1;
  strcpy(s->host, DEFAULT_HOST);
  read_ini(SETTINGS_PATH, on_setting, s);
}

int settings_save(const dsc_settings_t* s)
{
  FILE* f = fopen(SETTINGS_PATH, "w");
  if (!f)
    return 0;
  fprintf(f, "[DSCHEEVOS]\nENABLED = %d\nHARDCORE = %d\nSEND_DURING_PLAY = %d\nUSERNAME = %s\nTOKEN = %s\n",
          s->enabled, s->hardcore, s->live, s->username, s->token);
  if (strcmp(s->host, DEFAULT_HOST) != 0)
    fprintf(f, "HOST = %s\n", s->host);
  if (s->spectator)
    fprintf(f, "SPECTATOR = 1\n");
  if (s->skip_unofficial)
    fprintf(f, "SKIP_UNOFFICIAL = 1\n");
  if (s->stream)
    fprintf(f, "STREAM = 1\n");
  fclose(f);
  return 1;
}

typedef struct game_ini_t {
  char* path;
  int path_size;
  int* dsi_mode;
} game_ini_t;

static void on_bootstrap(const char* key, const char* value, void* ud)
{
  game_ini_t* g = (game_ini_t*)ud;
  if (!strcmp(key, "NDS_PATH")) copy(g->path, (size_t)g->path_size, value);
  else if (!strcmp(key, "DSI_MODE")) *g->dsi_mode = atoi(value);
}

static void on_donor20(const char* key, const char* value, void* ud)
{
  game_ini_t* g = (game_ini_t*)ud;
  if (!strcmp(key, "DONOR20_NDS_PATH")) copy(g->path, (size_t)g->path_size, value);
}

int bootstrap_ini_donor20(char* path, int path_size)
{
  game_ini_t g = { path, path_size, NULL };
  path[0] = '\0';
  read_ini("sd:/_nds/nds-bootstrap.ini", on_donor20, &g);
  return path[0] != '\0';
}

int bootstrap_ini_game(char* path, int path_size, int* dsi_mode)
{
  game_ini_t g = { path, path_size, dsi_mode };
  path[0] = '\0';
  *dsi_mode = 1;  /* nds-bootstrap's default when DSI_MODE is missing */
  read_ini("sd:/_nds/nds-bootstrap.ini", on_bootstrap, &g);
  return path[0] != '\0';
}
