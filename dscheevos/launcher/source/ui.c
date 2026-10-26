#include "ui.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static PrintConsole top, bottom;

/* Dark indigo backdrop (console palette entry 0 = the screen background). */
#define UI_BACKDROP RGB15(2, 3, 9)

static int splash_rows = 0;   /* rows the splash drew on top; status prints below */

void ui_init(void)
{
  videoSetMode(MODE_0_2D);
  videoSetModeSub(MODE_0_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankC(VRAM_C_SUB_BG);
  consoleInit(&top, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
  consoleInit(&bottom, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 3, false, true);
  BG_PALETTE[0] = UI_BACKDROP;
  BG_PALETTE_SUB[0] = UI_BACKDROP;
  consoleSelect(&top);
}

/* Visible length: the printed width, skipping "\x1b[...m" colour sequences. */
static int visible_len(const char* s)
{
  int n = 0;
  while (*s) {
    if (s[0] == '\x1b' && s[1] == '[') {
      s += 2;
      while (*s && *s != 'm') s++;
      if (*s) s++;
    } else {
      n++;
      s++;
    }
  }
  return n;
}

/* Prints text centred on a 32-column row (row 0 at the top). */
static void center(int row, const char* text)
{
  int col = (32 - visible_len(text)) / 2;
  if (col < 0) col = 0;
  printf("\x1b[%d;%dH%s", row, col, text);
}

void ui_splash(const char* game)
{
  char name[31];
  consoleSelect(&top);
  consoleClear();
  center(8, "\x1b[36;1mC H E E V O T R O N\x1b[39;0m");               /* wordmark, bright cyan */
  center(9, "\x1b[36m--------------------\x1b[39m");                  /* accent rule */
  center(11, "\x1b[30;1machievements on real hardware\x1b[39;0m");    /* dim tagline */
  if (game && game[0]) {
    snprintf(name, sizeof(name), "%s", game);
    center(14, name);
  }
  splash_rows = 17;   /* ui_status lines go here, below the splash */
  consoleSelect(&top);
  printf("\x1b[%d;0H", splash_rows);
}

void ui_header(const char* game, const char* line2)
{
  consoleSelect(&top);
  consoleClear();
  printf("\x1b[0;0H\x1b[36;1m Cheevotron\x1b[39;0m");
  printf("\n--------------------------------");
  printf("\n %.30s\n", game ? game : "");
  if (line2)
    printf(" %.30s\n", line2);
  printf("\n");
  splash_rows = 0;
}

void ui_status(const char* fmt, ...)
{
  va_list ap;
  consoleSelect(&top);
  va_start(ap, fmt);
  if (splash_rows) {
    /* One centred status line on the splash, replacing the previous one. */
    char line[64];
    vsnprintf(line, sizeof(line), fmt, ap);
    printf("\x1b[%d;0H\x1b[2K", splash_rows);
    center(splash_rows, line);
  } else {
    printf(" ");
    vprintf(fmt, ap);
    printf("\n");
  }
  va_end(ap);
}

void ui_clear_bottom(void)
{
  consoleSelect(&bottom);
  consoleClear();
}

void ui_bottom(const char* fmt, ...)
{
  va_list ap;
  consoleSelect(&bottom);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

int ui_choose(const char* message, const char* a_label, const char* b_label)
{
  ui_clear_bottom();
  ui_bottom("\n %s\n\n", message);
  if (a_label) ui_bottom(" \x1b[32;1mA\x1b[39;0m  %s\n", a_label);
  if (b_label) ui_bottom(" \x1b[31;1mB\x1b[39;0m  %s\n", b_label);
  for (;;) {
    swiWaitForVBlank();
    scanKeys();
    if (a_label && (keysDown() & KEY_A)) return 'A';
    if (b_label && (keysDown() & KEY_B)) return 'B';
  }
}

int ui_wait(const char* message, int frames)
{
  ui_clear_bottom();
  ui_bottom("\n %s\n", message);
  while (frames-- > 0) {
    int k;
    swiWaitForVBlank();
    scanKeys();
    k = keysDown() & (KEY_A | KEY_B | KEY_START | KEY_SELECT);
    if (k) return k;
  }
  return 0;
}

int ui_input(const char* prompt, char* buf, int size, int masked)
{
  Keyboard* kb;
  int len = (int)strlen(buf);
  ui_clear_bottom();
  kb = keyboardDemoInit();
  keyboardShow();
  for (;;) {
    int c, i;
    consoleSelect(&top);
    printf("\x1b[20;0H\x1b[2K %s\n\x1b[2K > ", prompt);
    for (i = 0; i < len; i++) putchar(masked ? '*' : buf[i]);
    printf("_");
    do {
      swiWaitForVBlank();
      scanKeys();
      if (keysDown() & KEY_B && len == 0) { keyboardHide(); return 0; }
      if (keysDown() & KEY_START) c = '\n';
      else c = keyboardUpdate();
    } while (c <= 0 && c != '\n');
    if (c == '\n' || c == DVK_ENTER) {
      if (len > 0) break;
    } else if (c == '\b' || c == DVK_BACKSPACE) {
      if (len > 0) buf[--len] = '\0';
    } else if (c >= 32 && c < 127 && len < size - 1) {
      buf[len++] = (char)c;
      buf[len] = '\0';
    }
  }
  (void)kb;
  keyboardHide();
  consoleSelect(&top);
  printf("\x1b[20;0H\x1b[2K\n\x1b[2K");
  return 1;
}
