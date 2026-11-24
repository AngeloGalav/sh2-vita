#pragma once
#include <3ds.h>
#include "envlight.h"

// The stages' light and fog functions (Stage_Data +0x34, the second of its five): the PC runs the stage's
// every frame between taking the room's record (0x47be40) and setting up the lights (0x47b870), in play and
// in scenes. They move the fog (0x47cc10 end, 0x47cc20 start, 0x47cc30 / 0x47cc40 the factors at them), the
// clip planes (0x945558, 0x94554c), the ambient and the colours, and put other parallel lights in place of
// the record's 1 and 2 (0x47b6c0). stageenv.c has them by stage.

typedef struct {
	bool set[3];                 // the record's light k is replaced this frame (1 and 2)
	bool chars[3];               // ... for the characters only (0x47b6c0 with a negative index)
	float dir[3][3], col[3][3];  // direction it travels, colour as in the record (the map's is a quarter)
	bool fogp_set;
	int fogp[4];                 // the fog particles' colour (0x4879c0)
	int points;                  // the record's point lights on (pointLNum), -1 to leave them
	bool pt0_set;
	float pt0[3];                // the first point light's colour
} StageLights;

typedef struct {
	int stage, room, demo;       // demo: the scene's number, -1 when none runs
	float demo_t;                // gCutsceneTimer
	int demo_cuts;               // cuts since the scene began
	float eye[3], look[3];       // the camera, and the way it looks
	float james[3];
	float dt;
} StageEnvIn;

// env comes in as the room's record and goes out as the frame's.
void stageenv_frame(RoomEnv* env, StageLights* l, const StageEnvIn* in);
