#ifndef _PS2_MEMCARD_UI_H
#define _PS2_MEMCARD_UI_H

#include <stdbool.h>

// runs one frame of the boot screen (warning, then card detection) and returns
// true while it's active; the game loop must not run until it returns false
bool ps2_memcard_ui_boot_frame(void);

// value the file select loop returns to make the level script reload the menu
#define PS2_MENU_RESCAN_EXIT 100

enum Ps2MenuState {
    PS2_MENU_RUN,     // update the menu normally
    PS2_MENU_BUSY,    // rescan in progress, the menu must not update
    PS2_MENU_RESTART, // rescan finished, reload the menu
};

// file select hook, called every frame before the menu updates; Z + X (A) on the
// main screen fades to black and rescans the memory card
enum Ps2MenuState ps2_memcard_ui_menu_update(const bool can_rescan);

// draws the memcard status icon during gameplay; call once per frame at the end
// of the game's rendering so it stays on top
void ps2_memcard_ui_render(void);

#endif // _PS2_MEMCARD_UI_H
