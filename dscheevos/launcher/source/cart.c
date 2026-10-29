#include "cart.h"
#include "config.h"
#include "log.h"

#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CART_DIR   DSC_DIR "carts"
#define STUB_SIZE  0x1000   /* the header, then zeros: nds-bootstrap's first stage reads up to 0x240 */

static void wait_frames(int n)
{
  while (n-- > 0)
    swiWaitForVBlank();
}

/* The same sequence nds-bootstrap's first stage uses for IR cartridges on a
 * DSi (its conf_sd.cpp): power-cycle the slot so the card is in its raw
 * (unencrypted) state, then the reset and the header read libnds does. The
 * card is left in raw mode and powered; nds-bootstrap's cart boot starts
 * from there with its own reset and the KEY1 handshake. */
int cart_read_header(uint8_t header[0x200])
{
  uint16_t crc;
  memset(header, 0, 0x200);
  sysSetCardOwner(BUS_OWNER_ARM9);
  if (isDSiMode()) {
    disableSlot1();
    wait_frames(25);
    enableSlot1();
    wait_frames(15);
    cardParamCommand(CARD_CMD_DUMMY, 0,
                     CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(1) | CARD_DELAY1(0x1FFF) | CARD_DELAY2(0x3F),
                     NULL, 0);
  }
  cardReadHeader(header);
  sysSetCardOwner(BUS_OWNER_ARM7);
  crc = swiCRC16(0xFFFF, header, 0x15E);
  if (crc != (uint16_t)(header[0x15E] | (header[0x15F] << 8)) || header[0x15C] != 0x56 || header[0x15D] != 0xCF) {
    log_line("cart: no header (crc %04X, logo %02X%02X)", crc, header[0x15D], header[0x15C]);
    return 0;
  }
  log_line("cart: %.12s %.4s, unit %02X, arm9 %08lX+%08lX, arm7 %08lX+%08lX, icon %08lX",
           (const char*)header, (const char*)header + 0x0C, header[0x12],
           *(unsigned long*)(header + 0x20), *(unsigned long*)(header + 0x2C),
           *(unsigned long*)(header + 0x30), *(unsigned long*)(header + 0x3C), *(unsigned long*)(header + 0x68));
  return 1;
}

/* The stub's file name: the header's title, trimmed to what a FAT name and
 * the screens take; the game code when the title gives nothing. */
static void stub_name(const uint8_t header[0x200], char* name, int size)
{
  int i, n = 0;
  for (i = 0; i < 12 && n < size - 1; i++) {
    char c = (char)header[i];
    if (!c)
      break;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.')
      name[n++] = c;
    else
      name[n++] = '_';
  }
  while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '.'))
    n--;
  if (n == 0) {
    memcpy(name, header + 0x0C, 4);
    n = 4;
  }
  name[n] = '\0';
}

/* Whether the stub on the SD card is already this header and its zeros:
 * writing it took about 1 s at every start, reading it back a few ms. */
static int stub_written(const char* path, const uint8_t header[0x200])
{
  static uint8_t stub[STUB_SIZE];
  struct stat st;
  FILE* f;
  int i, same;
  if (stat(path, &st) != 0 || st.st_size != STUB_SIZE)
    return 0;
  f = fopen(path, "rb");
  if (!f)
    return 0;
  same = fread(stub, 1, STUB_SIZE, f) == STUB_SIZE && !memcmp(stub, header, 0x200);
  fclose(f);
  for (i = 0x200; same && i < STUB_SIZE; i++)
    same = !stub[i];
  return same;
}

int cart_write_stub(const uint8_t header[0x200], char* rom_path, int size)
{
  static const uint8_t zeros[0x200];
  char name[16];
  FILE* f;
  int i, ok;
  stub_name(header, name, sizeof(name));
  snprintf(rom_path, (size_t)size, CART_DIR "/%s.nds", name);
  if (stub_written(rom_path, header)) {
    log_line("cart: stub %s as written before", rom_path);
    return 1;
  }
  mkdir(CART_DIR, 0777);
  f = fopen(rom_path, "wb");
  if (!f) {
    log_line("cart: can't write %s", rom_path);
    return 0;
  }
  ok = fwrite(header, 1, 0x200, f) == 0x200;
  for (i = 0x200; ok && i < STUB_SIZE; i += sizeof(zeros))
    ok = fwrite(zeros, 1, sizeof(zeros), f) == sizeof(zeros);
  ok = !fclose(f) && ok;
  log_line("cart: stub %s%s", rom_path, ok ? "" : " (write failed)");
  return ok;
}
