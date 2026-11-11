#include "gamecam.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/view/sh2cam.h"
#include "areaidx.h"

#define SCREEN_SY 448.0f          // PS2 frame height: the units of the roads' projection values
#define EYE_HEIGHT 850.0f         // James' eyes above his feet

#define MAX_ARRAYS 160
static _VC_ROAD_DATA* s_arrays[MAX_ARRAYS + 1];   // NULL-terminated; writable (projection is
static int s_narrays, s_nroads;                    // adjusted in place by vcChangeProjByDist)
static char s_area[8];
static bool s_started;
static SH2CamChara s_slots[SH2CAM_CHARA_SLOTS];
static int s_mv_type;
static float s_eye[3], s_fov, s_yaw, s_watch[3], s_scr_z = 448.0f;

bool gamecam_has_roads(void) { return s_narrays > 0; }
int gamecam_road_count(void) { return s_nroads; }
int gamecam_mv_type(void) { return s_mv_type; }
const float* gamecam_eye(void) { return s_eye; }
const float* gamecam_watch(void) { return s_watch; }
float gamecam_scr_z(void) { return s_scr_z; }
float gamecam_fov(void) { return s_fov; }
float gamecam_yaw(void) { return s_yaw; }

// The same from area.idx (areaidx.c), copied: the arrays are adjusted in place.
static _VC_ROAD_DATA* load_cam_idx(const char* name, int* records)
{
	u32 n = 0;
	const void* d = areaidx_cam(name, &n);
	if (!d || n < sizeof(_VC_ROAD_DATA) || n % sizeof(_VC_ROAD_DATA))
		return NULL;
	_VC_ROAD_DATA* r = malloc(n);
	if (!r)
		return NULL;
	memcpy(r, d, n);
	sh2cam_convert_cam_file(r);
	*records = (int)(n / sizeof(_VC_ROAD_DATA));
	return r;
}

static _VC_ROAD_DATA* load_cam(const char* path, int* records)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	_VC_ROAD_DATA* r = NULL;
	if (n >= (long)sizeof(_VC_ROAD_DATA) && n % sizeof(_VC_ROAD_DATA) == 0) {
		r = malloc(n);
		if (r && fread(r, 1, n, f) != (size_t)n) {
			free(r);
			r = NULL;
		}
	}
	fclose(f);
	if (r) {
		sh2cam_convert_cam_file(r);
		*records = (int)(n / sizeof(_VC_ROAD_DATA));
	}
	return r;
}

// The PC passes the .cam arrays of every resident map block (the current one and its loaded
// neighbours, 0x512eb0), so roads switch cleanly across room boundaries. Rooms of one area sit
// apart on the world grid, so the whole area's arrays are loaded once per area.
void gamecam_load_area(const char* pack_dir, const char* area, const char (*rooms)[16], int nrooms)
{
	if (!strcmp(area, s_area) && s_narrays)
		return;
	for (int i = 0; i < s_narrays; i++)
		free(s_arrays[i]);
	s_narrays = s_nroads = 0;
	snprintf(s_area, sizeof s_area, "%s", area);
	// The open areas (forest, town east/west, lake) also have an area-wide <area>GB.cam that the PC
	// loads first (0x476310 -> 0x512f10, from the glb_crd block table at 0x8885a0); it holds the
	// street roads the per-tile files do not cover.
	bool idx = areaidx_load(pack_dir, area);   // every .cam of the area in one file
	{
		char path[160];
		int records = 0;
		snprintf(path, sizeof path, "%sGB", area);
		_VC_ROAD_DATA* r = idx ? load_cam_idx(path, &records) : NULL;
		bool listed = idx && areaidx_has(path);
		snprintf(path, sizeof path, "%s/bg/%s/%sGB.cam", pack_dir, area, area);
		if (!r && !listed)
			r = load_cam(path, &records);
		if (r) {
			s_arrays[s_narrays++] = r;
			s_nroads += records - 1;
		}
	}
	for (int i = 0; i < nrooms && s_narrays < MAX_ARRAYS; i++) {
		char path[160];
		int records = 0;
		_VC_ROAD_DATA* r = idx ? load_cam_idx(rooms[i], &records) : NULL;
		snprintf(path, sizeof path, "%s/bg/%s/%s.cam", pack_dir, area, rooms[i]);
		if (!r && !(idx && areaidx_has(rooms[i])))
			r = load_cam(path, &records);
		if (r) {
			s_arrays[s_narrays++] = r;
			s_nroads += records - 1;      // minus the END record
		}
	}
	s_arrays[s_narrays] = NULL;
	if (s_started)
		sh2cam_set_road_list(s_narrays ? s_arrays : NULL);   // the PC room loader swaps the list in place
}

void gamecam_reset(const GameCamPlayer* p)
{
	sh2cam_init(s_narrays ? s_arrays : NULL, p->pos, p->yaw, 0, NULL);
	s_started = true;
}

// D3DX row layout (row vectors, translation in row 3): rotation about Y by yaw, so that local +Z
// maps to the facing (sin yaw, 0, cos yaw).
static void yaw_matrix(float m[4][4], float yaw, const float t[3])
{
	float c = cosf(yaw), s = sinf(yaw);
	memset(m, 0, 16 * sizeof(float));
	m[0][0] = c;  m[0][2] = -s;
	m[1][1] = 1.0f;
	m[2][0] = s;  m[2][2] = c;
	m[3][0] = t[0]; m[3][1] = t[1]; m[3][2] = t[2]; m[3][3] = 1.0f;
}

static bool s_on_boat;
void gamecam_on_boat(bool on) { s_on_boat = on; }

// A record of the last minute of what the camera was given and what it gave, written out on request
// (SELECT + D-pad up on the Vita, compat/ctru.c) so a run on a console can be played again on a PC.
// Port's own, for testing.
#define TRACE_N 1800
typedef struct {
	float dt, pos[3], yaw, vel[2], turn, head[2], eye[3], watch[3], scr_z, slot[4][3], look[3], up[3];
	int stage, glb, control, anim, far_view, mv, nslots;
	unsigned slot_st[4];
} TraceRow;
static TraceRow* s_trace;
static unsigned s_trace_n;

void gamecam_trace_dump(void)
{
#ifdef SH2_VITA
	FILE* f = s_trace ? fopen("ux0:data/sh2-vita/camtrace.txt", "w") : NULL;
#else
	FILE* f = s_trace ? fopen("sdmc:/3ds/sh23ds/camtrace.txt", "w") : NULL;
#endif
	if (!f)
		return;
	fprintf(f, "area %s roads %d\n", s_area, s_nroads);
	unsigned n = s_trace_n < TRACE_N ? s_trace_n : TRACE_N;
	for (unsigned k = s_trace_n - n; k < s_trace_n; k++) {
		const TraceRow* r = &s_trace[k % TRACE_N];
		fprintf(f, "%.6f %.3f %.3f %.3f %.6f %.3f %.3f %.6f %.6f %.6f %d %d %d %d %d | %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f | %d",
		        r->dt, r->pos[0], r->pos[1], r->pos[2], r->yaw, r->vel[0], r->vel[1], r->turn, r->head[0], r->head[1],
		        r->stage, r->glb, r->control, r->anim, r->far_view, r->mv, r->eye[0], r->eye[1], r->eye[2],
		        r->watch[0], r->watch[1], r->watch[2], r->scr_z, r->nslots);
		for (int i = 0; i < r->nslots && i < 4; i++)
			fprintf(f, " %.1f %.1f %.1f %x", r->slot[i][0], r->slot[i][1], r->slot[i][2], r->slot_st[i]);
		fprintf(f, " | %.5f %.5f %.5f %.5f %.5f %.5f", r->look[0], r->look[1], r->look[2], r->up[0], r->up[1], r->up[2]);
		fputc('\n', f);
	}
	fclose(f);
}

static void trace_row(const GameCamPlayer* p, float dt, bool far_view, const SH2CamOutput* out, float scr_z)
{
	if (dt <= 0.0f || (!s_trace && !(s_trace = calloc(TRACE_N, sizeof *s_trace))))
		return;
	TraceRow* r = &s_trace[s_trace_n++ % TRACE_N];
	memset(r, 0, sizeof *r);
	r->dt = dt;
	memcpy(r->pos, p->pos, sizeof r->pos);
	r->yaw = p->yaw, r->vel[0] = p->vel[0], r->vel[1] = p->vel[1], r->turn = p->turn_speed;
	r->head[0] = p->head_yaw, r->head[1] = p->head_pitch;
	r->stage = p->stage, r->glb = p->glb_crd, r->control = p->control_type, r->anim = p->anim_status;
	r->far_view = far_view, r->mv = out->cam_mv_type, r->scr_z = scr_z;
	memcpy(r->eye, &out->view_mat[3][0], sizeof r->eye);
	memcpy(r->watch, out->watch_tgt_pos, sizeof r->watch);
	memcpy(r->look, &out->view_mat[2][0], sizeof r->look);
	memcpy(r->up, &out->view_mat[1][0], sizeof r->up);
	for (int i = 0; p->chara && i < SH2CAM_CHARA_SLOTS && p->chara[i].kind; i++) {
		if (i < 4) {
			memcpy(r->slot[i], p->chara[i].pos, sizeof r->slot[i]);
			r->slot_st[i] = p->chara[i].battle_status | (p->chara[i].is_lock_target ? 0x10000u : 0u);
		}
		r->nslots = i + 1;
	}
}

bool gamecam_scripted(void)
{
	const _VC_WORK* w = sh2cam_work();
	if (!w)
		return false;
	if (w->flags & VC_USER_MODE_FLAGS)
		return true;
	const _VC_ROAD_DATA* r = w->cur_near_road.road_p;
	return r && r->rd_type == VC_RD_TYPE_EVENT;
}

void gamecam_update(const GameCamPlayer* p, float dt, bool far_view, C3D_Mtx* view, float* fovy)
{
	if (!s_started)
		gamecam_reset(p);
	SH2CamInput in;
	memset(&in, 0, sizeof in);
	in.frame_ticks60 = dt * 60.0f;          // elapsed 60 Hz ticks, as the PC's SetFrameRate
	in.screen_sx = 640.0f;
	in.screen_sy = SCREEN_SY;
	in.stage_glb_crd = p->glb_crd;
	in.area_code = (uint8_t)p->stage;
	in.control_type = (uint8_t)p->control_type;          // byte 0 of 0x01DBEB80 holds the stage id
	in.far_view_button = far_view;
	in.hero_head_ang_x = p->head_pitch;     // James +0x1dc, +0x1e0 (0x535720): where the search view looks
	in.hero_head_ang_y = p->head_yaw;
	in.james_pos_x = p->pos[0];
	in.james_rot_y = p->yaw;
	in.james_anim_status = p->anim_status;  // 0x1fba8e8: the first-person mix reads it
	in.james_status = s_on_boat ? 0x20000u : 0u;

	float neck[3] = { p->pos[0], p->pos[1] - 830.0f, p->pos[2] };
	yaw_matrix(in.hero_neck_wm, p->yaw, neck);
	in.hero_eye_y = p->pos[1] - EYE_HEIGHT;

	in.chara_pos[0] = p->pos[0];
	in.chara_pos[1] = p->pos[1];
	in.chara_pos[2] = p->pos[2];
	in.chara_pos[3] = 1.0f;
	in.chara_bottom_y = p->pos[1] - 250.0f;
	in.chara_top_y = p->pos[1] - (p->stage < 0x35 ? 937.5f : 812.5f);
	in.chara_grnd_y = p->pos[1];
	// vcMakeHeroHeadPos (0x518d60): neck + (0, -50, 0) in neck space, then 150 higher
	in.chara_head_pos[0] = neck[0];
	in.chara_head_pos[1] = neck[1] - 50.0f - 150.0f;
	in.chara_head_pos[2] = neck[2];
	in.chara_head_pos[3] = 1.0f;
	in.chara_mv_spd = sqrtf(p->vel[0] * p->vel[0] + p->vel[1] * p->vel[1]);
	in.chara_mv_ang_y = atan2f(p->vel[1], p->vel[0]);   // math convention, as the PC (0x5219c5)
	in.chara_ang_spd_y = p->turn_speed;
	in.chara_eye_ang_y = p->yaw;
	in.chara_eye_ang_wy = 2.0943952f;
	in.chara_watch_xz_r = 5500.0f;
	in.chara_slots = p->chara ? p->chara : s_slots;   // empty when the caller has none

	SH2CamOutput out;
	sh2cam_exec(&in, &out);
	s_mv_type = out.cam_mv_type;

	// row 0 right, row 1 screen down, row 2 look direction, row 3 eye position.
	// The view is made from the camera's own axes. It used to go through a look-at with the eye and the eye
	// plus the look direction: a unit vector added to a coordinate of 140,000 keeps 1/64 of it (a float's
	// step there), so the direction came back in steps of up to a degree and the picture turned in jerks,
	// worst in the rooms furthest from the origin (the hospital, the labyrinth), while the camera's own
	// values, and every trace of them, were smooth.
	C3D_FVec eye = FVec3_New(out.view_mat[3][0], out.view_mat[3][1], out.view_mat[3][2]);
	C3D_FVec up = FVec3_New(-out.view_mat[1][0], -out.view_mat[1][1], -out.view_mat[1][2]);
	C3D_FVec z = FVec3_Normalize(FVec3_New(-out.view_mat[2][0], -out.view_mat[2][1], -out.view_mat[2][2]));
	C3D_FVec x = FVec3_Normalize(FVec3_Cross(up, z));
	C3D_FVec y = FVec3_Cross(z, x);
	view->r[0] = FVec4_New(x.x, x.y, x.z, -FVec3_Dot(x, eye));
	view->r[1] = FVec4_New(y.x, y.y, y.z, -FVec3_Dot(y, eye));
	view->r[2] = FVec4_New(z.x, z.y, z.z, -FVec3_Dot(z, eye));
	view->r[3] = FVec4_New(0.0f, 0.0f, 0.0f, 1.0f);
	float scr_z = out.scr_z > 1.0f ? out.scr_z : SCREEN_SY;
	trace_row(p, dt, far_view, &out, scr_z);
	*fovy = 2.0f * atanf(SCREEN_SY * 0.5f / scr_z);
	s_fov = *fovy;
	memcpy(s_eye, &out.view_mat[3][0], sizeof s_eye);
	memcpy(s_watch, out.watch_tgt_pos, sizeof s_watch);
	s_scr_z = scr_z;
	s_yaw = atan2f(out.view_mat[2][0], out.view_mat[2][2]);
}
