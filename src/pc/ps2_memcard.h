#ifndef _PS2_MEMCARD_H
#define _PS2_MEMCARD_H

#include <stdbool.h>
#include <stdint.h>

#define PS2_SAVE_PATH "SM64"

// size of the emulated EEPROM
#define PS2_EEPROM_SIZE 512

// what the memcard thread is currently doing
enum Ps2McActivity {
    PS2_MC_IDLE,
    PS2_MC_SEARCHING, // boot: looking for a card and reading the save
    PS2_MC_SAVING,    // writing the save
};

// outcome of the last finished operation
enum Ps2McResult {
    PS2_MC_RES_NONE,
    PS2_MC_RES_LOADED,  // boot: existing save loaded
    PS2_MC_RES_CREATED, // boot: no save found, an empty one was created
    PS2_MC_RES_SAVED,   // save written
    PS2_MC_RES_ERROR,   // card missing, unformatted, full, unreadable or ID mismatch
};

// inits libmc (memcard driver must be loaded) and starts the memcard thread;
// no card access happens until the boot sequence starts
bool ps2_memcard_init(void);

// boot sequence: find card, load or create the save; can be run again to rescan,
// which starts from blank data
void ps2_memcard_boot_start(void);
bool ps2_memcard_boot_done(void);
enum Ps2McResult ps2_memcard_boot_wait(void);

// EEPROM access from the game thread; reads and writes hit the RAM copy,
// writes also queue a background save
void ps2_memcard_read(void *dst, const uint32_t ofs, const uint32_t size);
void ps2_memcard_write(const void *src, const uint32_t ofs, const uint32_t size);

// audio handshake: audsrv RPCs block while the IOP is writing to the card, so
// before any card access the memcard thread requests the IOP and waits until the
// game thread has silenced the audio and granted it
void ps2_memcard_set_audio_sync(bool enable);
bool ps2_memcard_iop_requested(void);
void ps2_memcard_iop_grant(void);
bool ps2_memcard_iop_in_use(void);

// status for the UI; the result counter increases every time an operation finishes
enum Ps2McActivity ps2_memcard_activity(void);
uint32_t ps2_memcard_last_result(enum Ps2McResult *res);

#endif // _PS2_MEMCARD_H
