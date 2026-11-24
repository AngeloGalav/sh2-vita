#include "stageenv.h"
#include "events.h"
#include "prog.h"
#include <math.h>
#include <string.h>

// PC references (Director's Cut): the lists at Stage_Data +0x34; entry 1 is the function here, entry 0
// stage 51's 0x5750a0, entry 2 the 2D filters (main.c filters_frame).
// Not ported: the reflection light (0x47ad10, a specular light the port's characters have none of), the
// event spot of stage 44's corridor and of the candle in stage 56 (0x47b040, 0x47ded0), stage 12's lamp in
// room 0x9000a (0x4457b0), and the records' own clip planes past what the fog leaves to see (below).

static bool f(int n) { return ev_flag(n); }

static void light(StageLights* l, int k, const float dir[3], const float col[3], bool chars)
{
	float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	l->set[k] = true, l->chars[k] = chars;
	for (int i = 0; i < 3; i++)
		l->dir[k][i] = n > 1e-6f ? dir[i] / n : dir[i], l->col[k][i] = col[i];
}

// Port's own: the far plane only ever moves out (the records' have the Enhanced Edition's), and only for a
// fog that reaches past the record's: beyond the fog it would draw what is covered already.
static void fog(RoomEnv* e, float end, float start, float far)
{
	if (far > e->clip_far && end > e->fog_end + 1500.0f)
		e->clip_far = far;
	e->fog_end = end, e->fog_start = start;
}

static void gray_fog(RoomEnv* e, float r, float g, float b)
{
	e->fog_rgb[0] = e->clear_rgb[0] = floorf(r);
	e->fog_rgb[1] = e->clear_rgb[1] = floorf(g);
	e->fog_rgb[2] = e->clear_rgb[2] = floorf(b);
}

// ---------------------------------------------------------------- the light that follows the camera

// 0x5837c0, 0x580930, 0x581990 and 0x596890: a direction that swings after the camera's, a step a frame of
// 0.1 / (2 + the cosine between them), with 1 added to its y so that it comes from above.
static float s_follow[3] = { 1.0f, 0.0f, 0.0f };

static void follow_step(const float look[3], float dt)
{
	static float left;
	for (left += dt; left >= 1.0f / 30.0f; left -= 1.0f / 30.0f) {
		const float *a = look, *b = s_follow;
		float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		float p[3], n = 0.0f;                 // b x (a x b) with the inner product made a unit vector
		for (int i = 0; i < 3; i++)
			p[i] = a[i] - b[i] * c, n += p[i] * p[i];
		n = n > 1e-10f ? 1.0f / sqrtf(n) : 0.0f;
		float k = 0.1f / (c + 2.0f), m = 0.0f, o[3];
		for (int i = 0; i < 3; i++) {
			o[i] = b[i] + p[i] * n * k + (i == 1 ? 1.0f : 0.0f);
			m += o[i] * o[i];
		}
		m = m > 1e-10f ? 1.0f / sqrtf(m) : 1.0f;
		for (int i = 0; i < 3; i++)
			s_follow[i] = o[i] * m;
	}
}

// ---------------------------------------------------------------- the town west (stages 14 and 16)

// 0x595170 and the same part of 0x5914a0: p goes from 0 to 1 in the 300 seconds after James meets Maria
// (0x1f7d9dc, set by EvProgMariaMeeting), and the grey day turns to a low red sun.
static void dusk(RoomEnv* e, StageLights* l, float p)
{
	static const float amb[3] = { 0.6f, 0.55f, 0.5f };                                   // 0x8e127c
	static const float d0[3] = { 0.2509f, 0.239f, 0.938966f }, c0[3] = { 0.45f, 0.45f, 0.4f };   // 0x8e11ec
	static const float d1[3] = { -0.738f, 0.559f, 0.37618f }, c1[3] = { 0.8f, 0.4f, 0.35f };     // 0x8e11cc
	static const float fp[4] = { 133.0f, 117.0f, 101.0f, 154.0f }, fc[3] = { 89.6f, 76.8f, 64.0f };
	fog(e, 5525.0f, 1675.0f, 7500.0f);
	e->fog_far_f = 23.0f, e->fog_near_f = 240.0f;
	float dir[3], col[3], rgb[3];
	for (int i = 0; i < 3; i++) {
		e->amb[i] = 0.65f - (amb[i] - 0.65f) * (p - 1.0f);
		dir[i] = d0[i] + (d1[i] - d0[i]) * p;
		col[i] = c0[i] + (c1[i] - c0[i]) * p;
		rgb[i] = 125.0f + (fc[i] - 125.0f) * p;
	}
	light(l, 1, dir, col, false);
	gray_fog(e, rgb[0], rgb[1], rgb[2]);
	l->fogp_set = true;
	for (int i = 0; i < 4; i++)
		l->fogp[i] = (int)(128.0f + (fp[i] - 128.0f) * p);
}

static float dusk_share(void)
{
	return f(162) ? 1.0f : f(152) ? prog_dusk() : 0.0f;
}

static const float k_w_d0[3] = { 0.2509f, 0.239f, 0.938966f }, k_w_c0[3] = { 0.45f, 0.45f, 0.4f };
static const float k_w_d1[3] = { -0.738f, 0.559f, 0.37618f }, k_w_c1[3] = { 0.8f, 0.4f, 0.35f };

static void town_west(RoomEnv* e, StageLights* l, const StageEnvIn* in)      // 0x5954b0
{
	float p = dusk_share(), t = in->demo_t;
	if (in->demo < 0) {
		if (in->james[0] >= 21000.0f && in->james[0] <= 25000.0f && !f(150)) {
			// (past 45500 the PC leaves the fog as it was: the record is only read when the room changes)
			float z = fminf(in->james[2] - 40000.0f, 5500.0f);
			if (z >= 0.0f)
				fog(e, 5525.0f + (6000.0f - 5525.0f) * z / 5500.0f, 1675.0f + (3500.0f - 1675.0f) * z / 5500.0f, 0.0f);
			light(l, 1, k_w_d0, k_w_c0, false);
		} else if (!f(251)) {
			dusk(e, l, p);
		}
		return;
	}
	switch (in->demo) {
	case 0x14:
		return;
	case 0x15: {                         // 0x5945d0: by the cuts; from frame 294 at least the third shot
		if (in->eye[0] > 12000.0f || in->james[0] > 12000.0f)
			return;
		int shot = in->demo_cuts;
		shot = t >= 294.0f ? (shot < 2 ? 2 : shot) : (shot > 1 ? 1 : shot);
		e->fog_far_f = 0.0f;
		if (shot < 2)
			fog(e, 10000.0f, 3000.0f, 10000.0f);
		else if (shot == 2)
			fog(e, 15000.0f, 3000.0f, 15000.0f);
		else if (shot == 3)
			fog(e, 17000.0f, 1000.0f, 17000.0f);
		else
			fog(e, 12000.0f, 2000.0f, 17000.0f);
		light(l, 1, k_w_d0, k_w_c0, false);
		return;
	}
	case 0x16:                           // 0x594750; 965.5 and 1460 are the Enhanced Edition's (968, 1463)
		e->fog_far_f = 0.0f;
		if (t >= 965.5f && t < 1460.0f)
			fog(e, 10000.0f, 1250.0f, 17000.0f);
		else
			fog(e, t < 965.5f ? 12000.0f : 15000.0f, 2000.0f, 17000.0f);
		light(l, 1, k_w_d0, k_w_c0, false);
		return;
	case 0x17:
		dusk(e, l, p);
		e->fog_far_f = 0.0f, e->fog_near_f = 255.0f;
		fog(e, 7000.0f, 2500.0f, 0.0f);
		return;
	case 0x18: {
		static const float d[3] = { -0.177f, 0.266f, 0.947f };       // 0x8e120c
		float col[3];
		dusk(e, l, p);
		for (int i = 0; i < 3; i++)
			col[i] = ((k_w_c1[i] - 0.001f) * p + 0.001f - 0.4f) * p + 0.4f;
		light(l, 2, d, col, false);
		return;
	}
	case 0x1b: {
		static const float d[3] = { -0.505f, 0.168f, -0.846f }, col[3] = { 0.85f, 0.2f, 0.2f };
		dusk(e, l, p);
		if (t >= 64.0f)
			e->fog_far_f = 80.0f, e->fog_near_f = 255.0f;
		light(l, 2, d, col, false);
		return;
	}
	case 0x1c: {
		static const float d[3] = { -0.935f, 0.35f, -0.0438f }, col[3] = { 0.5f, 0.2f, 0.2f };
		dusk(e, l, p);
		light(l, 2, d, col, false);
		light(l, 1, k_w_d1, k_w_c1, false);
		return;
	}
	case 0x20:
		fog(e, 7000.0f, 2000.0f, 0.0f);
		light(l, 1, k_w_d1, k_w_c1, false);
		return;
	case 0x5c:
		light(l, 1, k_w_d0, k_w_c0, false);
		fog(e, 5525.0f, 1675.0f, 0.0f);
		e->fog_far_f = 23.0f, e->fog_near_f = 240.0f;
		gray_fog(e, 125.0f, 125.0f, 125.0f);
		e->amb[0] = e->amb[1] = e->amb[2] = 0.65f;
		return;
	default:
		light(l, 1, k_w_d1, k_w_c1, false);
	}
}

// ---------------------------------------------------------------- the burning stairs (stage 47)

// 0x575840: two lights whose directions swing about a middle, each axis with a spring of its own
static void fire_lights(StageLights* l, float dt)
{
	static const float mid[2][3] = { { -0.8f, 0.2f, 0.25f }, { 0.8f, -0.1f, 0.3f } };
	static const float k[2][3] = { { 70.0f, 40.5f, 70.0f }, { 85.0f, 90.0f, 40.0f } };
	static const float col[2][3] = { { 0.6f, 0.4f, 0.3f }, { 0.5f, 0.3f, 0.1f } };
	static float at[2][3] = { { -0.6f, 0.35f, -0.1f }, { 0.55f, 0.4f, -0.05f } };
	static float was[2][3] = { { -0.6f, 0.35f, -0.1f }, { 0.55f, 0.4f, -0.05f } }, left;
	for (left += dt; left >= 1.0f / 30.0f; left -= 1.0f / 30.0f)
		for (int i = 0; i < 2; i++)
			for (int a = 0; a < 3; a++) {
				float n = at[i][a] * 2.0f - was[i][a] - (at[i][a] - mid[i][a]) * k[i][a] * 0.000556f;
				was[i][a] = at[i][a], at[i][a] = n;
			}
	light(l, 1, at[0], col[0], false);
	light(l, 2, at[1], col[1], false);
}

// ---------------------------------------------------------------- by stage

void stageenv_frame(RoomEnv* e, StageLights* l, const StageEnvIn* in)
{
	memset(l, 0, sizeof *l);
	l->points = -1;
	int dn = in->demo;
	float t = in->demo_t;
	follow_step(in->look, in->dt);
	switch (in->stage) {
	case 3:                                  // 0x5a1590
		if (dn == 0x5d) {
			static const float d[3] = { -0.513f, 0.814f, -0.272283f }, c[3] = { 1.25f, 1.25f, 1.25f };
			gray_fog(e, 120.0f, 120.0f, 130.0f);
			l->fogp_set = true;
			l->fogp[0] = l->fogp[1] = l->fogp[2] = l->fogp[3] = 128;
			e->fog_near_f = 255.0f, e->fog_far_f = 100.0f;
			fog(e, 53000.0f, e->fog_start, 53000.0f);
			e->clip_near = 5000.0f;
			light(l, 1, d, c, false);
		}
		break;
	case 4:                                  // 0x5a05a0
		if (dn == 3 || dn == 4)
			break;
		if (dn == 0x5a) {
			static const float d[3] = { -0.46f, 0.478f, 0.748f }, c[3] = { 1.85f, 1.2f, 0.95f };
			fog(e, 29000.0f, 3800.0f, 30000.0f);
			e->amb[0] = 1.0f, e->amb[1] = 0.9f, e->amb[2] = 0.8f;
			gray_fog(e, 105.0f, 93.0f, 90.0f);
			light(l, 1, d, c, false);
			l->fogp_set = true;
			l->fogp[0] = 133, l->fogp[1] = 117, l->fogp[2] = 101, l->fogp[3] = 200;
		} else {                             // 0x5a0480
			static const float d[3] = { -0.93f, 0.116f, -0.349f }, c[3] = { 0.25f, 0.2f, 0.2f };
			// RadioNoise (0x48c9d0) 2: before Angela (36), in the forest's first part: the fog opens towards
			// the middle of it
			if (ev_glb_crd() == 1 && !f(36) && in->james[0] > -20000.0f && in->james[2] > -20000.0f) {
				float dist = sqrtf(in->eye[0] * in->eye[0] + in->eye[1] * in->eye[1] + in->eye[2] * in->eye[2]);
				float k = 1.0f - fminf(dist, 10000.0f) * 0.0001f;
				fog(e, 5500.0f + (17635.0f - 5500.0f) * k, 3000.0f + 7000.0f * k, 9500.0f + 10500.0f * k);
				e->fog_far_f = 20.0f + k, e->fog_near_f = 235.0f - k;
			} else {
				fog(e, 5500.0f, 3000.0f, 9500.0f);
				e->fog_far_f = 20.0f, e->fog_near_f = 235.0f;
			}
			light(l, 1, d, c, false);
		}
		break;
	case 5: {                                // 0x59ec80
		static const float d[3] = { -0.948391f, 0.1168f, 0.2948f }, c[3] = { 0.45f, 0.45f, 0.4f };
		if (!f(251))
			light(l, 1, d, c, false);
		if (dn == 5) {
			float k = fminf(t, 115.0f);
			fog(e, 5500.0f + 12.173913f * k, 3000.0f + 4.347826f * k, 0.0f);
		} else if (dn >= 6 && dn <= 8) {
			if (in->eye[2] < 50000.0f && in->eye[0] <= 48000.0f)
				fog(e, 6500.0f, 6000.0f, 12000.0f);
			else
				fog(e, 5500.0f, 3000.0f, 7500.0f);
		} else if (ev_glb_crd() == 2 && !f(43)) {
			float x = in->eye[0] - 1750.0f, y = in->eye[1] + 1378.0f, z = in->eye[2] + 38222.0f;
			float q = fminf(sqrtf(x * x + y * y + z * z), 3500.0f);
			fog(e, 6900.0f - 0.4f * q, 3500.0f - q / 7.0f, 8100.0f);
		} else if (!f(251)) {
			fog(e, 5500.0f, 3000.0f, 7500.0f);
		}
		break;
	}
	case 12:                                 // 0x596890
		if (e->map_id == 0x9000a) {
			e->clip_near = 50.0f;
		} else if (e->map_id == 0x90058 && dn != 0x13) {
			static const float c[3] = { 0.3f, 0.3f, 0.3f };
			light(l, 1, s_follow, c, false);
		}
		break;
	case 14:
		town_west(e, l, in);
		break;
	case 16:                                 // 0x5914a0
		if (!f(251))
			dusk(e, l, dusk_share());
		break;
	case 27:                                 // 0x588130
		if (dn >= 0x30 && dn <= 0x32)
			e->clip_near = 1.0f;
		break;
	case 34: case 35: case 36: {             // 0x5837c0, 0x581990, 0x580930: on the characters only
		static const float c[3] = { 0.6f, 0.6f, 0.6f };
		int m = e->map_id;
		if (in->stage == 34 ? m == 0xb0059 : in->stage == 36 ? m == 0xb0089
		    : dn < 0 && (m == 0xb00ad || m == 0xb00b1 || m == 0xb005d || m == 0xb00b5))
			light(l, 2, s_follow, c, true);
		break;
	}
	case 37:                                 // 0x57f5b0
		e->clip_near = dn == 0x48 ? 10.0f : 100.0f;
		break;
	case 38:                                 // 0x57e930
		if (dn == 0x4d)
			fog(e, t < 1350.0f ? 20000.0f : 30000.0f, e->fog_start, t < 1350.0f ? 20000.0f : 35000.0f);
		else if (dn < 0)
			e->fog_end = 5500.0f;
		else if (dn == 0x61)
			fog(e, 50000.0f, 5000.0f, 50000.0f);
		break;
	case 44:                                 // 0x5771a0
		if (e->map_id == 0xd0025) {
			static const float v[2][4] = { { 5.4f, 6.45f, 0.675f, 0.45f }, { 22.4f, 0.045f, 0.0f, 0.0f } };
			if (in->eye[0] > -23000.0f) {        // 0x8cb29c
				for (int i = 0; i < 3; i++)
					e->bcol[i] = v[0][0], e->bamb[i] = v[0][1], e->vamb[i] = v[0][2], e->amb[i] = v[0][3];
			} else {                             // 0x8cb25c
				static const float va[3] = { 4.925f, 4.0f, 4.0f }, am[3] = { 0.775f, 0.625f, 0.625f };
				for (int i = 0; i < 3; i++)
					e->bcol[i] = v[1][0], e->bamb[i] = v[1][1], e->vamb[i] = va[i], e->amb[i] = am[i];
			}
		} else if (e->map_id == 0xd0019 && dn == 0x54) {
			l->points = 0;
		}
		break;
	case 45:                                 // 0x575f80
		if (dn < 0 && (e->map_id == 0xd0029 || e->map_id == 0xd002d || e->map_id == 0xd0031 || e->map_id == 0xd0039)) {
			static const float d[3] = { 0.1f, 0.9f, 0.1f }, c[3] = { 0.4f, 0.4f, 0.4f };
			light(l, 2, d, c, false);
		}
		break;
	case 47:
		fire_lights(l, in->dt);
		break;
	case 48: case 49: case 50: case 51:      // 0x43f9d0, 0x440820, 0x441810, 0x5751a0, and 51's 0x5750a0
		if (in->stage == 51 && dn == 0x60 && t > 1220.0f)
			e->clip_near = 10.0f;
		if (!f(510)) {
			// until Mary (510): another set of the room's colours for a camera up on the platform (0x7b33f8)
			// and one below it (0x7b33b8), where the point lights are off, light 1 is and light 2 is from
			// above (0x47b6c0 of light 0 has no effect in the exe)
			bool up = in->eye[1] <= -16000.0f;
			for (int i = 0; i < 3; i++) {
				e->bcol[i] = up ? 22.4f : 11.4f, e->bamb[i] = up ? 0.8f : 2.745f;
				e->vamb[i] = up ? 0.357143f : 0.05f, e->amb[i] = up ? 0.55f : 0.725f;
			}
			l->points = up;
			if (!up) {
				static const float d1[3] = { 1.0f, 0.0f, 0.0f }, off[3] = { 0.0f, 0.0f, 0.0f };
				static const float d2[3] = { 0.01777f, 0.9999f, 0.023f }, c2[3] = { 1.7f, 1.7f, 1.7f };
				light(l, 1, d1, off, false);
				light(l, 2, d2, c2, false);
			}
		}
		break;
	case 55:                                 // 0x572ec0
		if (e->map_id == 0x10001d && in->james[1] > -20.0f) {
			l->pt0_set = true;
			l->pt0[0] = l->pt0[1] = l->pt0[2] = 1.2f;
		}
		break;
	case 56:                                 // 0x571be0
		if (e->map_id == 0x10001d)
			l->pt0_set = true;
		break;
	default:
		break;
	}
}
