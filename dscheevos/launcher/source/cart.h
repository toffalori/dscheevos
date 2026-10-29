/* Cheevotron cart mode: the game is the cartridge in Slot-1. The launcher
 * reads its header (raw mode, no KEY1) for the name, the game code and the
 * phone check, and writes it to a small file that stands in for the ROM in
 * the session and in nds-bootstrap's first stage; nds-bootstrap then boots
 * and hashes the cartridge itself. */
#ifndef DSC_CART_H
#define DSC_CART_H

#include <stdint.h>

/* Reads the inserted cartridge's header (0x200 bytes) and checks its CRCs.
 * Returns 0 when there is no cartridge or its header doesn't read. */
int cart_read_header(uint8_t header[0x200]);

/* Writes the header stub for this cartridge and returns its path in rom_path
 * (named after the cartridge's title, so the splash and the loading screen
 * show the game). Returns 0 if the file couldn't be written. */
int cart_write_stub(const uint8_t header[0x200], char* rom_path, int size);

#endif
