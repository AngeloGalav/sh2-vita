// The citro3d calls the game makes, for the PS Vita build, on vitaGL (compat/c3d_*.c).
//
// The model the layer keeps is the PICA's: one 240 x 400 portrait render target per 3DS screen,
// projections tilted a quarter turn with a [-1, 0] depth range, a uniform file of float4 registers
// per vertex program, six TEV combiner stages, fragment lighting from look-up tables and a fog
// table indexed by depth. compat/c3d_gl.c turns each draw's state into a GL program (the vertex
// program translated from its .v.pica, a fragment program generated from the TEV and lighting
// state and cached), so the game's renderer runs as written. Type layouts that the game reads
// (C3D_FVec, C3D_Mtx, C3D_Tex's size fields) match citro3d's.
#pragma once
#include <3ds.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------- maths
typedef union {
	struct { float w, z, y, x; };
	struct { float r, k, j, i; };
	float c[4];
} C3D_FVec;
typedef C3D_FVec C3D_FQuat;
typedef union {
	C3D_FVec r[4];
	float m[16];
} C3D_Mtx;
typedef struct { u8 b, g, r, a; } C3D_UNIT_COLOR;

// The top screen's shape: on the Vita its whole display, 960 x 544 (16:9 or so), where the 3DS's
// 400 x 240 is 5:3. The 3D follows it; the game's 2D, laid out in 400 x 240, is drawn across the
// full width (6.7% wider than it was drawn on the 3DS).
#define C3D_AspectRatioTop (960.0f / 544.0f)
#define C3D_AspectRatioBot (320.0f / 240.0f)
#define M_TAU (6.28318530717958647692528676655900576)
static inline float C3D_AngleFromDegrees(float deg) { return deg * (float)M_TAU / 360.0f; }

static inline C3D_FVec FVec4_New(float x, float y, float z, float w)
{
	C3D_FVec v; v.x = x; v.y = y; v.z = z; v.w = w; return v;
}
static inline C3D_FVec FVec3_New(float x, float y, float z) { return FVec4_New(x, y, z, 0.0f); }
static inline float FVec3_Dot(C3D_FVec a, C3D_FVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline C3D_FVec FVec3_Subtract(C3D_FVec a, C3D_FVec b) { return FVec3_New(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline C3D_FVec FVec3_Cross(C3D_FVec a, C3D_FVec b)
{
	return FVec3_New(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline C3D_FVec FVec3_Normalize(C3D_FVec v)
{
	float l = sqrtf(FVec3_Dot(v, v));
	return l > 0.0f ? FVec3_New(v.x / l, v.y / l, v.z / l) : v;
}
static inline void Mtx_Zeros(C3D_Mtx* m) { memset(m, 0, sizeof *m); }
static inline void Mtx_Copy(C3D_Mtx* out, const C3D_Mtx* in) { *out = *in; }
void Mtx_Identity(C3D_Mtx* m);
void Mtx_Multiply(C3D_Mtx* out, const C3D_Mtx* a, const C3D_Mtx* b);
C3D_FVec Mtx_MultiplyFVec4(const C3D_Mtx* m, C3D_FVec v);
C3D_FVec Mtx_MultiplyFVec3(const C3D_Mtx* m, C3D_FVec v);
void Mtx_Translate(C3D_Mtx* m, float x, float y, float z, bool bRightSide);
void Mtx_Scale(C3D_Mtx* m, float x, float y, float z);
void Mtx_RotateX(C3D_Mtx* m, float angle, bool bRightSide);
void Mtx_RotateY(C3D_Mtx* m, float angle, bool bRightSide);
void Mtx_RotateZ(C3D_Mtx* m, float angle, bool bRightSide);
void Mtx_Ortho(C3D_Mtx* m, float left, float right, float bottom, float top, float near, float far, bool lh);
void Mtx_Persp(C3D_Mtx* m, float fovy, float aspect, float near, float far, bool lh);
void Mtx_OrthoTilt(C3D_Mtx* m, float left, float right, float bottom, float top, float near, float far, bool lh);
void Mtx_PerspTilt(C3D_Mtx* m, float fovy, float aspect, float near, float far, bool lh);
void Mtx_PerspStereoTilt(C3D_Mtx* m, float fovy, float aspect, float near, float far, float iod,
                         float screen, bool lh);
void Mtx_LookAt(C3D_Mtx* out, C3D_FVec eye, C3D_FVec target, C3D_FVec up, bool lh);

// ---------------------------------------------------------------- frame and targets
#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
enum { C3D_UNSIGNED_BYTE = 0, C3D_UNSIGNED_SHORT = 1 };
enum { C3D_FRAME_SYNCDRAW = 1, C3D_FRAME_NONBLOCK = 2 };
typedef enum { C3D_CLEAR_COLOR = 1, C3D_CLEAR_DEPTH = 2, C3D_CLEAR_ALL = 3 } C3D_ClearBits;

typedef struct C3D_Tex C3D_Tex;
typedef struct C3D_RenderTarget_tag {
	u16 width, height;           // as citro3d: the portrait size (240 x 400 for the top screen)
	u16 gl_w, gl_h;              // the GL surface's size: larger than width x height for the screen's 2x2 target
	float sx, sy;                // gl_w / width, gl_h / height: what the game's viewports are scaled by
	u32 fbo, depth_rb;           // GL objects
	C3D_Tex* tex;                // the colour texture, owned unless made from the game's texture
	bool own_tex;
	int screen, side;            // C3D_RenderTargetSetOutput; -1 when shown nowhere
	u32 clear_color;
	bool linked;
	struct { void* colorBuf; void* depthBuf; u16 width, height; } frameBuf;   // colorBuf: a CPU copy, made on demand
} C3D_RenderTarget;

bool C3D_Init(size_t cmdBufSize);
void C3D_Fini(void);
float C3D_FrameRate(float fps);
bool C3D_FrameBegin(u8 flags);
bool C3D_FrameDrawOn(C3D_RenderTarget* target);
void C3D_FrameSplit(u8 flags);
void C3D_FrameEnd(u8 flags);
void C3D_FrameSync(void);
float C3D_GetDrawingTime(void);
float C3D_GetProcessingTime(void);
float C3D_GetCmdBufUsage(void);
C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, GPU_DEPTHBUF depthFmt);
C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, GPU_DEPTHBUF depthFmt);
void C3D_RenderTargetDelete(C3D_RenderTarget* target);
void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags);
void C3D_RenderTargetClear(C3D_RenderTarget* target, C3D_ClearBits bits, u32 clearColor, u32 clearDepth);
void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h);
void C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags);
void C3D_SyncTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags);

// ---------------------------------------------------------------- fixed-function state
void C3D_DepthMap(bool bIsZBuffer, float zScale, float zOffset);
void C3D_CullFace(GPU_CULLMODE mode);
void C3D_StencilTest(bool enable, GPU_TESTFUNC function, int ref, int inputMask, int writeMask);
void C3D_StencilOp(GPU_STENCILOP sfail, GPU_STENCILOP dfail, GPU_STENCILOP pass);
void C3D_BlendingColor(u32 color);
void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask);
void C3D_AlphaTest(bool enable, GPU_TESTFUNC function, int ref);
void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq, GPU_BLENDFACTOR srcClr,
                    GPU_BLENDFACTOR dstClr, GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha);
void C3D_ColorLogicOp(GPU_LOGICOP op);

// ---------------------------------------------------------------- vertex programs
// A "DVLB" here names one of the game's vertex programs (compat/c3d_shaders.c holds each one's
// GL translation and the uniform registers picasso gave its .fvec names).
typedef struct { int program; } DVLE_s;
typedef struct { u32 numDVLE; DVLE_s* DVLE; DVLE_s one; } DVLB_s;
typedef struct { DVLE_s* dvle; } shaderInstance_s;
typedef struct { shaderInstance_s* vertexShader; shaderInstance_s* geometryShader; shaderInstance_s vsh; } shaderProgram_s;
DVLB_s* DVLB_ParseFile(u32* shbinData, u32 shbinSize);
void DVLB_Free(DVLB_s* dvlb);
Result shaderProgramInit(shaderProgram_s* sp);
Result shaderProgramFree(shaderProgram_s* sp);
Result shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle);
s8 shaderInstanceGetUniformLocation(shaderInstance_s* si, const char* name);
void C3D_BindProgram(shaderProgram_s* program);

// The uniform file: 96 float4 registers for the vertex stage, written through these
C3D_FVec* C3D_FVUnifWritePtr(GPU_SHADER_TYPE type, int id, int size);
static inline void C3D_FVUnifMtxNx4(GPU_SHADER_TYPE type, int id, const C3D_Mtx* mtx, int num)
{
	C3D_FVec* p = C3D_FVUnifWritePtr(type, id, num);
	for (int i = 0; i < num; i++)
		p[i] = mtx->r[i];
}
static inline void C3D_FVUnifMtx4x4(GPU_SHADER_TYPE t, int id, const C3D_Mtx* m) { C3D_FVUnifMtxNx4(t, id, m, 4); }
static inline void C3D_FVUnifMtx3x4(GPU_SHADER_TYPE t, int id, const C3D_Mtx* m) { C3D_FVUnifMtxNx4(t, id, m, 3); }
static inline void C3D_FVUnifSet(GPU_SHADER_TYPE type, int id, float x, float y, float z, float w)
{
	C3D_FVec* p = C3D_FVUnifWritePtr(type, id, 1);
	p->x = x; p->y = y; p->z = z; p->w = w;
}

// ---------------------------------------------------------------- vertex data
typedef struct {
	u32 flags[2];
	u64 permutation;
	int attrCount;
	struct { int reg; GPU_FORMATS fmt; int count; bool fixed; } a[12];
} C3D_AttrInfo;
void AttrInfo_Init(C3D_AttrInfo* info);
int AttrInfo_AddLoader(C3D_AttrInfo* info, int regId, GPU_FORMATS format, int count);
int AttrInfo_AddFixed(C3D_AttrInfo* info, int regId);
C3D_AttrInfo* C3D_GetAttrInfo(void);
void C3D_SetAttrInfo(C3D_AttrInfo* info);
void C3D_FixedAttribSet(int id, float x, float y, float z, float w);

typedef struct {
	u32 base_paddr;
	int bufCount;
	struct { const void* data; int stride; int attribCount; u64 permutation; } b[12];
} C3D_BufInfo;
void BufInfo_Init(C3D_BufInfo* info);
int BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride, int attribCount, u64 permutation);
C3D_BufInfo* C3D_GetBufInfo(void);
void C3D_SetBufInfo(C3D_BufInfo* info);

void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size);
void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices);

// ---------------------------------------------------------------- textures
typedef struct { void* data[6]; } C3D_TexCube;
struct C3D_Tex {
	union { void* data; C3D_TexCube* cube; };   // the PICA-layout pixels for textures the CPU writes
	GPU_TEXCOLOR fmt : 8;          // 0x80 | the format: already in the Vita's form (c3d_tex.c, vitatex.py)
	size_t size : 24;
	union { u32 dim; struct { u16 height, width; }; };
	u32 param;
	u32 border;
	union { u32 lodParam; struct { u16 lodBias; u8 maxLevel, minLevel; }; };
	u32 gl;                        // the GL texture
	bool on_vram, dirty, params_dirty, render_target;
	bool etc1_mips;                // the ETC1 filter last applied (c3d_tex.c, SELECT + square)
	int reg;                       // its data's entry in c3d_tex.c's registry + 1, 0 none
	u32 seen;                      // that entry's write count when last uploaded
};
typedef struct {
	u16 width, height;
	u8 maxLevel : 4;
	GPU_TEXCOLOR format : 8;
	GPU_TEXTURE_MODE_PARAM type : 3;
	bool onVram : 1;
} C3D_TexInitParams;
bool C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p);
static inline bool C3D_TexInit(C3D_Tex* tex, u16 w, u16 h, GPU_TEXCOLOR fmt)
{
	C3D_TexInitParams p = { w, h, 0, fmt, GPU_TEX_2D, false };
	return C3D_TexInitWithParams(tex, NULL, p);
}
static inline bool C3D_TexInitVRAM(C3D_Tex* tex, u16 w, u16 h, GPU_TEXCOLOR fmt)
{
	C3D_TexInitParams p = { w, h, 0, fmt, GPU_TEX_2D, true };
	return C3D_TexInitWithParams(tex, NULL, p);
}
void* C3D_TexGetImagePtr(C3D_Tex* tex, void* data, int level, u32* size);
void C3D_TexLoadImage(C3D_Tex* tex, const void* data, GPU_TEXFACE face, int level);
void C3D_TexBind(int unitId, C3D_Tex* tex);
void C3D_TexFlush(C3D_Tex* tex);           // the CPU changed tex->data: upload it
void C3D_TexDelete(C3D_Tex* tex);
void C3D_TexSetFilter(C3D_Tex* tex, GPU_TEXTURE_FILTER_PARAM magFilter, GPU_TEXTURE_FILTER_PARAM minFilter);
void C3D_TexSetFilterMipmap(C3D_Tex* tex, GPU_TEXTURE_FILTER_PARAM filter);
void C3D_TexSetWrap(C3D_Tex* tex, GPU_TEXTURE_WRAP_PARAM wrapS, GPU_TEXTURE_WRAP_PARAM wrapT);
static inline void C3D_TexSetLodBias(C3D_Tex* tex, float lodBias) { (void)tex; (void)lodBias; }

// ---------------------------------------------------------------- TEV
typedef struct {
	u16 srcRgb, srcAlpha;
	union { u32 opAll; struct { u32 opRgb : 12, opAlpha : 12; }; };
	u16 funcRgb, funcAlpha;
	u32 color;
	u16 scaleRgb, scaleAlpha;
} C3D_TexEnv;
typedef enum { C3D_RGB = 1, C3D_Alpha = 2, C3D_Both = 3 } C3D_TexEnvMode;
C3D_TexEnv* C3D_GetTexEnv(int id);
void C3D_SetTexEnv(int id, C3D_TexEnv* env);
void C3D_DirtyTexEnv(C3D_TexEnv* env);
void C3D_TexEnvBufUpdate(int mode, int mask);
void C3D_TexEnvBufColor(u32 color);
static inline void C3D_TexEnvInit(C3D_TexEnv* env)
{
	env->srcRgb = GPU_TEVSOURCES(GPU_PREVIOUS, 0, 0);
	env->srcAlpha = env->srcRgb;
	env->opAll = 0;
	env->funcRgb = env->funcAlpha = GPU_REPLACE;
	env->color = 0xFFFFFFFF;
	env->scaleRgb = env->scaleAlpha = GPU_TEVSCALE_1;
}
#define C3D_TexEnvSrc(env, mode, ...) C3D_TexEnvSrc_((env), (mode), __VA_ARGS__, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR)
#define C3D_TexEnvSrc_(env, mode, s1, s2, s3, ...) C3D_TexEnvSrc3((env), (mode), (s1), (s2), (s3))
static inline void C3D_TexEnvSrc3(C3D_TexEnv* env, C3D_TexEnvMode mode, int s1, int s2, int s3)
{
	int p = GPU_TEVSOURCES(s1, s2, s3);
	if (mode & C3D_RGB) env->srcRgb = p;
	if (mode & C3D_Alpha) env->srcAlpha = p;
}
#define C3D_TexEnvOpRgb(env, ...) C3D_TexEnvOpRgb_((env), __VA_ARGS__, 0, 0)
#define C3D_TexEnvOpRgb_(env, o1, o2, o3, ...) ((env)->opRgb = GPU_TEVOPERANDS((o1), (o2), (o3)))
#define C3D_TexEnvOpAlpha(env, ...) C3D_TexEnvOpAlpha_((env), __VA_ARGS__, 0, 0)
#define C3D_TexEnvOpAlpha_(env, o1, o2, o3, ...) ((env)->opAlpha = GPU_TEVOPERANDS((o1), (o2), (o3)))
static inline void C3D_TexEnvFunc(C3D_TexEnv* env, C3D_TexEnvMode mode, GPU_COMBINEFUNC f)
{
	if (mode & C3D_RGB) env->funcRgb = f;
	if (mode & C3D_Alpha) env->funcAlpha = f;
}
static inline void C3D_TexEnvColor(C3D_TexEnv* env, u32 color) { env->color = color; }
static inline void C3D_TexEnvScale(C3D_TexEnv* env, int mode, GPU_TEVSCALE s)
{
	if (mode & C3D_RGB) env->scaleRgb = s;
	if (mode & C3D_Alpha) env->scaleAlpha = s;
}

// ---------------------------------------------------------------- fog
// A fog table: 128 values over the depth range, as FogLut_FromArray takes them
typedef struct { float v[129]; } C3D_FogLut;
void C3D_FogGasMode(GPU_FOGMODE fogMode, GPU_GASMODE gasMode, bool zFlip);
void C3D_FogColor(u32 color);
void C3D_FogLutBind(C3D_FogLut* lut);
void FogLut_FromArray(C3D_FogLut* lut, const float data[256]);
static inline float FogLut_CalcZ(float depth, float near, float far)
{
	return far * near / (depth * (far - near) + near);
}

// ---------------------------------------------------------------- fragment lighting
#define C3D_LIGHT_LUT_SIZE 256
typedef struct { float v[C3D_LIGHT_LUT_SIZE]; bool negative; } C3D_LightLut;
typedef struct { C3D_LightLut lut; float scale, bias; } C3D_LightLutDA;
typedef float (*C3D_LightLutFunc)(float x, float param);
typedef float (*C3D_LightLutFuncDA)(float dist, float arg0, float arg1);
void LightLut_FromFunc(C3D_LightLut* lut, C3D_LightLutFunc func, float param, bool negative);
void LightLutDA_Create(C3D_LightLutDA* lut, C3D_LightLutFuncDA func, float from, float to, float arg0, float arg1);

typedef struct {
	float ambient[3], diffuse[3], specular0[3], specular1[3], emission[3];
} C3D_Material;

typedef struct C3D_LightEnv_t C3D_LightEnv;
typedef struct C3D_Light_t {
	C3D_LightEnv* parent;
	int id;
	bool enabled, spot_on, dist_on, positional;
	float pos[4];                    // view space; w 0 for a directional light
	float spot_dir[3];
	float diffuse[3], ambient[3], specular0[3], specular1[3];
	C3D_LightLut* spot;
	C3D_LightLutDA* da;
} C3D_Light;
struct C3D_LightEnv_t {
	C3D_Light* lights[8];
	C3D_LightLut* lut[8];
	int lut_input[8];
	bool lut_abs[8];
	float ambient[3];
	float mat_ambient[3], mat_diffuse[3], mat_specular0[3], mat_emission[3];
	u32 serial;                      // bumped on every change, for the GL backend
};
void C3D_LightEnvInit(C3D_LightEnv* env);
void C3D_LightEnvBind(C3D_LightEnv* env);
void C3D_LightEnvMaterial(C3D_LightEnv* env, const C3D_Material* mtl);
void C3D_LightEnvAmbient(C3D_LightEnv* env, float r, float g, float b);
void C3D_LightEnvLut(C3D_LightEnv* env, GPU_LIGHTLUTID lutId, GPU_LIGHTLUTINPUT input, bool negative, C3D_LightLut* lut);
int C3D_LightInit(C3D_Light* light, C3D_LightEnv* env);
void C3D_LightEnable(C3D_Light* light, bool enable);
void C3D_LightColor(C3D_Light* light, float r, float g, float b);
void C3D_LightDiffuse(C3D_Light* light, float r, float g, float b);
void C3D_LightSpecular0(C3D_Light* light, float r, float g, float b);
void C3D_LightSpecular1(C3D_Light* light, float r, float g, float b);
void C3D_LightPosition(C3D_Light* light, C3D_FVec* pos);
void C3D_LightSpotEnable(C3D_Light* light, bool enable);
void C3D_LightSpotDir(C3D_Light* light, float x, float y, float z);
void C3D_LightSpotLut(C3D_Light* light, C3D_LightLut* lut);
void C3D_LightDistAttnEnable(C3D_Light* light, bool enable);
void C3D_LightDistAttn(C3D_Light* light, C3D_LightLutDA* lut);

// ---------------------------------------------------------------- procedural texture (unit 3)
typedef struct {
	int clampU, clampV;
	int mapRgbU, mapRgbV;
	bool separateAlpha;
	int mapAlphaU, mapAlphaV;
	int filter;
	int offset, width;
} C3D_ProcTex;
typedef float C3D_ProcTexLut[128];
typedef struct { u32 color[256]; u32 diff[256]; } C3D_ProcTexColorLut;
void C3D_ProcTexInit(C3D_ProcTex* pt, int offset, int length);
void C3D_ProcTexClamp(C3D_ProcTex* pt, GPU_PROCTEX_CLAMP u, GPU_PROCTEX_CLAMP v);
void C3D_ProcTexCombiner(C3D_ProcTex* pt, bool separateAlpha, GPU_PROCTEX_MAPFUNC rgb, GPU_PROCTEX_MAPFUNC alpha);
void C3D_ProcTexFilter(C3D_ProcTex* pt, GPU_PROCTEX_FILTER min);
void C3D_ProcTexBind(int texCoordId, C3D_ProcTex* pt);
void C3D_ProcTexLutBind(GPU_PROCTEX_LUTID id, C3D_ProcTexLut* lut);
void ProcTexLut_FromArray(C3D_ProcTexLut* lut, const float in[129]);
void C3D_ProcTexColorLutBind(C3D_ProcTexColorLut* lut);
void ProcTexColorLut_Write(C3D_ProcTexColorLut* out, const u32* in, int offset, int length);
