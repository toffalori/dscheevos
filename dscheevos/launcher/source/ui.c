#include "ui.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* nds-bootstrap's copy of this screen draws from the same code: trophy, title,
 * bar and first-start note here, as a bitmap behind the console text. */
#include "../../../external/nds-bootstrap/retail/common/include/cheevotron_loading.h"

static PrintConsole top, bottom;
static int top_bitmap = -1;   /* BG3: the splash's drawn part */

/* White theme: the start goes white from here to the game (the loaders between
 * show white, and nds-bootstrap shows a copy of the splash in these colours:
 * keep them equal to holdLoadingScreen() in its bootloaderi/source/arm9/
 * main.arm9.c). Palette entry 0 is the screen background; entry 16*n+15
 * holds console colour n. */
#define UI_BACKDROP CHEEVOTRON_RGB_BACKGROUND
#define UI_WORDMARK CHEEVOTRON_RGB_WORDMARK   /* \x1b[36;1m */
#define UI_ACCENT   CHEEVOTRON_RGB_ACCENT     /* \x1b[36m: Loading */
#define UI_DIM      CHEEVOTRON_RGB_DIM        /* \x1b[30;1m */
#define UI_TEXT     CHEEVOTRON_RGB_TEXT       /* default (\x1b[39m) */

static int splash_rows = 0;   /* rows the splash drew on top; status prints below */
static int dots = 3, dot_frames;   /* the splash's "Loading..." dots */

static void apply_theme(u16* pal)
{
  pal[0] = UI_BACKDROP;
  pal[6 * 16 + 15] = UI_ACCENT;
  pal[14 * 16 + 15] = UI_WORDMARK;
  pal[8 * 16 + 15] = UI_DIM;
  pal[7 * 16 + 15] = UI_TEXT;
  pal[15 * 16 + 15] = UI_TEXT;
  pal[9 * 16 + 15] = RGB15(24, 3, 3);    /* \x1b[31;1m: errors */
  pal[10 * 16 + 15] = RGB15(2, 17, 4);   /* \x1b[32;1m: A */
  pal[11 * 16 + 15] = RGB15(21, 13, 0);  /* \x1b[33;1m: warnings */
  /* the bitmap's colours (cheevotron_loading.h); the console's glyphs use only entries 16*n+15 */
  pal[CHEEVOTRON_LOADING_WORDMARK] = CHEEVOTRON_RGB_WORDMARK;
  pal[CHEEVOTRON_LOADING_ACCENT] = CHEEVOTRON_RGB_ACCENT;
  pal[CHEEVOTRON_LOADING_DIM] = CHEEVOTRON_RGB_DIM;
  pal[CHEEVOTRON_LOADING_TEXT] = CHEEVOTRON_RGB_TEXT;
  pal[CHEEVOTRON_LOADING_SHADOW] = CHEEVOTRON_RGB_SHADOW;
  pal[CHEEVOTRON_LOADING_GOLD] = CHEEVOTRON_RGB_GOLD;
}

void ui_init(void)
{
  videoSetMode(MODE_3_2D);   /* BG0 the console, BG3 the splash bitmap behind it */
  videoSetModeSub(MODE_0_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankC(VRAM_C_SUB_BG);
  consoleInit(&top, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
  consoleInit(&bottom, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 3, false, true);
  /* 8 bpp at 0x06010000, past the console's font (base 0) and map (0x0600F800) */
  top_bitmap = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
  bgSetPriority(top_bitmap, 3);
  bgHide(top_bitmap);
  apply_theme(BG_PALETTE);
  apply_theme(BG_PALETTE_SUB);
  consoleSelect(&top);
}

/* The splash's drawn part into BG3, shown behind the console. */
static void draw_splash_bitmap(void)
{
  static u8 frame[256 * 192] __attribute__((aligned(4)));
  vu32* dst = (vu32*)bgGetGfxPtr(top_bitmap);
  const u32* src = (const u32*)frame;
  memset(frame, CHEEVOTRON_LOADING_BACKGROUND, sizeof(frame));
  cheevotronLoadingFrame(frame);
  for (int i = 0; i < (int)(sizeof(frame) / 4); i++)
    dst[i] = src[i];   /* VRAM takes no byte writes */
  bgShow(top_bitmap);
}

/* Keeps the splash's dots moving while something waits, so a slow start
 * doesn't look like a hang (nds-bootstrap's copy of the screen goes on with
 * them). Call once per frame. */
void ui_tick(void)
{
  if (!splash_rows || ++dot_frames < 20)
    return;
  dot_frames = 0;
  dots = (dots + 1) & 3;
  consoleSelect(&top);
  printf("\x1b[16;18H\x1b[36m%.*s%*s\x1b[39m", dots, "...", 3 - dots, "");   /* "Loading..." is at column 11 */
  printf("\x1b[%d;0H", splash_rows);
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
  cheevotronLoadingName(name, game ? game : "");   /* without the extension, as nds-bootstrap's copy */
  if (splash_rows) {
    /* already up (shown before the game was known): only add its name, so it stays one screen */
    printf("\x1b[14;0H\x1b[2K");
    center(14, name);
    printf("\x1b[%d;0H", splash_rows);
    return;
  }
  consoleClear();
  draw_splash_bitmap();   /* trophy, title, bar, first-start note */
  if (name[0])
    center(14, name);
  center(16, "\x1b[36mLoading...\x1b[39m");   /* cyan, above the status lines */
  splash_rows = 18;   /* ui_status lines go here, below the splash */
  consoleSelect(&top);
  printf("\x1b[%d;0H", splash_rows);
}

void ui_header(const char* game, const char* line2)
{
  consoleSelect(&top);
  consoleClear();
  bgHide(top_bitmap);
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
