#pragma once
#include <3ds.h>
#include <stdbool.h>

// The pocket radio (0x48cbc0, event.c): four looping layers of static whose levels follow the enemies
// near James. They are the PC's stereo channels 1..4, internal sounds 5360..5363 (0x8a93b4), which the
// bank carries as game ids RADIO_SE.. (tools/sh2conv/sound.py).
#define RADIO_SE 19990

// What the game is showing, as the PC's gEventIndex tells the radio (0x401fc0).
typedef enum {
	RADIO_PLAY,     // play: full level
	RADIO_ITEMS,    // gEventIndex 5, 6: half level
	RADIO_PAUSE,    // gEventIndex 8: a quarter
	RADIO_SILENT    // gEventIndex 7, 9, 0xb, 0xc and everything outside a game: off
} RadioScene;

// Mode 0 follows the enemies and needs the radio in the inventory; the others are story scenes
// (DAT_01f7d584): 1 the radio on the floor before the first monster (0x59e1e0), 2 that monster's
// scene (0x59da20), 3 after the videotape (0x577fa0).
void radio_set_mode(int mode);
// New game, Continue: the switch on, the level given (10 in 0x49e980, 4 in 0x5a21f0), mode 0.
void radio_reset(int volume);
void radio_stage_loaded(int stage);    // the stage records' setup: 0x59e1e0, 0x575b00

bool radio_on(void);
void radio_toggle(void);               // the item screen's switch (0x471590)
int radio_volume(void);                // DAT_01f7d408, 0..15
void radio_set_volume(int v);

void radio_update(RadioScene scene, float dt);
