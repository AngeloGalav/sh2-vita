// The game's six vertex programs (port/source/*.v.pica) translated to Cg for vitaGL, line for
// line: the same registers (c[] is the PICA's uniform file, laid out as picasso allocates the
// .fvec names, in declaration order from c0), the same raw integer inputs v0.., the same outputs.
// What differs is the last step, shared by all: the PICA's clip z in [-w, 0], scaled by
// C3D_DepthMap, becomes GL's [-w, w], and the depth value goes to the fragment program for the
// fog table (TEXCOORD6), the clip position for the half-size fog's own depth test (TEXCOORD7,
// c3d_gl.c). Every program writes every varying so any fragment program links.
// Vita-only.
#include "c3d_internal.h"

#define HEAD(inputs, nregs) \
	"void main(" inputs \
	"uniform float4 c[" #nregs "], uniform float4 u_depth,\n" \
	"out float4 gl_Position : POSITION, out float4 o_col : COLOR, out float4 o_tc0 : TEXCOORD0,\n" \
	"out float4 o_tc1 : TEXCOORD1, out float4 o_tc2 : TEXCOORD2, out float4 o_view : TEXCOORD4,\n" \
	"out float4 o_nq : TEXCOORD5, out float2 o_z : TEXCOORD6, out float4 o_scr : TEXCOORD7)\n{\n" \
	"o_col = float4(1.0); o_tc0 = float4(0.0); o_tc1 = float4(0.0); o_tc2 = float4(0.0);\n" \
	"o_view = float4(0.0); o_nq = float4(1.0, 0.0, 0.0, 0.0);\n"

// p is the PICA's clip position
#define TAIL \
	"gl_Position = float4(p.x, p.y, 2.0 * u_depth.x * p.z + (2.0 * u_depth.y - 1.0) * p.w, p.w);\n" \
	"o_z = float2(u_depth.x * p.z + u_depth.y * p.w, p.w);\n" \
	"o_scr = gl_Position;\n}\n"

#define MV_PROJ(src) \
	"float4 r1 = float4(dot(c[4], " src "), dot(c[5], " src "), dot(c[6], " src "), dot(c[7], " src "));\n" \
	"float4 p = float4(dot(c[0], r1), dot(c[1], r1), dot(c[2], r1), dot(c[3], r1));\n"

// The normal quaternion as the .v.pica files build it: the 180-degree turn about the half-way
// axis between +Z and n, (1, 0, 0, 0) when n is -Z
#define NORMAL_QUAT(n) \
	"float h = 0.5 * (1.0 + " n ".z);\n" \
	"if (h > 0.0) { float rs = rsqrt(h); o_nq = float4(0.5 * " n ".x * rs, 0.5 * " n ".y * rs, 1.0 / rs, 0.0); }\n"

static const char WORLD[] =
	HEAD("float4 v0, float4 v1, float4 v2, float4 v3,\n", 18)
	"float4 r0 = float4(v0.xyz, 1.0);\n"
	MV_PROJ("r0")
	"o_view = -r1;\n"
	"o_tc0 = float4(v1.x, 1.0 - v1.y, v1.z, v1.w);\n"
	"float3 n = v3.xyz * rsqrt(dot(v3.xyz, v3.xyz));\n"
	"float4 r4 = c[11] * max(dot(c[8].xyz, n), 0.0);\n"
	"r4 = max(dot(c[9].xyz, n), 0.0) * c[12] + r4;\n"
	"r4 = max(dot(c[10].xyz, n), 0.0) * c[13] + r4;\n"
	"r4 = c[14] + r4;\n"
	"r4 = c[15] * r4;\n"
	"float4 r5 = v2 * 0.00392156862745;\n"
	"r4 = r5 * c[16] + r4;\n"
	"r4 = min(c[17] + r4, float4(1.0));\n"
	"o_col = float4(r4.xyz, r5.w);\n"
	"float3 vn = float3(dot(c[4].xyz, n), dot(c[5].xyz, n), dot(c[6].xyz, n));\n"
	"vn = vn * rsqrt(dot(vn, vn));\n"
	NORMAL_QUAT("vn")
	TAIL;

// inidx: palette slot * 3; bones from c8, three rows each
static const char SKIN[] =
	HEAD("float4 v0, float4 v1, float4 v2, float4 v3, float4 v4,\n", 92)
	"float4 r0 = float4(v0.xyz, 1.0);\n"
	"float4 w = v3 * 0.00392156862745;\n"
	"float3 pos = float3(0.0), nrm = float3(0.0);\n"
	"for (int k = 0; k < 4; k++) {\n"
	"  int i = 8 + (int)v2[k];\n"
	"  pos += w[k] * float3(dot(c[i], r0), dot(c[i + 1], r0), dot(c[i + 2], r0));\n"
	"  nrm += w[k] * float3(dot(c[i].xyz, v4.xyz), dot(c[i + 1].xyz, v4.xyz), dot(c[i + 2].xyz, v4.xyz));\n"
	"}\n"
	"float4 r3 = float4(pos, 1.0);\n"
	MV_PROJ("r3")
	"o_view = -r1;\n"
	"o_tc0 = float4(v1.x, 1.0 - v1.y, v1.z, v1.w);\n"
	"float3 vn = float3(dot(c[4].xyz, nrm), dot(c[5].xyz, nrm), dot(c[6].xyz, nrm));\n"
	"vn = vn * rsqrt(dot(vn, vn));\n"
	NORMAL_QUAT("vn")
	TAIL;

static const char FOG[] =
	HEAD("float4 v0, float4 v1, float4 v2, float4 v3,\n", 8)
	"float4 r0 = float4(v0.xyz, 1.0);\n"
	MV_PROJ("r0")
	"o_tc0 = v1;\n"
	"o_tc1 = v2;\n"
	"o_col = v3 * 0.00392156862745;\n"
	TAIL;

static const char MOVIE[] =
	HEAD("float4 v0, float4 v1,\n", 4)
	"float4 r0 = float4(v0.xy, 0.5, 1.0);\n"
	"float4 p = float4(dot(c[0], r0), dot(c[1], r0), dot(c[2], r0), dot(c[3], r0));\n"
	"o_tc0 = v1;\n"
	TAIL;

static const char SHADOW[] =
	HEAD("float4 v0, float4 v1,\n", 8)
	"float4 r0 = float4(v0.xyz, 1.0);\n"
	MV_PROJ("r0")
	"o_col = v1 * 0.00392156862745;\n"
	TAIL;

static const char TOUCHUI[] =
	HEAD("float4 v0, float4 v1, float4 v2,\n", 4)
	"float4 r0 = float4(v0.xy, 0.5, 1.0);\n"
	"float4 p = float4(dot(c[0], r0), dot(c[1], r0), dot(c[2], r0), dot(c[3], r0));\n"
	"o_tc0 = float4(v1.x, 1.0 - v1.y, v1.z, v1.w);\n"
	"o_col = v2 * 0.00392156862745;\n"
	TAIL;

const VertexProgram c3d_vertex_programs[] = {
	{ "world", { "projection", "modelView", "lightDir", "lightCol", "amb", "gain", "vgain", "base" },
	  { 0, 4, 8, 11, 14, 15, 16, 17 }, 18, 4, true, 1, WORLD },
	{ "skin", { "projection", "modelView", "bones" }, { 0, 4, 8 }, 92, 5, true, 1, SKIN },
	{ "fog", { "projection", "modelView" }, { 0, 4 }, 8, 4, false, 2, FOG },
	{ "movie", { "projection" }, { 0 }, 4, 2, false, 1, MOVIE },
	{ "shadow", { "projection", "modelView" }, { 0, 4 }, 8, 2, false, 0, SHADOW },
	{ "touchui", { "projection" }, { 0 }, 4, 3, false, 1, TOUCHUI },
};
const int c3d_vertex_program_count = sizeof c3d_vertex_programs / sizeof c3d_vertex_programs[0];
