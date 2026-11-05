// What the compatibility layer's files share. Vita-only.
#pragma once
#include <3ds.h>

#define VITA_DATA_DIR "ux0:data/sh23ds"

void vita_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool vita_quit_requested(void);
bool vita_cross_confirms(void);   // the system's confirm button: Cross (KEY_A is Cross) or Circle
const char* vita_path(const char* in, char* buf, size_t n);

// c3d_gl.c
void vita_gl_init(void);
enum { VITA_LAYOUT_DUAL = 0, VITA_LAYOUT_SINGLE = 1 };
extern int vita_layout;
void vita_res_save(int low);     // Options, Resolution: read as the game next starts (c3d_gl.c)
// Where a front-panel touch (960 x 544 pixels) lands on the 3DS touch screen, if it does
bool vita_touch_to_bottom(int x, int y, u16* px, u16* py);
// nodata.c's software screens
void vita_present_framebuffers(const u8* top, const u8* bottom);
