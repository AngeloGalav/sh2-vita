// See itemmenu.h. Ported from the PC: 0x475230 the frame, 0x46a4f0 and 0x46c1a0 the ring, 0x475db0 the
// sprites, 0x46e150 the headings' bars, 0x46e700 the command bar, 0x46cb50 and 0x470c60 the frame and
// its trail, 0x46f180 the status panel, 0x470570 the marks, 0x472e80 the texts, 0x474410 and
// 0x471590 the input. The menu's own units are x -256..256 and y -240..240, the middle of the screen
// at 0, 0; each frame steps as one of the PC's (30 a second).
#include "itemmenu.h"
#include "combat.h"
#include "events.h"
#include "font.h"
#include "prog.h"
#include "sfx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tex3ds.h>

enum { ST_ITEMS = 1, ST_OPTION = 2, ST_MEMO = 3, ST_MAP = 4, ST_EQUIP = 5, ST_CMD = 6, ST_CMD_EQ = 7, ST_READ = 11, ST_PIC = 12 };
enum { CMD_USE = 0, CMD_RELOAD = 1, CMD_VOLUME = 2, CMD_POWER = 3, CMD_ON = 4, CMD_OFF = 5, CMD_EXAMINE = 6, CMD_EQUIP = 7,
       CMD_UNEQUIP = 8, CMD_COMBINE = 15 };
#define MAX_ITEMS 86
#define SFX_TURN SFX_MENU_MOVE

static C3D_Tex s_sheet[2];
static bool s_sheet_ok[2];
static struct { s32 id[79]; float scale[79]; s16 uv[79][3]; s32 xid[12]; s16 xuv[12][3]; } s_tab;
static bool s_tab_ok;
static int s_chapter;
static ItemMenuHost s_host;

static int s_state, s_n, s_start, s_cur, s_row, s_rows;
static u8 s_list[MAX_ITEMS];
static float s_ang[MAX_ITEMS], s_fade[MAX_ITEMS];
static bool s_turning;
static float s_frame[4][2], s_trail[8][4][2];   // the frame's corners: top left, bottom left, bottom right, top right
static float s_bar_row, s_bar_on, s_flash;
static float s_eq_fade[MAX_ITEMS];
static int s_mark[3];
static float s_mark_a[3];
static int s_hold, s_seed;
static float s_tint_t;

static float X(float x) { return 200.0f + x * 0.625f; }
static float Y(float y) { return 120.0f + y * 0.46875f; }

void itemmenu_panel(float* x0, float* y0, float* x1, float* y1)
{
	// 0x46f180: -0.44922 .. -0.17578 of the width, -0.36458 .. -0.14583 of the height
	*x0 = X(-230.0f), *x1 = X(-90.0f), *y0 = Y(-175.0f), *y1 = Y(-70.0f);
}

void itemmenu_init(const char* pack_dir, int chapter)
{
	s_chapter = chapter;
	static const char* const NAME[2] = { "itemmenu", "itemmenux" };
	char path[160];
	for (int i = 0; i < 2; i++) {
		if (s_sheet_ok[i])
			continue;
		snprintf(path, sizeof path, "%s/ui/%s.t3x", pack_dir, NAME[i]);
		FILE* f = fopen(path, "rb");
		if (!f)
			continue;
		Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, &s_sheet[i], NULL, false);
		fclose(f);
		if (t) {
			Tex3DS_TextureFree(t);
			C3D_TexSetFilter(&s_sheet[i], GPU_LINEAR, GPU_LINEAR);
			s_sheet_ok[i] = true;
		}
	}
	if (!s_tab_ok) {
		snprintf(path, sizeof path, "%s/ui/itemmenu.dat", pack_dir);
		FILE* f = fopen(path, "rb");
		if (f) {
			s_tab_ok = fread(s_tab.id, 4, 79, f) == 79 && fread(s_tab.scale, 4, 79, f) == 79 && fread(s_tab.uv, 6, 79, f) == 79 &&
			           fread(s_tab.xid, 4, 12, f) == 12 && fread(s_tab.xuv, 6, 12, f) == 12;
			fclose(f);
		}
	}
}

// 0x46a5c0: the kind that sets an item's commands (its two state flag exceptions left out)
static int kind_of(int it)
{
	switch (it) {
	case 1: case 2: case 3: return 2;
	case 4: case 6: case 8: case 10: return 4;
	case 5: case 7: case 9: case 11: return 5;
	case 12: case 13: case 14: case 15: case 16: case 17: return 3;
	case 18: return 6;
	case 19: return 7;
	case 20: case 26: case 28: case 32: case 41: case 50: case 51: case 52: return 11;
	case 21: case 22: case 74: case 75: case 79: case 80: case 81: case 84: return 9;
	case 23: case 24: case 29: case 30: case 31: case 43: case 55: case 77: return 8;
	case 25: case 27: case 33: case 34: case 37: case 38: case 39: case 40: case 42: case 44: case 45: case 46:
	case 47: case 48: case 49: case 58: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 72:
	case 73: case 76: case 78: case 82: case 83: case 85: return 12;
	case 35: case 53: case 54: case 62: case 63: case 64: return 1;
	case 36: case 56: case 57: case 59: case 60: case 61: return 10;
	default: return 0;
	}
}

// 0x472e80's commands: item_msg numbers, top to bottom, and the y of the first
static int commands(int it, int* cmd, int* ytext)
{
	int eq = combat_weapon_item() == it ? CMD_UNEQUIP : CMD_EQUIP, n;
	switch (kind_of(it)) {
	case 1: case 12: cmd[0] = CMD_USE, cmd[1] = CMD_COMBINE, n = kind_of(it) == 1 ? 2 : 1; break;
	case 2: cmd[0] = CMD_USE, n = 1; break;
	case 3: cmd[0] = eq, n = 1; break;
	case 4: cmd[0] = eq, cmd[1] = CMD_RELOAD, n = 2; break;
	case 5: cmd[0] = CMD_RELOAD, n = 1; break;
	case 6: cmd[0] = CMD_POWER, n = 1; break;
	case 7: cmd[0] = CMD_POWER, cmd[1] = CMD_VOLUME, n = 2; break;
	case 8: case 9: case 11: cmd[0] = CMD_USE, cmd[1] = CMD_EXAMINE, n = 2; break;
	case 10: cmd[0] = CMD_USE, cmd[1] = CMD_COMBINE, cmd[2] = CMD_EXAMINE, n = 3; break;
	default: n = 0; break;
	}
	if (ytext)
		*ytext = n == 3 ? 0x50 : n == 2 ? 0x5f : 0x69;
	return n;
}

// 0x46a4f0: the ten places of the ring from s_start; the fifth is the chosen one
static void places(int* slot)
{
	memset(slot, 0, sizeof(int) * MAX_ITEMS);
	for (int k = 0; k < 10; k++) {
		int i = s_start + k;
		i = i < s_n ? i : i - s_n;
		if (i >= 0 && i < s_n)
			slot[i] = k;
	}
}

static void list_items(void)
{
	// 0x4729b0: at least nine places, the ones past the items empty, so the ring always has a middle
	s_n = 0;
	for (int i = 1; i < MAX_ITEMS; i++)
		if (ev_item_count(i) > 0 || combat_owns_item(i))
			s_list[s_n++] = (u8)i;
	while (s_n < 9)
		s_list[s_n++] = 0;
}

int itemmenu_current(void) { return s_cur; }

void itemmenu_open(const ItemMenuHost* host, int last_item)
{
	s_host = *host;
	list_items();
	s_start = s_n - 5;                   // 0x4729b0: the first item in the middle, or the last one chosen
	for (int i = 0; last_item && i < s_n; i++)
		if (s_list[i] == last_item)
			s_start = i >= 5 ? i - 5 : i + s_n - 5;
	int slot[MAX_ITEMS];
	places(slot);
	for (int i = 0; i < MAX_ITEMS; i++) {
		s_ang[i] = i < s_n ? (float)slot[i] : 0.0f;
		s_fade[i] = 0.8f;
		s_eq_fade[i] = 0.0f;
	}
	s_state = ST_ITEMS, s_row = 0, s_turning = false, s_hold = 0;
	static const float start[4][2] = { { -120, -120 }, { -120, 120 }, { 120, 120 }, { 120, -120 } };   // 0x46b3e0 case 0
	memcpy(s_frame, start, sizeof s_frame);
	for (int i = 0; i < 8; i++)
		memcpy(s_trail[i], start, sizeof start);
	s_bar_on = s_flash = 0.0f;
	memset(s_mark, 0, sizeof s_mark);
	memset(s_mark_a, 0, sizeof s_mark_a);
	s_cur = 0;
}

// 0x46b3e0's frame for each state: x0, y0, x1, y1
static void frame_target(float* r)
{
	static const float T[14][4] = {
		{ -120, -120, 120, 120 }, { -55, -65, 55, 65 }, { -240, 173, -80, 203 }, { -80, 173, 80, 203 }, { 80, 173, 240, 203 },
		{ -80, -203, 80, -63 }, { 80, -203, 240, -60 }, { 80, -203, 240, -60 }, { -240, -203, -80, -60 }, { -80, -203, 80, -63 },
		{ -55, -65, 55, 65 }, { -230, 80, 230, 170 }, { -100, -150, 100, 50 }, { -55, -52, 55, -48 },
	};
	memcpy(r, T[s_state >= 0 && s_state < 14 ? s_state : 1], sizeof T[0]);
}

// 0x46a550 with 2, 1: halfway there and a unit more a frame
static float ease(float v, float t)
{
	float d = t - v, s = d * 0.5f + (d > 0 ? 1.0f : -1.0f);
	return fabsf(d) <= fabsf(s) ? t : v + s;
}

static void step(void)
{
	int slot[MAX_ITEMS];
	places(slot);
	s_cur = 0;
	for (int i = 0; i < s_n; i++) {
		// 0x46c1a0: a twentieth of the way and 0.11 a frame; too far is there at once
		float t = (float)slot[i], d = t - s_ang[i];
		float st = d / 20.0f + (d > 0 ? 0.11f : -0.11f);
		s_ang[i] = fabsf(d) <= fabsf(st) ? t : s_ang[i] + st;
		if (fabsf(t - s_ang[i]) > 1.5f)
			s_ang[i] = t;
		if (t == 5.0f && s_ang[i] == t)
			s_turning = false;
		if (slot[i] == 5)
			s_cur = s_list[i];
		// 0x475230: the chosen one to 1 and the others to 0.8, a tenth a frame
		bool on = slot[i] == 5;
		s_fade[i] += on ? 0.1f : -0.1f;
		s_fade[i] = fminf(fmaxf(s_fade[i], 0.8f), 1.0f);
		// the equipped weapon's picture grows in up at the equipment: +0.4 -0.2 a frame up to 0.9
		s_eq_fade[i] += combat_weapon_item() == s_list[i] ? 0.2f : -0.2f;
		s_eq_fade[i] = fminf(fmaxf(s_eq_fade[i], 0.0f), 0.9f);
	}
	float r[4];
	frame_target(r);
	const float tx[4] = { r[0], r[0], r[2], r[2] }, ty[4] = { r[1], r[3], r[3], r[1] };
	memmove(s_trail[1], s_trail[0], sizeof s_trail[0] * 7);
	memcpy(s_trail[0], s_frame, sizeof s_frame);
	bool moved = false;
	for (int i = 0; i < 4; i++) {
		float x = ease(s_frame[i][0], tx[i]), y = ease(s_frame[i][1], ty[i]);
		moved |= x != s_frame[i][0] || y != s_frame[i][1];
		s_frame[i][0] = x, s_frame[i][1] = y;
	}
	// 0x475230: the lit heading's colour goes round 0..2 in 0.04 steps, from 0 while the frame moves
	s_flash = moved || s_flash >= 2.0f ? 0.0f : s_flash;
	s_flash += 0.04f;
	bool cmd_up = s_state == ST_CMD || s_state == ST_CMD_EQ;
	s_bar_on += cmd_up ? 6.0f : -6.0f;
	s_bar_on = fminf(fmaxf(s_bar_on, 0.0f), 40.0f);
	s_bar_row = ease(s_bar_row * 1000.0f, s_row * 1000.0f) / 1000.0f;
	for (int i = 0; i < 3; i++) {
		s_mark_a[i] += s_mark[i] ? 0.133f : -0.133f;     // 4 a second
		s_mark_a[i] = fminf(fmaxf(s_mark_a[i], 0.0f), 1.0f);
	}
	s_tint_t += 1.0f / 30.0f;
}

static bool any(u32 down, u32 bits) { return (down & bits) != 0; }
#define K_UP (KEY_DUP | KEY_CPAD_UP)
#define K_DOWN (KEY_DDOWN | KEY_CPAD_DOWN)
#define K_LEFT (KEY_DLEFT | KEY_CPAD_LEFT)
#define K_RIGHT (KEY_DRIGHT | KEY_CPAD_RIGHT)

static void go(int st)
{
	s_state = st;
	sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
}

// 0x471590: what the chosen command does
static void act(int it, int cmd)
{
	switch (cmd) {
	case CMD_EQUIP: case CMD_UNEQUIP:
		combat_equip_item(it);
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		s_state = ST_ITEMS;
		break;
	case CMD_RELOAD:
		if (s_host.reload)
			s_host.reload(it);
		s_state = ST_ITEMS;
		break;
	case CMD_POWER:
		if (s_host.power)
			s_host.power(it);
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		break;
	case CMD_EXAMINE:
		prog_score_examine(it);
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		s_state = ST_READ;
		break;
	case CMD_COMBINE:                      // 0x471590: up to three go in the marks along the bottom
		for (int i = 0; i < 3; i++)
			if (s_mark[i] == it) {
				s_mark[i] = 0;
				sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
				s_state = ST_ITEMS;
				return;
			}
		for (int i = 0; i < 3; i++)
			if (!s_mark[i]) {
				s_mark[i] = it;
				break;
			}
		sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
		s_state = ST_ITEMS;
		break;
	case CMD_VOLUME:
		sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		break;
	default:
		if (s_host.use) {
			int m[2] = { 0, 0 }, k = 0;
			for (int i = 0; i < 3 && k < 2; i++)
				if (s_mark[i] && s_mark[i] != it)
					m[k++] = s_mark[i];
			s_host.use(it, m[0], m[1]);
		}
		break;
	}
}

bool itemmenu_update(u32 down, u32 held)
{
	list_items();
	if (s_start >= s_n)
		s_start %= s_n;
	step();
	int it = s_cur, cmd[3], ctext;
	switch (s_state) {
	case ST_ITEMS:
		if (down & KEY_B || down & KEY_START) {
			sfx_play(SFX_MENU_CANCEL, 1.0f, 0.0f);
			if (s_host.leave)
				s_host.leave(NULL);
			return false;
		}
		if (any(down, K_DOWN)) {
			go(ST_MEMO);
			break;
		}
		if (any(down, K_UP) && combat_weapon_item()) {
			go(ST_EQUIP);
			break;
		}
		if ((down & KEY_A) && it && kind_of(it)) {
			s_row = 0;
			s_rows = commands(it, cmd, &ctext);
			go(ST_CMD);
			break;
		}
		// 0x474410: a press turns the ring, held it goes on after eight frames, once it has stopped
		s_hold = any(held, K_LEFT | K_RIGHT) ? s_hold + 1 : 0;
		if (!s_turning && s_n && (any(down, K_LEFT | K_RIGHT) || s_hold > 8)) {
			bool right = any(held | down, K_RIGHT);
			s_start = right ? (s_start + 1 == s_n ? 0 : s_start + 1) : (s_start == 0 ? s_n - 1 : s_start - 1);
			s_turning = true;
			sfx_play(SFX_TURN, 1.0f, 0.0f);
		}
		break;
	case ST_OPTION: case ST_MEMO: case ST_MAP:
		if (any(down, K_UP) || (down & KEY_B)) {
			go(ST_ITEMS);
			break;
		}
		if (any(down, K_LEFT) && s_state > ST_OPTION)
			go(s_state - 1);
		else if (any(down, K_RIGHT) && s_state < ST_MAP)
			go(s_state + 1);
		else if (down & KEY_A) {
			sfx_play(SFX_MENU_OK, 1.0f, 0.0f);
			if (s_host.leave)
				s_host.leave(s_state == ST_OPTION ? s_host.options : s_state == ST_MEMO ? s_host.memos : s_host.map);
			return false;
		}
		break;
	case ST_EQUIP:
		if (any(down, K_DOWN) || (down & KEY_B))
			go(ST_ITEMS);
		else if (down & KEY_A) {
			s_row = 0;
			s_rows = commands(combat_weapon_item(), cmd, &ctext);
			go(ST_CMD_EQ);
		}
		break;
	case ST_CMD: case ST_CMD_EQ: {
		int subject = s_state == ST_CMD_EQ ? combat_weapon_item() : it;
		s_rows = commands(subject, cmd, &ctext);
		if (down & KEY_B) {
			go(s_state == ST_CMD_EQ ? ST_EQUIP : ST_ITEMS);
			break;
		}
		if (any(down, K_UP) && s_row > 0)
			s_row--, sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		if (any(down, K_DOWN) && s_row + 1 < s_rows)
			s_row++, sfx_play(SFX_MENU_MOVE, 1.0f, 0.0f);
		if ((down & KEY_A) && s_rows) {
			int was = s_state;
			act(subject, cmd[s_row]);
			if (was == s_state && cmd[s_row] != CMD_POWER && cmd[s_row] != CMD_VOLUME)
				return false;
		}
		break;
	}
	case ST_READ:
		if (down & (KEY_A | KEY_B))
			go(ST_ITEMS);
		break;
	}
	return true;
}

static void flat(float x0, float y0, float x1, float y1, u32 c)
{
	const u32 k[4] = { c, c, c, c };
	font_quad_px(NULL, x0, y0, x1, y1, 0, 0, 0, 0, k);
}

// 0x475db0: x, y the middle, scale the picture's, the height half of 128 texels
static void sprite(int id, float x, float y, u8 a, float scale)
{
	if (!s_tab_ok || id <= 0)
		return;
	const s16* uv = NULL;
	float k = 1.0f, tex = 1024.0f;
	C3D_Tex* t = NULL;
	if (s_chapter == 1) {
		for (int i = 0; i < 12; i++)
			if (s_tab.xid[i] == id)
				uv = s_tab.xuv[i];
		t = s_sheet_ok[1] ? &s_sheet[1] : NULL, tex = 512.0f;
	}
	if (!uv) {
		for (int i = 0; i < 79; i++)
			if (s_tab.id[i] == id) {
				uv = s_tab.uv[i], k = s_tab.scale[i];
				break;
			}
		t = s_sheet_ok[0] ? &s_sheet[0] : NULL, tex = 1024.0f;
	}
	if (!uv || !t)
		return;
	// the layout is 1.5 x 1.0625 screen pixels a unit: the picture keeps the PC's 1.25 x 1 shape in it
	float hw = (uv[2] - uv[0]) * 0.5f * k * scale * (1.25f * 1.0625f / 1.5f), hh = 64.0f * k * scale;
	u32 c = 0xffffff00u | a;
	const u32 col[4] = { c, c, c, c };
	font_quad_px(t, X(x - hw), Y(y - hh), X(x + hw), Y(y + hh), uv[0] / tex, uv[1] / tex, uv[2] / tex, (uv[1] + 127) / tex, col);
}

// 0x46e150: a heading's bar, rows of a parabola either side of its line, one screen pixel apart.
// R 0x20, G u, B 3u, u falling away from the middle; the state's own one goes to white and back.
static void bars(void)
{
	const int n = 7;
	float t = s_flash >= 1.0f ? 2.0f - s_flash : s_flash;
	for (int side = -1; side <= 1; side += 2)
		for (int col = -90; col < 0x186; col += 160)
			for (int r = 0; r < n; r++) {
				float u = (float)(n - r) * 80.0f / n;
				bool lit = (s_state == ST_EQUIP && side < 0 && col == 70) || ((s_state == ST_CMD || s_state == ST_CMD_EQ) && side < 0 && col == 230) ||
				           (s_state == ST_OPTION && side > 0 && col == -90) || (s_state == ST_MEMO && side > 0 && col == 70) ||
				           (s_state == ST_MAP && side > 0 && col == 230);
				u32 c;
				if (lit)
					c = (u32)(u * 4 * t) << 24 | (u32)(u * 3 * t + u) << 16 | (u32)(u * t + u * 3) << 8 | 0xff;
				else
					c = 0x20000000u | (u32)u << 16 | (u32)(u * 3) << 8 | 0xff;
				float d = (float)(r * r) * 36.0f / (n * n * 10.0f) * 10.0f;
				float x0 = col - 140 + d, x1 = col - d;
				float py = 1.0f / (544.0f / 240.0f);
				float y = Y(side * 189.0f);
				flat(X(x0), y + r * py, X(x1), y + (r + 1) * py, c);
				if (r)
					flat(X(x0), y - r * py, X(x1), y - (r - 1) * py, c);
			}
}

// 0x46e700: the bar behind the chosen command
static void command_bar(void)
{
	if (s_bar_on <= 0.0f)
		return;
	float base = -133.0f - 10.0f * (s_rows > 0 ? s_rows - 1 : 0) + s_bar_row * 26.0f;
	const int n = 7;
	for (int r = 0; r < n; r++) {
		float d = (float)(r * r) * 36.0f / (n * n * 10.0f) * 10.0f;
		int k = (n - r) * (int)s_bar_on;
		u32 c = 0x40000000u | (u32)(k / 2 > 255 ? 255 : k / 2) << 16 | (u32)(k / 4 > 255 ? 255 : k / 4) << 8 | 0xff;
		float py = 240.0f / 544.0f, y = Y(base);
		flat(X(90 + d), y + r * py, X(230 - d), y + (r + 1) * py, c);
		if (r)
			flat(X(90 + d), y - r * py, X(230 - d), y - (r - 1) * py, c);
	}
}

static void line(float x0, float y0, float x1, float y1, u32 c)
{
	float w = 1.0f / 2.4f, h = 240.0f / 544.0f;
	float ax = X(x0), ay = Y(y0), bx = X(x1), by = Y(y1);
	if (ax > bx) { float t = ax; ax = bx, bx = t; }
	if (ay > by) { float t = ay; ay = by, by = t; }
	if (ax == bx)
		flat(ax, ay, ax + w, by + h, c);
	else
		flat(ax, ay, bx + w, ay + h, c);
}

// 0x46cb50: the frame in 0x3060f0, and with the frame still its two corners 6 out at the top left and
// the bottom right. 0x470c60 fills the eight places it has just been, fainter each.
static void frame(void)
{
	bool moving = memcmp(s_frame, s_trail[0], sizeof s_frame) != 0;
	const u32 blue = 0x3060f0ffu;
	float x0 = s_frame[0][0], y0 = s_frame[0][1], y1 = s_frame[1][1], x1 = s_frame[2][0];
	if (moving || memcmp(s_trail[0], s_trail[7], sizeof s_trail[0])) {
		for (int i = 0; i < 7; i++) {
			u8 a = (u8)((1.0f - i * 0.142857f) * 20.0f);
			float* c = s_trail[i][0], *d = s_trail[i + 1][0];
			const u32 k[4] = { 0x3060f000u | a, 0x3060f000u | a, 0x3060f000u | a, 0x3060f000u | a };
			font_quad_px(NULL, X(fminf(c[0], d[0])), Y(fminf(c[1], d[1])), X(fmaxf(s_trail[i][2][0], s_trail[i + 1][2][0])),
			             Y(fmaxf(s_trail[i][1][1], s_trail[i + 1][1][1])), 0, 0, 0, 0, k);
		}
	}
	line(x0, y0, x1, y0, blue);
	line(x1, y0, x1, y1, blue);
	line(x0, y1, x1, y1, blue);
	line(x0, y0, x0, y1, blue);
	if (!moving && !s_turning) {
		float mx = (x1 - x0) * 0.5f, my = (y1 - y0) * 0.5f;
		line(x0 - 6, y0 - 6, x0 + mx, y0 - 6, blue);
		line(x0 - 6, y0 - 6, x0 - 6, y1 - my, blue);
		line(x1 - mx, y1 + 5, x1 + 6, y1 + 5, blue);
		line(x1 + 6, y0 + my, x1 + 6, y1 + 5, blue);
	}
}

// 0x46f180: the panel's tint and the noise that grows as James is hurt, both red while he is below 0.7
static void status(void)
{
	float hp = combat_hp(), max = combat_hp_max(), f = max > 0.0f ? hp / max : 0.0f;
	f = fminf(fmaxf(f, 0.0f), 1.0f);
	if (f < 0.7f) {
		float beat = 0.5f + 0.5f * sinf(s_tint_t * (6.2831853f * (1.4f + (1.0f - f) * 1.4286f) / 4.0f));
		u32 c = 0xb0000000u | (u32)(beat * (1.0f - f) * 160.0f);
		const u32 k[4] = { c, c, c, c };
		font_quad_px(NULL, X(-230), Y(-175), X(-90), Y(-70), 0, 0, 0, 0, k);   // over the still
	}
	int count = (int)(10.0f - f * 10.0f);
	srand((unsigned)s_seed++);
	for (int i = 0; i < count; i++) {
		int a = rand() % 140, b = rand() % 105, len = rand() % 105;
		if (rand() % 6 >= 1)
			continue;
		int x = a - 230, y = b - 173;
		if (x + len > -90)
			len = -90 - x;
		flat(X((float)x), Y((float)y), X((float)(x + len)), Y((float)y + 4.0f), 0xd01818c0u);
	}
}

// 0x470570: the marked items along the bottom in their three boxes
static void marks(void)
{
	for (int i = 0; i < 3; i++) {
		float x = (float)(100 + 50 * i);
		u8 a = (u8)(s_mark_a[i] * 255.0f);
		if (!a)
			continue;
		flat(X(x), Y(45), X(x + 40), Y(95), 0x20402000u | a / 2);
		line(x, 45, x + 40, 45, 0x408040ffu);
		line(x + 40, 45, x + 40, 95, 0x408040ffu);
		line(x, 95, x + 40, 95, 0x408040ffu);
		line(x, 45, x, 95, 0x408040ffu);
		if (s_mark[i])
			sprite(s_mark[i], x + 20, 67.0f, 0xff, 0.4f);
	}
}

static void shaded(int f, float x, float y, const char* s, int al, u32 c)
{
	font_text(f, x + 2, y + 2, s, al, 0x404040ffu, 0x404040ffu);
	font_text(f, x, y, s, al, c, c);
}

void itemmenu_draw(void)
{
	const u32 black = 0x000000ffu, full = 0xfefefeffu, dim = 0x404040ffu;
	float px0, py0, px1, py1;
	itemmenu_panel(&px0, &py0, &px1, &py1);
	flat(0, 0, 400, 240, black);
	extern void vita_capture_draw(float x0, float y0, float x1, float y1);
	vita_capture_draw(px0, py0, px1, py1);       // 0x46f180: the still of the frame
	status();
	bars();
	command_bar();
	// the ring behind the frame: scale 0.3 + 0.6 cos and alpha from the depth, by 0x46c1a0's circle of
	// 1500 wide by 1000 deep, times 2.5 across, in sixteenths of a unit
	int order[MAX_ITEMS], n = 0, slot[MAX_ITEMS];
	places(slot);
	for (int i = 0; i < s_n; i++)
		if (slot[i])
			order[n++] = i;
	// far ones first
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && fabsf(s_ang[order[j]] - 5.0f) > fabsf(s_ang[order[j - 1]] - 5.0f); j--) {
			int t = order[j];
			order[j] = order[j - 1], order[j - 1] = t;
		}
	for (int k = 0; k < n; k++) {
		int i = order[k];
		float a = (s_ang[i] - 5.0f) * 0.392699f, sn = sinf(a), cs = cosf(a);
		float xu = sn * 1500.0f * 2.5f / 16.0f, z = cs * -1000.0f;
		float al = (-z - 300.0f) * 0.181818f;
		al = al < 0.0f ? 0.0f : al > 127.0f ? 127.0f : al;
		float scale = (0.3f - z * 0.00060024f) * s_fade[i];
		sprite(s_list[i], xu, 0.0f, (u8)(al * 2.0f), scale);
	}
	// the equipped weapon at the heading
	for (int i = 0; i < s_n; i++)
		if (s_eq_fade[i] > 0.0f)
			sprite(s_list[i], 0.0f, -134.0f, 0xff, s_eq_fade[i]);
	marks();
	frame();
	// 0x472e80's text. The headings and the bottom row, centred
	font_text(FONT_BIG, 0x60, 0x32, ev_item_message(12), FONT_CENTER, full, full);
	font_text(FONT_BIG, 0x100, 0x32, ev_item_message(13), FONT_CENTER, full, full);
	font_text(FONT_BIG, 0x1a0, 0x32, ev_item_message(14), FONT_CENTER, full, full);
	font_text(FONT_BIG, 0x60, 0x1ac, ev_item_message(250), FONT_CENTER, full, full);
	font_text(FONT_BIG, 0x100, 0x1ac, ev_item_message(11), FONT_CENTER, full, full);
	font_text(FONT_BIG, 0x1a0, 0x1ac, ev_item_message(10), FONT_CENTER, full, full);
	int subject = s_state == ST_EQUIP || s_state == ST_CMD_EQ ? combat_weapon_item() : s_cur;
	int cmd[3], ctext = 0x69, nc = subject ? commands(subject, cmd, &ctext) : 0;
	bool lit = s_state == ST_CMD || s_state == ST_CMD_EQ;
	for (int i = 0; i < nc; i++) {
		int x = cmd[i] == CMD_POWER || cmd[i] == CMD_VOLUME ? 0x15e : 0x168;
		u32 c = lit ? full : dim;
		font_text(FONT_BIG, x, ctext + (nc == 3 ? 0x19 : 0x19) * i, ev_item_message(cmd[i]), FONT_LEFT, c, c);
	}
	if (!subject)
		return;
	// the rounds, between two dashes, on what holds them
	int k = kind_of(subject);
	if ((k == 2 || k == 4 || k == 5) && ev_item_count(subject) >= 0) {
		char num[8];
		snprintf(num, sizeof num, "%d", ev_item_count(subject));
		font_text(FONT_BIG, 0x100 + 0x1c, 0x121, num, FONT_CENTER, full, full);
		float w = font_width(FONT_BIG, num) * 0.5f;
		flat(X(0x1c - w - 20), Y(0x121 - 256 + 18), X(0x1c - w - 6), Y(0x121 - 256 + 20), full);
		flat(X(0x1c + w + 6), Y(0x121 - 256 + 18), X(0x1c + w + 20), Y(0x121 - 256 + 20), full);
	}
	if (subject == 18 || subject == 19) {
		bool on = s_host.power_on && s_host.power_on(subject);
		font_text(FONT_BIG, 0x130, 0x122, ev_item_message(on ? CMD_ON : CMD_OFF), FONT_RIGHT, full, full);
	}
	char name[64];
	const char* nm = ev_item_name(subject);
	snprintf(name, sizeof name, "%.*s", nm ? (int)strcspn(nm, "\n") : 0, nm ? nm : "");
	shaded(FONT_BIG, 0x14, 0x145, name, FONT_LEFT, full);
	const char* d = ev_item_desc(subject);
	if (d) {
		char text[400];
		snprintf(text, sizeof text, "%s", d);
		for (char* c = text; *c; c++)
			if (*c == '\f')
				*c = '\n';
		shaded(FONT_BIG, 0x2d, 0x168, text, FONT_LEFT, full);
	}
}
