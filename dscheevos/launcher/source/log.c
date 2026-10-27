#include "log.h"
#include "config.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define LOG_PATH DSC_DIR "launcher.log"

/* Milliseconds since log_start, for the start-up timeline: the CPU timing
 * timers (0 and 1), which tls.c then reads for entropy without restarting.
 * DSWiFi takes timers 2 and 3. Wraps after 128 s. */
static void ticks_start(void)
{
  cpuStartTiming(0);
}

static unsigned long ticks_ms(void)
{
  return (unsigned long)(cpuGetTiming() / (BUS_CLOCK / 1000));
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
