#pragma once
#include <3ds.h>
#include <stdbool.h>

// The PC's item screen on the Vita (0x475230 and what it calls): the sheet's item pictures on their
// ring, the six headings on their bars, the status panel, the blue frame.
typedef struct {
	void (*use)(int item, int mark0, int mark1);    // Use, and the marks with it; closes the screen
	void (*power)(int item);                        // the flashlight's or radio's switch
	bool (*power_on)(int item);
	void (*reload)(int item);
	void (*leave)(void (*then)(void));              // fade out, close the screen, then this (or nothing)
	void (*options)(void);
	void (*memos)(void);
	void (*map)(void);
	void (*read)(const char* picture, const char* text);   // a letter or a book full screen, then back
} ItemMenuHost;

void itemmenu_init(const char* pack_dir, int chapter);
void itemmenu_open(const ItemMenuHost* host, int last_item);
bool itemmenu_update(u32 down, u32 held);
void itemmenu_draw(void);
// The status panel in the 400 x 240 layout
void itemmenu_panel(float* x0, float* y0, float* x1, float* y1);
int itemmenu_current(void);
