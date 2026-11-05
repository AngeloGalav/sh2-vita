#include "radio.h"
#include "enemy.h"
#include "events.h"
#include "sfx.h"
#include <math.h>

#define MAX_EN 32                      // 0x1f7daa0: 32 enemy records

// 0x88cce0: per enemy kind, each layer's level and (0x88ccf0) the distance it reaches. Kinds 7, 9, 11
// and 12 have no row in the PC's table either.
static const float s_level[11][4] = {
	{ 0 }, { 1.0f, 1.0f, 0.6f, 0.8f }, { 0.9f, 0.8f, 1.0f, 0.5f }, { 0.5f, 0.2f, 0.5f, 0.0f },
	{ 0.5f, 0.5f, 0.5f, 1.0f }, { 0.3f, 0.3f, 0.3f, 1.0f }, { 1.0f, 0.8f, 1.0f, 0.0f }, { 0 },
	{ 0.8f, 1.0f, 0.8f, 0.8f }, { 0 }, { 0.4f, 0.8f, 1.0f, 0.5f },
};
static const float s_reach[11][4] = {
	{ 0 }, { 12500, 5000, 1250, 7500 }, { 14000, 6000, 1500, 5000 }, { 7500, 1500, 2500, 0 },
	{ 10000, 9000, 8000, 10000 }, { 10000, 10000, 10000, 10000 }, { 8000, 6000, 4000, 0 }, { 0 },
	{ 15000, 7500, 2500, 7500 }, { 0 }, { 5000, 3500, 1500, 5000 },
};

static const float s_floor_pos[3] = { 47716.9f, -268.4f, 49627.9f };    // 0x59e1e0: DAT_01f7d5cc

static int s_mode;                     // DAT_01f7d584
static bool s_switch = true;           // DAT_01f7d407
static int s_volume = 10;              // DAT_01f7d408
static bool s_playing;                 // DAT_01f7d580
static float s_lvl[4], s_pan[4];       // DAT_01f7d588, DAT_01f7d5a8
static float s_master;                 // DAT_01f7d5c8

void radio_set_mode(int mode) { s_mode = mode; }

void radio_reset(int volume)
{
	s_switch = true;
	s_volume = volume;
	s_mode = 0;
}

void radio_stage_loaded(int stage)
{
	if (stage == 5 && !ev_flag(43))    // 0x59e1e0: until 0x800 of the second flag word
		s_mode = 1;
	else if (stage == 46 && s_mode) {  // 0x575b00, Stg_hotel_3f_b's record
		s_mode = 0;
		sfx_stop(RADIO_SE);
		sfx_stop(RADIO_SE + 1);
	}
}

bool radio_on(void) { return s_switch; }
void radio_toggle(void) { s_switch = !s_switch; }
int radio_volume(void) { return s_volume; }
void radio_set_volume(int v) { s_volume = v < 0 ? 0 : v > 15 ? 15 : v; }

static void silence(void)
{
	for (int i = 0; i < 4; i++)
		sfx_stop(RADIO_SE + i);
}

static float toward(float v, float goal, float step)
{
	return v < goal ? (v + step > goal ? goal : v + step) : (v - step < goal ? goal : v - step);
}

// 0x4a6ce0, the record's +0x94. Stand-in for two of its tests: the character's +0x148 bits 0 and 2
// are read here as "alive", so the PC's 0.8 for a body in its death throes is not kept.
static float enemy_factor(const Enemy* e)
{
	if (!enemy_alive(e))
		return 0.0f;
	if (e->kind == EN_KIND_TYU && e->type == 0)
		return 1.0f;
	if (e->kind == EN_KIND_ONI || e->kind == EN_KIND_ARM || e->kind == EN_KIND_TYU ||
	    e->kind == EN_KIND_RED || e->kind == EN_KIND_IKE)
		return 0.0f;
	if (e->kind == EN_KIND_NSE && e->type != 6)
		return 0.5f;
	return 1.0f;
}

// 0x48cbc0's default mode: every enemy in reach adds to the layers, the strongest counting in full
// and each next one at a third of the one before.
static void from_enemies(float dt)
{
	static float vol[MAX_EN][4], pan[MAX_EN][4];
	const Enemy* list[MAX_EN];
	int n = 0, weight = 0, count = enemy_count();
	for (int i = 0; i < count && n < MAX_EN; i++) {
		const Enemy* e = enemy_get(i);
		if (!e || !e->used || (int)e->kind > 10)
			continue;
		const float* r = s_reach[e->kind];
		if (enemy_factor(e) == 0.0f)
			continue;
		if (e->p_dist > r[0] && e->p_dist > r[1] && e->p_dist > r[2] && e->p_dist > r[3])
			continue;
		if (e->p_dist <= r[3]) {
			switch (e->kind) {
			case 1: case 2: case 6: weight += 3; break;
			case 3: weight += 1; break;
			case 4: case 5: weight += 9; break;
			case 8: case 10: weight += 6; break;
			default: break;
			}
		}
		list[n++] = e;
	}
	for (int i = 0; i < n; i++) {
		const Enemy* e = list[i];
		const float pos[3] = { e->x, e->y, e->z };
		float f = enemy_factor(e), p = sfx_pan_at(pos);
		for (int l = 0; l < 4; l++) {
			float reach = s_reach[e->kind][l];
			vol[i][l] = pan[i][l] = 0.0f;
			if ((l == 3 && weight < 9) || e->p_dist > reach)
				continue;
			// 0x48cbc0 tests the distance against 1.5: it is 1 when closer, a quarter cosine past it
			float c = e->p_dist < 1.5f ? 1.0f : cosf((e->p_dist - 1.5f) / (reach - 1.5f) * 1.5708f);
			vol[i][l] = s_level[e->kind][l] * f * c;
			pan[i][l] = p;
		}
	}
	for (int l = 0; l < 4; l++) {
		// the PC sorts each layer by level, swapping the pans with it
		for (int a = 0; a < n - 1; a++)
			for (int b = a + 1; b < n; b++)
				if (vol[a][l] < vol[b][l]) {
					float t = vol[a][l]; vol[a][l] = vol[b][l]; vol[b][l] = t;
					t = pan[a][l]; pan[a][l] = pan[b][l]; pan[b][l] = t;
				}
		float v = n ? vol[0][l] : 0.0f, p = n ? pan[0][l] : 0.0f, third = 1.0f;
		for (int j = 1; j < n; j++) {
			third *= 0.33333f;
			float add = vol[j][l] * third;
			if (j >= 3 && add < 0.1f)
				break;
			v += add;
			p += pan[j][l] * third;
		}
		v = v > 1.0f ? 1.0f : v;
		p = p > 1.0f ? 1.0f : p < -1.0f ? -1.0f : p;
		s_lvl[l] = toward(s_lvl[l], v, dt * 0.4f);
		s_pan[l] = toward(s_pan[l], p, dt * 0.5f);
	}
}

// 0x48cbc0 ends each mode with 0x514400(channel, level, pan). 0x518320 doubles the level and 0x518140
// sends it to SetVolume through the table at 0x8a8bb8, 20 log10(i / 255) dB: a linear gain of level * 2
// of 255. Its pan, 64 - 32 * pan, is half of the 0..127 range.
static void channel(int i, float level)
{
	float g = level * 2.0f / 255.0f;
	if (g < 0.004f) {
		sfx_stop(RADIO_SE + i);
		return;
	}
	sfx_mix(RADIO_SE + i, g, s_pan[i] * 0.5f);
}

void radio_update(RadioScene scene, float dt)
{
	if (scene == RADIO_SILENT || (!s_switch && s_mode != 3) || (s_mode == 0 && !ev_have_item(19))) {
		if (s_playing) {
			silence();
			s_playing = false;
		}
		return;
	}
	if (!s_playing) {
		s_lvl[0] = s_lvl[1] = s_lvl[2] = s_lvl[3] = 0.0f;
		s_master = 0.0f;
		s_playing = true;
	}
	float goal = scene == RADIO_ITEMS ? 0.5f : scene == RADIO_PAUSE ? 0.25f : 1.0f;
	float by_switch = (float)s_volume * 0.0666667f;
	int layers = 2;
	switch (s_mode) {
	case 1: {
		float f = 0.8f * sfx_distance_at(s_floor_pos);
		s_lvl[0] = f;       // 0x88cd00, 0x88cd04: 1.0 each
		s_lvl[1] = f;
		s_pan[0] = sfx_pan_at(s_floor_pos);
		s_master = toward(s_master, goal, dt * 0.5f);
		by_switch = 1.0f;
		break;
	}
	case 2:
		for (int i = 0; i < 2; i++) {
			s_lvl[i] = toward(s_lvl[i], 0.4f, dt * 0.4f);
			s_pan[i] = toward(s_pan[i], 0.0f, dt * 0.5f);
		}
		s_master = 1.0f;
		by_switch = 1.0f;
		break;
	case 3:
		s_master = toward(s_master, goal, dt * 0.5f);
		for (int i = 0; i < 4; i++)
			s_lvl[i] = toward(s_lvl[i], 1.0f, dt * 0.2f);
		s_pan[0] = s_pan[1] = 0.0f;
		break;
	default:
		from_enemies(dt);
		s_master = toward(s_master, goal, dt * 0.5f);
		layers = 4;
		break;
	}
	for (int i = 0; i < layers; i++)
		channel(i, s_lvl[i] * s_master * by_switch * 127.0f);
}
