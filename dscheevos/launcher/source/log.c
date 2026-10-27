#include "log.h"
#include "config.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define LOG_PATH DSC_DIR "launcher.log"

/* Milliseconds since log_start, for the start-up timeline. ARM9 timers 2 and
 * 3 cascaded at 32728 Hz (BUS_CLOCK / 1024): tls.c and bench.c restart
 * timer 0 for their own measurements. */
static void ticks_start(void)
{
  TIMER_CR(2) = 0;
  TIMER_CR(3) = 0;
  TIMER_DATA(2) = 0;
  TIMER_DATA(3) = 0;
  TIMER_CR(3) = TIMER_ENABLE | TIMER_CASCADE;
  TIMER_CR(2) = TIMER_ENABLE | TIMER_DIV_1024;
}

static unsigned long ticks_ms(void)
{
  u16 hi, lo;
  do {
    hi = TIMER_DATA(3);
    lo = TIMER_DATA(2);
  } while (hi != TIMER_DATA(3));
  return (unsigned long)((((u64)hi << 16) | lo) * 1000 / (BUS_CLOCK / 1024));
}

void log_start(void)
{
  FILE* f = fopen(LOG_PATH, "w");
  ticks_start();
  if (f) {
    /* the console clock, to line up with nds-bootstrap's stage1.log */
    unsigned long s = (unsigned long)(time(NULL) % 86400);
    fprintf(f, "DScheevos launcher " DSC_VERSION ", %02lu:%02lu:%02lu\n", s / 3600, s / 60 % 60, s % 60);
    fclose(f);
  }
}

void log_line(const char* fmt, ...)
{
  va_list ap;
  FILE* f = fopen(LOG_PATH, "a");
  if (!f)
    return;
  fprintf(f, "%6lu ", ticks_ms());
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}
