/* Text UI: top screen shows the game and status, bottom screen shows
 * choices and the keyboard. */
#ifndef DSC_UI_H
#define DSC_UI_H

void ui_init(void);
void ui_header(const char* game, const char* line2);   /* top screen */
/* Branded loading screen: wordmark, the game name, and a status line. */
void ui_splash(const char* game);
/* Moves the splash's "Loading..." dots; call once per frame while waiting. */
void ui_tick(void);
void ui_status(const char* fmt, ...);                  /* top screen, below the header */
void ui_clear_bottom(void);
void ui_bottom(const char* fmt, ...);

/* Shows a message and the A/B choices; returns 'A' or 'B'. */
int ui_choose(const char* message, const char* a_label, const char* b_label);
/* Shows a message for at most `frames`; any key skips. Returns the key mask pressed (0 if none). */
int ui_wait(const char* message, int frames);
/* Keyboard entry on the bottom screen; returns 0 if cancelled. */
int ui_input(const char* prompt, char* buf, int size, int masked);

#endif
