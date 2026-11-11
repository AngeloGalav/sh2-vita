// citro3d on vitaGL: the frame, render targets, fixed-function state, vertex data and the
// fragment pipeline, for the Vita build.
//
// Each draw turns the PICA state into one GL program: the vertex program in Cg (c3d_shaders.c)
// and a fragment program generated from what the PICA would do after rasterising, in its order:
// fragment lighting (citro3d's light environment, look-up tables and all), the six TEV combiner
// stages, the alpha test, then the fog table. Programs are cached by a key of the state that
// shapes the code; colours, light positions and tables go in as uniforms and one look-up texture.
// The rules are the PICA's as Citra documents them (lighting sums, LUT inputs, TEV operands and
// functions, the fog factor); the game's own shaders and numbers are untouched.
//
// Render targets stay in the PICA's portrait orientation (240 x 400 for the top screen), so the
// game's viewports and tilted projections need no change; presenting turns them a quarter to the
// Vita's landscape screen. Vita-only.
#include <citro3d.h>
#include <vitaGL.h>
#include <vitashark.h>
#include <psp2/display.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "vita.h"
#include "c3d_internal.h"
#include "perf.h"

#define SHADER_CACHE VITA_DATA_DIR "/shader_cache"   // compiled programs (compile)

bool touchui_overlay(void);    // ../source/touchui.c

// ---------------------------------------------------------------- log

void vita_log(const char* fmt, ...)
{
	static FILE* f;
	if (!f) {
		sceIoMkdir(VITA_DATA_DIR, 0777);
		f = fopen(VITA_DATA_DIR "/vita_log.txt", "w");
	}
	char line[16384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	sceClibPrintf("sh2vita: %s\n", line);    // the console's debug output, and Vita3K's log
	if (f) {
		fprintf(f, "%s\n", line);
		fflush(f);
	}
}

bool vita_quit_requested(void) { return false; }   // the PS button suspends; the Vita has no "close" event

// ---------------------------------------------------------------- state

#define NREGS 96
#define MAX_TARGETS 16

static struct {
	bool gl_ready;
	int program;                     // c3d_vertex_programs index, -1 none
	C3D_FVec unif[NREGS];
	u32 unif_gen;
	C3D_AttrInfo attr;
	C3D_BufInfo buf;
	float fixed[12][4];
	C3D_TexEnv env[6];
	C3D_Tex* tex[3];
	C3D_LightEnv* lenv;
	int fog_mode;
	bool fog_flip;
	u32 fog_color;
	C3D_FogLut* fog_lut;
	C3D_ProcTex* proctex;
	int proctex_tc;
	C3D_ProcTexLut* pt_rgb;
	C3D_ProcTexLut* pt_alpha;
	C3D_ProcTexColorLut* pt_color;
	float depth_scale, depth_offset;
	bool alpha_test;
	int alpha_func, alpha_ref;
	C3D_RenderTarget* target;
	C3D_RenderTarget* targets[MAX_TARGETS];
	float fps;
	u64 frame_start, frame_cpu;
	u32 lut_tex, ptcol_tex;
	u8 lut_img[32][256];
	u8 lut_sent[32][256];
	u32 ptcol_img[256], ptcol_sent[256];
} G = { .program = -1, .depth_scale = -1.0f, .depth_offset = 0.0f, .fps = 60.0f };

// the fog at half size (see half_fog_begin)
static struct {
	bool on, active;
	bool no_test;                // SELECT + L's third state: the half size without the depth test
	bool show_depth;             // its fourth: the scene's depth as read, over the screen
	C3D_RenderTarget rt;         // the half-size target
	C3D_Tex tex;
	C3D_RenderTarget* main;      // the target the fog belongs on
	GLuint ztex;                 // main's depth, as a texture
	int fog_prog;                // the fog vertex program's index, -1 none
	unsigned passes, draws;      // per second, for the log
	const u32* zdata;            // the depth's memory, for the log's samples
	u16 zw, zh;
} HF = { .on = true, .no_test = true, .fog_prog = -2 };

// ux0:data/sh2-vita/halffog0.txt starts with it off (a test switch)
static void half_fog_init(void)
{
	FILE* f = fopen(VITA_DATA_DIR "/halffog0.txt", "r");
	if (f) {
		fclose(f);
		HF.on = false;
		vita_log("fog at half size off (halffog0.txt)");
	}
}

// SELECT + L: off, on with the depth test, on without it (the start), the depth shown.
// Not the PC's: its sprites test the scene's depth. Without the test each sprite also lays over
// what stands in front of it, which on a console covers the far half-loaded geometry that with the
// test showed through the fog, and costs less (stand-in, port/STATUS.md).
void c3d_half_fog_toggle(void)
{
	int st = !HF.on ? 0 : HF.show_depth ? 3 : HF.no_test ? 2 : 1;
	st = (st + 1) % 4;
	HF.on = st != 0, HF.no_test = st == 2, HF.show_depth = st == 3;
	static const char* const NAME[4] = { "off", "on", "on, without its depth test", "on, the depth it reads shown" };
	vita_log("fog at half size %s", NAME[st]);
}

// The small fog target's colour work in half, for the SGX's rate, or float: SELECT + triangle (ctru.c)
// changes it at once, to compare in one spot. With no depth test the whole program goes in half.
static bool s_fog_float;

void c3d_fog_precision_toggle(void)
{
	s_fog_float = !s_fog_float;
	vita_log("half-size fog's colour in %s", s_fog_float ? "float" : "half");
}

static void half_fog_end(void);

// The game view's surface, portrait as the PICA's. ux0:data/sh2-vita/res416.txt (Options, Resolution)
// draws it at 720 x 416, a little over half the pixels, and the present stretches it. Read at start-up. 416
// and not 408: a depth surface's rows are whole multiples of 32 samples, and the half-size fog reads
// the depth as a texture of exactly that width.
static int s_view_w = 544, s_view_h = 960;

enum { LUT_D0 = 0, LUT_FOG = 1, LUT_LIGHT0 = 2, LUT_PT_RGB = 18, LUT_PT_ALPHA = 19, LUT_ROWS = 32 };

// ---------------------------------------------------------------- init

static GLuint s_present_prog;
static GLint s_present_tex;
static void present_init(void);
static GLuint compile(GLenum type, const char* s);
// The game textures' t turned over in the vertex programs (vertex_shader). vflip0.txt: in the
// fragment programs, as before (a test switch)
static bool s_vflip = true;

void vita_gl_init(void)
{
	if (G.gl_ready)
		return;
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	SceKernelThreadInfo ti = { .size = sizeof ti };
	if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti) >= 0)
		vita_log("GL thread %s: stack %d KB", ti.name, ti.stackSize / 1024);
	sceIoMkdir(VITA_DATA_DIR, 0777);
	sceIoMkdir(SHADER_CACHE, 0777);
	SceIoStat st;
	if (sceIoGetstat("ur0:data/libshacccg.suprx", &st) < 0)
		vita_log("ur0:data/libshacccg.suprx is missing: no shader can be compiled");
	else
		vita_log("ur0:data/libshacccg.suprx: %lld bytes", (long long)st.st_size);
	// Larger GXM rings than the defaults (VDM 128 KB, vertex 2 MB, fragment 512 KB, USSE 16 KB), and
	// more scenes per FBO for frames split between targets, as other vitaGL ports set them for a frame
	// with many draws and passes. ux0:data/sh2-vita/gxmrings0.txt keeps the defaults (a test switch,
	// read here because these must precede vitaGL's start)
	FILE* rings = fopen(VITA_DATA_DIR "/gxmrings0.txt", "r");
	if (rings) {
		fclose(rings);
		vita_log("default GXM rings (gxmrings0.txt)");
	} else {
		vglSetVDMBufferSize(1024 * 1024);
		vglSetVertexBufferSize(4 * 1024 * 1024);
		vglSetFragmentBufferSize(1024 * 1024);
		vglSetUSSEBufferSize(2048 * 1024);
		vglSetupRenderTargetScenesNum(1, 4);
	}
	FILE* low = fopen(VITA_DATA_DIR "/res416.txt", "r");
	if (low) {
		fclose(low);
		s_view_w = 416, s_view_h = 720;
		vita_log("game view at 720 x 416 (res416.txt)");
	}
	FILE* vf = fopen(VITA_DATA_DIR "/vflip0.txt", "r");
	if (vf) {
		fclose(vf);
		s_vflip = false;
		vita_log("texture t turned over in the fragment programs (vflip0.txt)");
	}
	vglInitExtended(0, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
	// The programs are Cg with their semantics written out, so nothing needs the GLSL translator's
	// postponed binding; compiling in glCompileShader reports failures there, where compile() checks
	// them (postponed, they surface as a NULL program inside glLinkProgram)
	vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
	vglWaitVblankStart(GL_TRUE);
	glGenTextures(1, &G.lut_tex);
	glBindTexture(GL_TEXTURE_2D, G.lut_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 256, LUT_ROWS, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, G.lut_img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenTextures(1, &G.ptcol_tex);
	glBindTexture(GL_TEXTURE_2D, G.ptcol_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, G.ptcol_img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	present_init();
	for (int i = 0; i < 6; i++)
		C3D_TexEnvInit(&G.env[i]);
	G.gl_ready = true;
	half_fog_init();
	vita_log("vitaGL ready");
}

bool C3D_Init(size_t cmdBufSize)
{
	(void)cmdBufSize;
	vita_gl_init();
	return true;
}

void C3D_Fini(void) { }

// ---------------------------------------------------------------- frame

float C3D_FrameRate(float fps)
{
	float old = G.fps;
	if (fps > 0.0f)
		G.fps = fps;                     // C3D_FrameEnd paces it
	return old;
}

static void res_apply(void);
static void hf_free(void);

bool C3D_FrameBegin(u8 flags)
{
	(void)flags;
	res_apply();
	G.frame_start = sceKernelGetProcessTimeWide();
	return true;
}

static void bind_target(C3D_RenderTarget* t);

bool C3D_FrameDrawOn(C3D_RenderTarget* t)
{
	if (!t)
		return false;
	half_fog_end();
	bind_target(t);
	glViewport(0, 0, t->gl_w, t->gl_h);
	return true;
}

void C3D_FrameSplit(u8 flags) { (void)flags; }
void C3D_FrameSync(void) { glFinish(); }

static void present(void);
static void vbo_frame(void);

// draws per second for the log: issued, skipped with no shader bound, skipped with a failed program
static unsigned s_draws[3];

// A burst of frames for a console test: SELECT + D-pad down (ctru.c) arms it, three seconds later the next
// CAP_N frames as they are shown go, at half size, to frames.raw in the data folder ("FRM1", width, height,
// count, then RGB rows), and the camera's trace with them (gamecam.c). What changes from one frame to the
// next can then be looked at on a PC.
#define CAP_N 16
#define CAP_W 480
#define CAP_H 272
static u64 s_cap_at;
static int s_cap_n = -1;
static u8 *s_cap, *s_cap_row;

void c3d_capture_arm(void)
{
	if (s_cap_n >= 0)
		return;
	if (!s_cap)
		s_cap = malloc(CAP_N * CAP_W * CAP_H * 3);
	if (!s_cap_row)
		s_cap_row = malloc(960 * 544 * 4);
	if (!s_cap || !s_cap_row) {
		vita_log("frame burst: no memory");
		return;
	}
	s_cap_at = sceKernelGetProcessTimeWide() + 3000000;
	s_cap_n = 0;
	vita_log("frame burst armed");
}

static void capture_frame(void)
{
	if (s_cap_n < 0 || sceKernelGetProcessTimeWide() < s_cap_at)
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, s_cap_row);
	u8* d = s_cap + s_cap_n * CAP_W * CAP_H * 3;
	for (int y = 0; y < CAP_H; y++)
		for (int x = 0; x < CAP_W; x++) {
			const u8* a = s_cap_row + ((y * 2) * 960 + x * 2) * 4;
			for (int k = 0; k < 3; k++)
				*d++ = (u8)((a[k] + a[4 + k] + a[960 * 4 + k] + a[960 * 4 + 4 + k] + 2) >> 2);
		}
	if (++s_cap_n < CAP_N)
		return;
	FILE* f = fopen(VITA_DATA_DIR "/frames.raw", "wb");
	if (f) {
		const u32 h[3] = { CAP_W, CAP_H, CAP_N };
		fwrite("FRM1", 1, 4, f);
		fwrite(h, 4, 3, f);
		fwrite(s_cap, 1, CAP_N * CAP_W * CAP_H * 3, f);
		fclose(f);
	}
	extern void gamecam_trace_dump(void);
	gamecam_trace_dump();
	s_cap_n = -1;
	vita_log("frame burst written");
}

void C3D_FrameEnd(u8 flags)
{
	(void)flags;
	u64 tp = svcGetSystemTick();
	half_fog_end();
	G.frame_cpu = sceKernelGetProcessTimeWide() - G.frame_start;
	vbo_frame();
	static unsigned frames;
	static u64 last;
	frames++;
	if (G.frame_start - last >= 1000000) {
		C3dStats* st = &c3d_stats;
		vita_log("frames %u: draws %u, no shader %u, no program %u; vitaGL %u KB free; textures %u (%u KB, %u ms, "
		         "untile %u ms), buffers %u (%u KB, %u ms; %u new, %u marked by %u flushes of %u KB), draw calls %u "
		         "(%u ms), swap %u ms", frames, s_draws[0],
		         s_draws[1], s_draws[2], (unsigned)(vglMemFree(VGL_MEM_ALL) >> 10), st->tex_n, st->tex_kb,
		         (unsigned)(st->tex_us / 1000), (unsigned)(st->untile_us / 1000), st->buf_n, st->buf_kb,
		         (unsigned)(st->buf_us / 1000), st->buf_new, st->dirtied, st->flush_n, st->flush_kb, st->draw_n,
		         (unsigned)(st->draw_us / 1000), (unsigned)(st->swap_us / 1000));
		if (HF.on) {
			// the depth the half-size fog reads, three samples (stored a frame or two ago): a float in
			// 0..1 is a stored depth, 80808080 vitaGL's untouched fill
			u32 z[3] = { 0, 0, 0 };
			if (HF.zdata) {
				u32 stride = (HF.zw + 31u) & ~31u;
				z[0] = HF.zdata[(HF.zh / 2) * stride + HF.zw / 2];
				z[1] = HF.zdata[16 * stride + 16];
				z[2] = HF.zdata[(HF.zh - 16) * stride + HF.zw - 16];
			}
			float f[3];
			memcpy(f, z, sizeof f);
			vita_log("half fog: %u passes, %u draws; depth %08x (%f) %08x (%f) %08x (%f)", HF.passes, HF.draws,
			         (unsigned)z[0], f[0], (unsigned)z[1], f[1], (unsigned)z[2], f[2]);
			HF.passes = HF.draws = 0;
		}
		memset(st, 0, sizeof *st);
		frames = s_draws[0] = s_draws[1] = s_draws[2] = 0;
		last = G.frame_start;
	}
	present();
	capture_frame();
	// r_end in parts for perf.txt's slow frame lines: r_pres the frame's last draws and the once a
	// second log line, r_pace the wait for the 30 fps beat, r_swap vitaGL's swap (a full display queue,
	// its garbage collector), r_swap1 the swap before this one, which sets this frame's beat through
	// last_vc
	static u64 swap_ticks;
	perf_part("r_pres", tp);
	perf_part("r_swap1", svcGetSystemTick() - swap_ticks);
	tp = svcGetSystemTick();
	// 30 fps: swaps at least two refreshes apart, sent after a refresh. vitaGL shows each frame at the first
	// refresh after the GPU has finished it (its display callback), while the swap returns at once and the
	// CPU goes on with the next frame. Measured on a console with the frame end made to wait for that
	// (sceGxmDisplayQueueFinish, which cost a third of the frame rate and is gone again): the GPU's work
	// after a swap runs to 20 and 30 ms in the heavy rooms, so where it comes to about one refresh, or two,
	// frames reach the screen a refresh early or late in turn while the game's own times stay even.
	// So the screen is watched: after each refresh of the wait, whether the frame shown has changed tells at
	// which refresh after its swap the last frame came up. When two answers both come often, the swaps are
	// sent half a refresh later (or no longer so), which puts the GPU's finish in the middle of a refresh
	// instead of on its edge. Port's own; vita_log has the counts every 4 seconds.
	static unsigned last_vc;
	static void* seen_base;
	static unsigned came[4], sum[4], frames_k, cool;
	static bool half;
	static u64 t_log;
	if (G.fps <= 30.5f) {
		int k = 0;
		while ((int)(sceDisplayGetVcount() - last_vc) < 2) {
			sceDisplayWaitVblankStart();
			SceDisplayFrameBuf fb = { .size = sizeof fb };
			if (!k && sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE) >= 0 && fb.base != seen_base) {
				seen_base = fb.base;
				k = (int)(sceDisplayGetVcount() - last_vc);
			}
		}
		k = k < 1 || k > 2 ? 3 : k;      // 3: not up yet when the next one goes
		came[k]++, sum[k]++;
		if (cool)
			cool--;
		if (++frames_k >= 30) {
			unsigned hi = came[1] > came[2] ? 1 : 2, lo = 3 - hi;
			if (came[3] > came[hi])
				lo = hi, hi = 3;
			else if (came[3] > came[lo])
				lo = 3;
			if (came[lo] >= 5 && !cool) {
				half = !half;
				cool = 90;
				vita_log("frame beat: swaps %s (frames came up after 1, 2, 3+ refreshes: %u %u %u)",
				         half ? "half a refresh later" : "on the refresh", came[1], came[2], came[3]);
			}
			memset(came, 0, sizeof came);
			frames_k = 0;
		}
		if (half)
			sceKernelDelayThread(8300);
		u64 now = sceKernelGetProcessTimeWide();
		if (now - t_log > 4000000) {
			vita_log("frames came up after 1, 2, 3+ refreshes: %u %u %u%s", sum[1], sum[2], sum[3], half ? " (swaps half a refresh later)" : "");
			memset(sum, 0, sizeof sum);
			t_log = now;
		}
	}
	perf_part("r_pace", tp);
	tp = svcGetSystemTick();
	u64 ts = sceKernelGetProcessTimeWide();
	vglSwapBuffers(GL_FALSE);
	c3d_stats.swap_us += sceKernelGetProcessTimeWide() - ts;
	swap_ticks = svcGetSystemTick() - tp;
	perf_part("r_swap", tp);
	last_vc = sceDisplayGetVcount();
	G.target = NULL;
}

float C3D_GetDrawingTime(void) { return 0.0f; }
float C3D_GetProcessingTime(void) { return G.frame_cpu / 1000.0f; }
float C3D_GetCmdBufUsage(void) { return 0.0f; }

// ---------------------------------------------------------------- render targets

static void add_target(C3D_RenderTarget* t)
{
	for (int i = 0; i < MAX_TARGETS; i++)
		if (!G.targets[i]) {
			G.targets[i] = t;
			return;
		}
}

static void make_fbo(C3D_RenderTarget* t, GPU_DEPTHBUF depth)
{
	glGenFramebuffers(1, &t->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex->gl, 0);
	if ((int)depth >= 0) {
		glGenRenderbuffers(1, &t->depth_rb);
		glBindRenderbuffer(GL_RENDERBUFFER, t->depth_rb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, t->gl_w, t->gl_h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth_rb);
	}
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE)
		vita_log("render target %dx%d incomplete: 0x%x", t->width, t->height, st);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	G.target = NULL;
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, GPU_DEPTHBUF depthFmt)
{
	(void)colorFmt;
	C3D_RenderTarget* t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	t->width = width;
	t->height = height;
	// The screen's 2x2 target (supersample.c, 800 x 480 in portrait) is made at the Vita's own
	// 960 x 544 instead, so the scene is drawn at the size it is shown; the game keeps seeing
	// 480 x 800 and its viewports are scaled (C3D_SetViewport). So is the top screen's own 240 x 400,
	// where the title, the start-up logos and the results are drawn: 400 x 240 stretched out, they
	// were soft whatever their art.
	t->gl_w = width, t->gl_h = height;
	if ((width == 480 && height == 800) || (width == 240 && height == 400))
		t->gl_w = (u16)s_view_w, t->gl_h = (u16)s_view_h;
	t->sx = (float)t->gl_w / width, t->sy = (float)t->gl_h / height;
	t->screen = -1;
	t->tex = calloc(1, sizeof(C3D_Tex));
	t->own_tex = true;
	t->tex->width = width;
	t->tex->height = height;
	t->tex->param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR);
	c3d_tex_make_target(t->tex, t->gl_w, t->gl_h);
	make_fbo(t, depthFmt);
	t->frameBuf.width = width;
	t->frameBuf.height = height;
	add_target(t);
	return t;
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, GPU_DEPTHBUF depthFmt)
{
	(void)face;
	(void)level;
	C3D_RenderTarget* t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	t->width = tex->width;
	t->height = tex->height;
	t->gl_w = t->width, t->gl_h = t->height;
	t->sx = t->sy = 1.0f;
	t->screen = -1;
	t->tex = tex;
	c3d_tex_make_target(tex, tex->width, tex->height);
	make_fbo(t, depthFmt);
	t->frameBuf.width = t->width;
	t->frameBuf.height = t->height;
	add_target(t);
	return t;
}

void C3D_RenderTargetDelete(C3D_RenderTarget* t)
{
	if (!t)
		return;
	for (int i = 0; i < MAX_TARGETS; i++)
		if (G.targets[i] == t)
			G.targets[i] = NULL;
	if (G.target == t) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		G.target = NULL;
	}
	glDeleteFramebuffers(1, &t->fbo);
	if (t->depth_rb)
		glDeleteRenderbuffers(1, &t->depth_rb);
	if (t->own_tex) {
		glDeleteTextures(1, &t->tex->gl);
		free(t->tex);
	}
	free(t->frameBuf.colorBuf);
	free(t);
}

// Only one target shows on each screen (stereo.c's right eye is never linked on the Vita: no 3D)
void C3D_RenderTargetSetOutput(C3D_RenderTarget* t, gfxScreen_t screen, gfx3dSide_t side, u32 flags)
{
	(void)flags;
	if (side == GFX_RIGHT)
		return;
	for (int i = 0; i < MAX_TARGETS; i++)
		if (G.targets[i] && G.targets[i]->screen == (int)screen)
			G.targets[i]->screen = -1;
	if (t)
		t->screen = screen;
}

static void bind_target(C3D_RenderTarget* t)
{
	if (G.target == t)
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, t ? t->fbo : 0);
	G.target = t;
}

void C3D_RenderTargetClear(C3D_RenderTarget* t, C3D_ClearBits bits, u32 color, u32 depth)
{
	half_fog_end();
	C3D_RenderTarget* was = G.target;
	bind_target(t);
	GLbitfield m = 0;
	if (bits & C3D_CLEAR_COLOR) {
		glClearColor((color >> 24 & 255) / 255.0f, (color >> 16 & 255) / 255.0f, (color >> 8 & 255) / 255.0f,
		             (color & 255) / 255.0f);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		m |= GL_COLOR_BUFFER_BIT;
	}
	if ((bits & C3D_CLEAR_DEPTH) && t->depth_rb) {
		glClearDepthf((depth & 0xFFFFFF) / 16777215.0f);
		glClearStencil(depth >> 24);
		glDepthMask(GL_TRUE);
		glStencilMask(0xFF);
		m |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
	}
	glDisable(GL_SCISSOR_TEST);
	glClear(m);
	if (was)
		bind_target(was);
	// the masks the game set are restored on the next draw
	G.unif_gen++;
}

void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h)
{
	C3D_RenderTarget* t = G.target;
	if (t && (t->sx != 1.0f || t->sy != 1.0f)) {
		glViewport((GLint)(x * t->sx + 0.5f), (GLint)(y * t->sy + 0.5f), (GLsizei)(w * t->sx + 0.5f),
		           (GLsizei)(h * t->sy + 0.5f));
		return;
	}
	glViewport(x, y, w, h);
}

// The PICA's framebuffer copies. gameover.c keeps a still of the last frame and main.c's
// screenshot reads the top screen; neither is wired on the Vita yet (port/vita/README.md).
void C3D_SyncDisplayTransfer(u32* in, u32 indim, u32* out, u32 outdim, u32 flags)
{
	(void)in; (void)indim; (void)out; (void)outdim; (void)flags;
}

void C3D_SyncTextureCopy(u32* in, u32 indim, u32* out, u32 outdim, u32 size, u32 flags)
{
	(void)in; (void)indim; (void)outdim; (void)flags;
	if (out)
		memset(out, 0, size);
	c3d_tex_mark_range(out, size);
}

// ---------------------------------------------------------------- fixed-function state
// Kept as the PICA has it and applied at each draw

static struct {
	GPU_CULLMODE cull;
	bool depth_on;
	int depth_func, depth_mask;
	bool stencil_on;
	int st_func, st_ref, st_in, st_write, st_sfail, st_dfail, st_pass;
	int beq_c, beq_a, bsrc_c, bdst_c, bsrc_a, bdst_a;
	bool logic_copy;
	u32 blend_color;
} F = { GPU_CULL_NONE, true, GPU_GREATER, GPU_WRITE_ALL, false, GPU_ALWAYS, 0, 0xFF, 0xFF, 0, 0, 0,
        GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
        GPU_ONE_MINUS_SRC_ALPHA, false, 0 };

void C3D_DepthMap(bool z, float scale, float offset)
{
	(void)z;
	G.depth_scale = scale;
	G.depth_offset = offset;
}
void C3D_CullFace(GPU_CULLMODE mode) { F.cull = mode; }
void C3D_StencilTest(bool on, GPU_TESTFUNC f, int ref, int in, int write)
{
	F.stencil_on = on;
	F.st_func = f;
	F.st_ref = ref;
	F.st_in = in;
	F.st_write = write;
}
void C3D_StencilOp(GPU_STENCILOP sfail, GPU_STENCILOP dfail, GPU_STENCILOP pass)
{
	F.st_sfail = sfail;
	F.st_dfail = dfail;
	F.st_pass = pass;
}
void C3D_BlendingColor(u32 color) { F.blend_color = color; }
void C3D_DepthTest(bool on, GPU_TESTFUNC f, GPU_WRITEMASK mask)
{
	F.depth_on = on;
	F.depth_func = f;
	F.depth_mask = mask;
}
void C3D_AlphaTest(bool on, GPU_TESTFUNC f, int ref)
{
	G.alpha_test = on;
	G.alpha_func = f;
	G.alpha_ref = ref;
}
void C3D_AlphaBlend(GPU_BLENDEQUATION ce, GPU_BLENDEQUATION ae, GPU_BLENDFACTOR sc, GPU_BLENDFACTOR dc,
                    GPU_BLENDFACTOR sa, GPU_BLENDFACTOR da)
{
	F.logic_copy = false;
	F.beq_c = ce;
	F.beq_a = ae;
	F.bsrc_c = sc;
	F.bdst_c = dc;
	F.bsrc_a = sa;
	F.bdst_a = da;
}
// The game only asks for COPY (blending off); other logic ops are not on vitaGL
void C3D_ColorLogicOp(GPU_LOGICOP op) { (void)op; F.logic_copy = true; }

static const GLenum TESTF[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };
static const GLenum STOP[8] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
static const GLenum BEQ[5] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX };
static const GLenum BF[15] = {
	GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
	// vitaGL has no constant blend colour; the game never blends with one
	GL_ONE, GL_ZERO, GL_ONE, GL_ZERO, GL_SRC_ALPHA_SATURATE,
};

static void apply_fixed(void)
{
	if (F.cull == GPU_CULL_NONE) {
		glDisable(GL_CULL_FACE);
	} else {
		glEnable(GL_CULL_FACE);
		glFrontFace(GL_CCW);
		glCullFace(F.cull == GPU_CULL_BACK_CCW ? GL_BACK : GL_FRONT);
	}
	bool has_depth = G.target && G.target->depth_rb;
	if (F.depth_on && has_depth) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(TESTF[F.depth_func & 7]);
		glDepthMask((F.depth_mask & GPU_WRITE_DEPTH) ? GL_TRUE : GL_FALSE);
	} else {
		glDisable(GL_DEPTH_TEST);
	}
	glColorMask(F.depth_mask & 1, (F.depth_mask >> 1) & 1, (F.depth_mask >> 2) & 1, (F.depth_mask >> 3) & 1);
	if (F.stencil_on && has_depth) {
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(TESTF[F.st_func & 7], F.st_ref, F.st_in);
		glStencilMask(F.st_write);
		glStencilOp(STOP[F.st_sfail & 7], STOP[F.st_dfail & 7], STOP[F.st_pass & 7]);
	} else {
		glDisable(GL_STENCIL_TEST);
	}
	if (F.logic_copy) {
		glDisable(GL_BLEND);
	} else {
		glEnable(GL_BLEND);
		glBlendEquationSeparate(BEQ[F.beq_c % 5], BEQ[F.beq_a % 5]);
		glBlendFuncSeparate(BF[F.bsrc_c % 15], BF[F.bdst_c % 15], BF[F.bsrc_a % 15], BF[F.bdst_a % 15]);
	}
}

// ---------------------------------------------------------------- vertex programs

static char s_prog_names[8][16];

DVLB_s* DVLB_ParseFile(u32* data, u32 size)
{
	(void)size;
	DVLB_s* d = calloc(1, sizeof *d);
	d->numDVLE = 1;
	d->DVLE = &d->one;
	d->one.program = -1;
	for (int i = 0; i < c3d_vertex_program_count; i++)
		if (!strcmp((const char*)data, c3d_vertex_programs[i].name))
			d->one.program = i;
	if (d->one.program < 0)
		vita_log("DVLB_ParseFile: no Vita translation of %s", (const char*)data);
	(void)s_prog_names;
	return d;
}

void DVLB_Free(DVLB_s* d) { free(d); }
Result shaderProgramInit(shaderProgram_s* sp)
{
	memset(sp, 0, sizeof *sp);
	return 0;
}
Result shaderProgramFree(shaderProgram_s* sp) { (void)sp; return 0; }
Result shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle)
{
	sp->vsh.dvle = dvle;
	sp->vertexShader = &sp->vsh;
	return 0;
}

s8 shaderInstanceGetUniformLocation(shaderInstance_s* si, const char* name)
{
	if (!si || !si->dvle || si->dvle->program < 0)
		return -1;
	const VertexProgram* p = &c3d_vertex_programs[si->dvle->program];
	for (int i = 0; i < 8 && p->uniform_names[i]; i++)
		if (!strcmp(p->uniform_names[i], name))
			return p->uniform_regs[i];
	return -1;
}

void C3D_BindProgram(shaderProgram_s* sp)
{
	G.program = sp && sp->vertexShader && sp->vertexShader->dvle ? sp->vertexShader->dvle->program : -1;
}

C3D_FVec* C3D_FVUnifWritePtr(GPU_SHADER_TYPE type, int id, int size)
{
	(void)type;
	(void)size;
	G.unif_gen++;
	if (id < 0 || id >= NREGS)
		id = 0;
	return &G.unif[id];
}

// ---------------------------------------------------------------- vertex data

void AttrInfo_Init(C3D_AttrInfo* a) { memset(a, 0, sizeof *a); }
int AttrInfo_AddLoader(C3D_AttrInfo* a, int reg, GPU_FORMATS fmt, int count)
{
	if (a->attrCount >= 12)
		return -1;
	int i = a->attrCount++;
	a->a[i].reg = reg;
	a->a[i].fmt = fmt;
	a->a[i].count = count;
	a->a[i].fixed = false;
	return i;
}
int AttrInfo_AddFixed(C3D_AttrInfo* a, int reg)
{
	if (a->attrCount >= 12)
		return -1;
	int i = a->attrCount++;
	a->a[i].reg = reg;
	a->a[i].fixed = true;
	return i;
}
C3D_AttrInfo* C3D_GetAttrInfo(void) { return &G.attr; }
void C3D_SetAttrInfo(C3D_AttrInfo* a) { if (a != &G.attr) G.attr = *a; }
void C3D_FixedAttribSet(int id, float x, float y, float z, float w)
{
	if (id < 0 || id >= 12)
		return;
	G.fixed[id][0] = x;
	G.fixed[id][1] = y;
	G.fixed[id][2] = z;
	G.fixed[id][3] = w;
}

void BufInfo_Init(C3D_BufInfo* b) { memset(b, 0, sizeof *b); }
int BufInfo_Add(C3D_BufInfo* b, const void* data, ptrdiff_t stride, int attribCount, u64 permutation)
{
	if (b->bufCount >= 12)
		return -1;
	int i = b->bufCount++;
	b->b[i].data = data;
	b->b[i].stride = (int)stride;
	b->b[i].attribCount = attribCount;
	b->b[i].permutation = permutation;
	return i;
}
C3D_BufInfo* C3D_GetBufInfo(void) { return &G.buf; }
void C3D_SetBufInfo(C3D_BufInfo* b) { if (b != &G.buf) G.buf = *b; }

static const int FMT_SIZE[4] = { 1, 1, 2, 4 };
static const GLenum FMT_GL[4] = { GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_FLOAT };

// ---------------------------------------------------------------- vertex buffers
// The game's vertex data lives in its own memory, as on the 3DS. Handed to vitaGL as client arrays,
// every draw copied all the vertices up to its highest index: megabytes a draw in the town's big
// maps (r_map 160 ms a frame in ca14). Each buffer is kept in a GL buffer object instead, keyed by
// its address, and sent again only when the game has written it since (GSPGPU_FlushDataCache, as
// the 3DS needs for the GPU to see a write), when a draw reaches past what was sent, or after the
// memory was freed (linearFree).

#define MAX_VBO 2048
#define VBO_MIN (32 * 1024)          // smaller draws stay client arrays (apply_attribs)
typedef struct {
	const u8* ptr;
	u32 size;                    // bytes sent
	GLuint vbo;
	bool dirty;
	u32 used;                    // the frame it was last drawn from
} Vbo;
static Vbo s_vbo[MAX_VBO];
C3dStats c3d_stats;
static int s_nvbo, s_vbo_last;
static u32 s_frame_no;

void c3d_buf_mark_range(const void* adr, u32 size)
{
	const u8* a = adr;
	c3d_stats.flush_n++;
	c3d_stats.flush_kb += size >> 10;
	for (int i = 0; i < s_nvbo; i++)
		if (a < s_vbo[i].ptr + s_vbo[i].size && a + size > s_vbo[i].ptr) {
			c3d_stats.dirtied += !s_vbo[i].dirty;
			s_vbo[i].dirty = true;
		}
}

static void vbo_drop(int i)
{
	glDeleteBuffers(1, &s_vbo[i].vbo);
	s_vbo[i] = s_vbo[--s_nvbo];
	s_vbo_last = 0;
}

void c3d_buf_forget(const void* adr)
{
	for (int i = 0; i < s_nvbo; i++)
		if (s_vbo[i].ptr == adr) {
			vbo_drop(i);
			return;
		}
}

// The buffer object holding need bytes from ptr, bound to target
static void vbo_bind_to(GLenum target, const u8* ptr, u32 need);
static void vbo_bind(const u8* ptr, u32 need) { vbo_bind_to(GL_ARRAY_BUFFER, ptr, need); }

static void vbo_bind_to(GLenum target, const u8* ptr, u32 need)
{
	Vbo* v = NULL;
	if (s_vbo_last < s_nvbo && s_vbo[s_vbo_last].ptr == ptr)
		v = &s_vbo[s_vbo_last];
	for (int i = 0; !v && i < s_nvbo; i++)
		if (s_vbo[i].ptr == ptr)
			v = &s_vbo[i], s_vbo_last = i;
	if (!v) {
		if (s_nvbo == MAX_VBO) {             // the longest unused goes
			int old = 0;
			for (int i = 1; i < s_nvbo; i++)
				if (s_vbo[i].used < s_vbo[old].used)
					old = i;
			vbo_drop(old);
		}
		v = &s_vbo[s_nvbo];
		s_vbo_last = s_nvbo++;
		memset(v, 0, sizeof *v);
		v->ptr = ptr;
		v->dirty = true;
		glGenBuffers(1, &v->vbo);
		c3d_stats.buf_new++;
	}
	glBindBuffer(target, v->vbo);
	if (v->dirty || need > v->size) {
		u32 n = need > v->size ? need : v->size;
		u64 t0 = sceKernelGetProcessTimeWide();
		glBufferData(target, n, ptr, GL_DYNAMIC_DRAW);
		c3d_stats.buf_n++;
		c3d_stats.buf_kb += n >> 10;
		c3d_stats.buf_us += sceKernelGetProcessTimeWide() - t0;
		v->size = n;
		v->dirty = false;
	}
	v->used = s_frame_no;
}

// Buffers not drawn from for ten seconds are let go (their memory may hold something else now)
static void vbo_frame(void)
{
	s_frame_no++;
	if (s_frame_no % 60)
		return;
	for (int i = 0; i < s_nvbo;)
		if (s_frame_no - s_vbo[i].used > 600)
			vbo_drop(i);
		else
			i++;
}

// main.c's 2D vertices are rewritten through the frame with no flush: never a cached copy, however many
// there are (a page of text went past VBO_MIN and drew the first frame's text, cut and overwritten)
static const u8* s_stream;
static u32 s_stream_size;

void vita_buf_stream(const void* p, u32 size)
{
	s_stream = p;
	s_stream_size = size;
}

// nverts: the vertices this draw reaches (its highest index + 1)
static void apply_attribs(int ninputs, u32 nverts)
{
	bool set[16] = { 0 };
	for (int k = 0; k < G.buf.bufCount; k++) {
		const u8* base = G.buf.b[k].data;
		// Only large buffers go into buffer objects (the maps and models, written once and flushed
		// when changed). The small ones (2D, text, sprites) stay client arrays, cheap to copy: some
		// are rewritten every frame with no flush (main.c ui_draw), which a cached copy would miss.
		u32 bytes = nverts * G.buf.b[k].stride;
		bool cached = bytes >= VBO_MIN && !(base >= s_stream && base < s_stream + s_stream_size);
		if (cached)
			vbo_bind(base, bytes);
		else
			glBindBuffer(GL_ARRAY_BUFFER, 0);
		int off = 0;
		for (int j = 0; j < G.buf.b[k].attribCount; j++) {
			int slot = (int)((G.buf.b[k].permutation >> (j * 4)) & 15);
			if (slot >= 12) {                 // padding: 4, 8, 12 or 16 bytes
				off += (slot - 11) * 4;
				continue;
			}
			const typeof(G.attr.a[0])* a = &G.attr.a[slot];
			if (slot >= G.attr.attrCount || a->fixed)
				continue;
			int cs = FMT_SIZE[a->fmt & 3];
			off = (off + cs - 1) & ~(cs - 1);
			if (a->reg < ninputs) {
				glEnableVertexAttribArray(a->reg);
				glVertexAttribPointer(a->reg, a->count, FMT_GL[a->fmt & 3], GL_FALSE, G.buf.b[k].stride,
				                      cached ? (const void*)(uintptr_t)off : (const void*)(base + off));
				set[a->reg] = true;
			}
			off += cs * a->count;
		}
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	for (int i = 0; i < G.attr.attrCount; i++)
		if (G.attr.a[i].fixed && G.attr.a[i].reg < ninputs && !set[G.attr.a[i].reg]) {
			int r = G.attr.a[i].reg;
			glDisableVertexAttribArray(r);
			glVertexAttrib4f(r, G.fixed[i][0], G.fixed[i][1], G.fixed[i][2], G.fixed[i][3]);
			set[r] = true;
		}
	for (int r = 0; r < ninputs; r++)
		if (!set[r]) {
			glDisableVertexAttribArray(r);
			glVertexAttrib4f(r, 0.0f, 0.0f, 0.0f, 1.0f);
		}
}

// ---------------------------------------------------------------- textures and TEV

void C3D_TexBind(int unit, C3D_Tex* tex)
{
	if (unit >= 0 && unit < 3)
		G.tex[unit] = tex;
}

void c3d_forget_tex(C3D_Tex* tex)
{
	for (int i = 0; i < 3; i++)
		if (G.tex[i] == tex)
			G.tex[i] = NULL;
}

C3D_TexEnv* C3D_GetTexEnv(int id) { return &G.env[id < 0 || id > 5 ? 0 : id]; }
void C3D_SetTexEnv(int id, C3D_TexEnv* env) { if (id >= 0 && id < 6) G.env[id] = *env; }
void C3D_DirtyTexEnv(C3D_TexEnv* env) { (void)env; }
void C3D_TexEnvBufUpdate(int mode, int mask) { (void)mode; (void)mask; }
void C3D_TexEnvBufColor(u32 color) { (void)color; }

// ---------------------------------------------------------------- fog

void C3D_FogGasMode(GPU_FOGMODE mode, GPU_GASMODE gas, bool zflip)
{
	(void)gas;
	G.fog_mode = mode;
	G.fog_flip = zflip;
}
void C3D_FogColor(u32 color) { G.fog_color = color; }
void C3D_FogLutBind(C3D_FogLut* lut) { G.fog_lut = lut; }

// FogLut_FromArray's input: 128 values then 128 differences to the next
void FogLut_FromArray(C3D_FogLut* lut, const float data[256])
{
	for (int i = 0; i < 128; i++)
		lut->v[i] = data[i];
	lut->v[128] = data[127] + data[255];
}

// ---------------------------------------------------------------- lighting

static inline u8 lut8(float v) { return v <= 0.0f ? 0 : v >= 1.0f ? 255 : (u8)(v * 255.0f + 0.5f); }

// A table holds the function at 256 points: over [-1, 1) for a signed input, [0, 1) otherwise
void LightLut_FromFunc(C3D_LightLut* lut, C3D_LightLutFunc func, float param, bool negative)
{
	lut->negative = negative;
	for (int i = 0; i < C3D_LIGHT_LUT_SIZE; i++) {
		float x = negative ? (i - 128) / 128.0f : i / 256.0f;
		lut->v[i] = func(x, param);
	}
}

void LightLutDA_Create(C3D_LightLutDA* lut, C3D_LightLutFuncDA func, float from, float to, float arg0, float arg1)
{
	float range = to - from;
	lut->scale = 1.0f / range;
	lut->bias = -from * lut->scale;
	lut->lut.negative = false;
	for (int i = 0; i < C3D_LIGHT_LUT_SIZE; i++)
		lut->lut.v[i] = func(from + range * i / 256.0f, arg0, arg1);
}

void C3D_LightEnvInit(C3D_LightEnv* env)
{
	memset(env, 0, sizeof *env);
	env->ambient[0] = env->ambient[1] = env->ambient[2] = 1.0f;
}
void C3D_LightEnvBind(C3D_LightEnv* env) { G.lenv = env; }
void C3D_LightEnvMaterial(C3D_LightEnv* env, const C3D_Material* m)
{
	memcpy(env->mat_ambient, m->ambient, sizeof env->mat_ambient);
	memcpy(env->mat_diffuse, m->diffuse, sizeof env->mat_diffuse);
	memcpy(env->mat_specular0, m->specular0, sizeof env->mat_specular0);
	memcpy(env->mat_emission, m->emission, sizeof env->mat_emission);
	env->serial++;
}
void C3D_LightEnvAmbient(C3D_LightEnv* env, float r, float g, float b)
{
	env->ambient[0] = r;
	env->ambient[1] = g;
	env->ambient[2] = b;
}
void C3D_LightEnvLut(C3D_LightEnv* env, GPU_LIGHTLUTID id, GPU_LIGHTLUTINPUT input, bool negative, C3D_LightLut* lut)
{
	if (id > 7)
		return;
	env->lut[id] = lut;
	env->lut_input[id] = input;
	env->lut_abs[id] = !negative;
}

int C3D_LightInit(C3D_Light* l, C3D_LightEnv* env)
{
	int i;
	for (i = 0; i < 8; i++)
		if (!env->lights[i])
			break;
	if (i == 8)
		return -1;
	memset(l, 0, sizeof *l);
	env->lights[i] = l;
	l->parent = env;
	l->id = i;
	l->enabled = true;
	for (int c = 0; c < 3; c++)
		l->diffuse[c] = l->specular0[c] = l->specular1[c] = 1.0f;
	return i;
}
void C3D_LightEnable(C3D_Light* l, bool on) { l->enabled = on; }
void C3D_LightColor(C3D_Light* l, float r, float g, float b)
{
	C3D_LightDiffuse(l, r, g, b);
	C3D_LightSpecular0(l, r, g, b);
	C3D_LightSpecular1(l, r, g, b);
}
void C3D_LightDiffuse(C3D_Light* l, float r, float g, float b) { l->diffuse[0] = r; l->diffuse[1] = g; l->diffuse[2] = b; }
void C3D_LightSpecular0(C3D_Light* l, float r, float g, float b) { l->specular0[0] = r; l->specular0[1] = g; l->specular0[2] = b; }
void C3D_LightSpecular1(C3D_Light* l, float r, float g, float b) { l->specular1[0] = r; l->specular1[1] = g; l->specular1[2] = b; }
void C3D_LightPosition(C3D_Light* l, C3D_FVec* p)
{
	l->pos[0] = p->x;
	l->pos[1] = p->y;
	l->pos[2] = p->z;
	l->positional = p->w != 0.0f;
}
void C3D_LightSpotEnable(C3D_Light* l, bool on) { l->spot_on = on; }
// The direction the light points, normalised. The spot's input in the fragment programs is -L . this, the
// light's way to the fragment against it: 1 on the beam's axis, where the game's tables have their top
void C3D_LightSpotDir(C3D_Light* l, float x, float y, float z)
{
	float n = sqrtf(x * x + y * y + z * z);
	if (n <= 0.0f)
		n = 1.0f;
	l->spot_dir[0] = x / n;
	l->spot_dir[1] = y / n;
	l->spot_dir[2] = z / n;
	l->spot_on = true;
}
void C3D_LightSpotLut(C3D_Light* l, C3D_LightLut* lut)
{
	l->spot = lut;
	l->spot_on = lut != NULL;
}
void C3D_LightDistAttnEnable(C3D_Light* l, bool on) { l->dist_on = on; }
void C3D_LightDistAttn(C3D_Light* l, C3D_LightLutDA* lut)
{
	l->da = lut;
	l->dist_on = lut != NULL;
}

// ---------------------------------------------------------------- procedural texture

void C3D_ProcTexInit(C3D_ProcTex* pt, int offset, int length)
{
	memset(pt, 0, sizeof *pt);
	pt->offset = offset;
	pt->width = length;
}
void C3D_ProcTexClamp(C3D_ProcTex* pt, GPU_PROCTEX_CLAMP u, GPU_PROCTEX_CLAMP v) { pt->clampU = u; pt->clampV = v; }
void C3D_ProcTexCombiner(C3D_ProcTex* pt, bool sep, GPU_PROCTEX_MAPFUNC rgb, GPU_PROCTEX_MAPFUNC alpha)
{
	pt->separateAlpha = sep;
	pt->mapRgbU = rgb;
	pt->mapAlphaU = alpha;
}
void C3D_ProcTexFilter(C3D_ProcTex* pt, GPU_PROCTEX_FILTER min) { pt->filter = min; }
void C3D_ProcTexBind(int tc, C3D_ProcTex* pt)
{
	G.proctex = pt;
	G.proctex_tc = tc;
}
void C3D_ProcTexLutBind(GPU_PROCTEX_LUTID id, C3D_ProcTexLut* lut)
{
	if (id == GPU_LUT_RGBMAP)
		G.pt_rgb = lut;
	else if (id == GPU_LUT_ALPHAMAP)
		G.pt_alpha = lut;
}
void ProcTexLut_FromArray(C3D_ProcTexLut* lut, const float in[129])
{
	for (int i = 0; i < 128; i++)
		(*lut)[i] = in[i];
}
void C3D_ProcTexColorLutBind(C3D_ProcTexColorLut* lut) { G.pt_color = lut; }
void ProcTexColorLut_Write(C3D_ProcTexColorLut* out, const u32* in, int offset, int length)
{
	for (int i = 0; i < length && offset + i < 256; i++)
		out->color[offset + i] = in[i];
}

// ---------------------------------------------------------------- fragment programs

static int s_gpu_test;                 // c3d_gpu_test_next

typedef struct {
	u8 vprog;
	u8 nstages;
	struct { u16 src_rgb, src_a, op_rgb, op_a; u8 f_rgb, f_a, s_rgb, s_a; } st[6];
	u8 lights;                       // count
	u8 lflags[8];                    // bit0 positional, bit1 spot, bit2 distance
	u8 d0_on, d0_input, d0_abs;
	u8 fog, fog_flip;
	u8 alpha_func;                   // 0: no test, else func + 1
	u8 pt_on, pt_tc, pt_clamp_u, pt_clamp_v, pt_rgb, pt_a, pt_sep;
	u8 pt_const, pt_alin;            // the colour table one colour (ptc), the alpha table a line (ptparam.zw)
	u8 rt_mask;                      // texture units holding a render target (GL's rows; not flipped)
	u8 border_mask;                  // texture units clamped to their border (GL has no such wrap)
	u8 halffog;                      // the fog drawn at half size (half_fog_begin): 1 + the depth test, 0 off
	u8 fog_float;                    // its colour work in float (s_fog_float)
} FragKey;

typedef struct {
	FragKey key;
	GLuint prog;
	GLint c, depth, k, fogc, aref, amb, L, d0, tex[3], lut, ptcol, ptparam, ptc;
	u32 unif_gen;
} Program;

#define MAX_PROGS 384
static Program s_progs[MAX_PROGS];
static int s_nprogs;

static void src_expr(char* o, size_t n, int src)
{
	const char* s;
	switch (src) {
	case GPU_PRIMARY_COLOR: s = "o_col"; break;
	case GPU_FRAGMENT_PRIMARY_COLOR: s = "fp"; break;
	case GPU_FRAGMENT_SECONDARY_COLOR: s = "fs"; break;
	case GPU_TEXTURE0: s = "t0"; break;
	case GPU_TEXTURE1: s = "t1"; break;
	case GPU_TEXTURE2: s = "t2"; break;
	case GPU_TEXTURE3: s = "t3"; break;
	case GPU_PREVIOUS_BUFFER: s = "float4(0.0)"; break;
	case GPU_CONSTANT: s = "kc"; break;
	default: s = "prev"; break;
	}
	snprintf(o, n, "%s", s);
}

static void op_rgb(char* o, size_t n, const char* v, int op)
{
	static const char* const SW[16] = { "%s.rgb", "(1.0 - %s.rgb)", "%s.aaa", "(1.0 - %s.aaa)", "%s.rrr",
		"(1.0 - %s.rrr)", "%s.rgb", "%s.rgb", "%s.ggg", "(1.0 - %s.ggg)", "%s.rgb", "%s.rgb", "%s.bbb",
		"(1.0 - %s.bbb)", "%s.rgb", "%s.rgb" };
	snprintf(o, n, SW[op & 15], v, v);
}

static void op_a(char* o, size_t n, const char* v, int op)
{
	static const char* const SW[8] = { "%s.a", "(1.0 - %s.a)", "%s.r", "(1.0 - %s.r)", "%s.g", "(1.0 - %s.g)",
		"%s.b", "(1.0 - %s.b)" };
	snprintf(o, n, SW[op & 7], v, v);
}

static void func_expr(char* o, size_t n, int f, const char* a, const char* b, const char* c, bool alpha)
{
	switch (f) {
	case GPU_REPLACE: snprintf(o, n, "%s", a); break;
	case GPU_MODULATE: snprintf(o, n, "%s * %s", a, b); break;
	case GPU_ADD: snprintf(o, n, "%s + %s", a, b); break;
	case GPU_ADD_SIGNED: snprintf(o, n, "%s + %s - 0.5", a, b); break;
	case GPU_INTERPOLATE: snprintf(o, n, "%s * %s + %s * (1.0 - %s)", a, c, b, c); break;
	case GPU_SUBTRACT: snprintf(o, n, "%s - %s", a, b); break;
	case GPU_DOT3_RGB:
	case GPU_DOT3_RGBA:
		if (alpha)
			snprintf(o, n, "dot3a");
		else
			snprintf(o, n, "float3(4.0 * dot(%s - 0.5, %s - 0.5))", a, b);
		break;
	case GPU_MULTIPLY_ADD: snprintf(o, n, "%s * %s + %s", a, b, c); break;
	case GPU_ADD_MULTIPLY: snprintf(o, n, "min(%s + %s, 1.0) * %s", a, b, c); break;
	default: snprintf(o, n, "%s", a); break;
	}
}

#define APPEND(...) (len += snprintf(src + len, sizeof src - len, __VA_ARGS__))

// The look-up value of table row r at input x (in the table's own [0, 256) index units)
#define LUT_AT "float lutat(sampler2D t, float lrow, float i) { return tex2D(t, float2((i + 0.5) / 256.0, (lrow + 0.5) / 32.0)).x; }\n"

static const char* lut_input(int input)
{
	switch (input) {
	case GPU_LUTINPUT_VH: return "dot(V, H)";
	case GPU_LUTINPUT_NV: return "dot(N, V)";
	case GPU_LUTINPUT_LN: return "dot(Ld, N)";
	case GPU_LUTINPUT_SP: return "dot(-Ld, sd)";
	default: return "dot(N, H)";
	}
}

static const char* pt_map(int f)
{
	switch (f) {
	case GPU_PT_U: return "u";
	case GPU_PT_U2: return "u * u";
	case GPU_PT_V: return "v";
	case GPU_PT_V2: return "v * v";
	case GPU_PT_ADD: return "(u + v) * 0.5";
	case GPU_PT_ADD2: return "(u * u + v * v) * 0.5";
	case GPU_PT_SQRT2: return "sqrt(u * u + v * v)";
	case GPU_PT_MIN: return "min(u, v)";
	case GPU_PT_RMAX: return "((u + v) * 0.5 + sqrt(u * u + v * v)) * 0.5";
	default: return "max(u, v)";
	}
}

static const char* pt_clamp(int m)
{
	switch (m) {
	case GPU_PT_CLAMP_TO_ZERO: return "(x > 1.0 ? 0.0 : x)";
	case GPU_PT_REPEAT: return "frac(x)";
	case GPU_PT_MIRRORED_REPEAT: return "(fmod(floor(x), 2.0) == 0.0 ? frac(x) : 1.0 - frac(x))";
	case GPU_PT_PULSE: return "(x > 0.5 ? 1.0 : 0.0)";
	default: return "min(x, 1.0)";
	}
}

// Compiles through vitashark and hands vitaGL the finished program: vitaGL's own glCompileShader
// fails on every source here (even a one-line shader, on the console and in Vita3K), while the same
// compiler called directly builds them. shark is started by vglInit*.
// Compiled programs are kept on the card by their source's hash: a TEV combination is compiled
// once, not once a boot (each compile is a hitch the first time a material shows; ldhewm3 keeps a
// shader cache for the same reason)
static u64 fnv64(const char* s)
{
	u64 h = 0xcbf29ce484222325ull;
	for (; *s; s++)
		h = (h ^ (u8)*s) * 0x100000001b3ull;
	return h;
}

static GLuint cached(GLenum type, u64 hash)
{
	char path[96];
	snprintf(path, sizeof path, SHADER_CACHE "/%016llx.gxp", (unsigned long long)hash);
	FILE* f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* d = n > 0 ? malloc(n) : NULL;
	bool ok = d && fread(d, 1, n, f) == (size_t)n;
	fclose(f);
	GLuint sh = 0;
	if (ok) {
		sh = glCreateShader(type);
		vglShaderGxpBinary(1, &sh, d, (GLsizei)n);
	}
	free(d);
	return sh;
}

static void cache_put(u64 hash, const void* gxp, u32 size)
{
	char path[96];
	snprintf(path, sizeof path, SHADER_CACHE "/%016llx.gxp", (unsigned long long)hash);
	FILE* f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(gxp, 1, size, f);
	fclose(f);
}

static GLuint compile(GLenum type, const char* s)
{
	// the options are part of the cache key (bumped with them): a program compiled otherwise is not taken
	u64 hash = fnv64(s) ^ (type == GL_VERTEX_SHADER ? 0x5653ull : 0x4653ull) ^ 0x3000000000000ull;
	GLuint hit = cached(type, hash);
	if (hit)
		return hit;
	uint32_t size = (uint32_t)strlen(s);    // in: the source's length; out: the program's size
	// The fragment programs in half precision with fast maths: the SGX does half floats at about twice
	// the rate, and colours, texture coordinates and the TEV's sums need no more (the PICA itself
	// combines in 8 bits a channel). The forest's and the scenes' slow frames were the GPU's (the
	// swap waiting 600 ms a second). Positions keep full precision in the vertex programs.
	// The half-size fog's depth test keeps full precision: it divides clip z by w, and w (the
	// distance, tens of thousands of units for the far sprites) is past half floats' 65504, which
	// made the test fail and every sprite vanish
	bool frag = type != GL_VERTEX_SHADER, depth_test = strstr(s, "zbuf") != NULL || strstr(s, "FOGFLOAT") != NULL;
	SceGxmProgram* gxp = shark_compile_shader_extended(s, &size, frag ? SHARK_FRAGMENT_SHADER : SHARK_VERTEX_SHADER,
		SHARK_OPT_FAST, 1, frag && !depth_test ? 1 : 0, 1);
	if (!gxp) {
		const SceShaccCgCompileOutput* out = shark_get_internal_compile_output();
		vita_log("shader compile failed:");
		for (int i = 0; out && i < out->diagnosticCount; i++) {
			const SceShaccCgDiagnosticMessage* m = &out->diagnostics[i];
			vita_log("  line %u:%u: %s", m->location ? (unsigned)m->location->lineNumber : 0,
				m->location ? (unsigned)m->location->columnNumber : 0, m->message ? m->message : "");
		}
		vita_log("%s", s);
		shark_clear_output();
		return 0;
	}
	GLuint sh = glCreateShader(type);
	vglShaderGxpBinary(1, &sh, gxp, (GLsizei)size);    // copies the program
	cache_put(hash, gxp, size);
	shark_clear_output();
	return sh;
}

// Links p from vs and fs, or leaves it 0 when either failed to compile: vitaGL's glLinkProgram
// reads the compiled programs without checking them
static bool link(GLuint p, GLuint vs, GLuint fs)
{
	if (!vs || !fs)
		return false;
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	return true;
}

// Each vertex program, by the texture coordinates it turns over (bit i: o_tc<i>.y)
static GLuint s_vs[8][8];

// The game's textures keep the PICA's row order, top row first, which the PICA samples at t = 1 and
// GL at t = 0, so their t is turned over. Done here, per vertex: 1 - t is linear, so it interpolates
// to the same value on every pixel, and a fragment program that reads a coordinate as it arrives
// spares the SGX a dependent texture read (Imagination's and Apple's advice for this GPU family; what
// it saves here is to be measured, vflip0.txt puts it back).
static GLuint vertex_shader(int vprog, int flip)
{
	GLuint* vs = &s_vs[vprog][flip];
	if (*vs)
		return *vs;
	static char src[8192];
	const char* base = c3d_vertex_programs[vprog].source;
	int n = snprintf(src, sizeof src, "%.*s", (int)(strrchr(base, '}') - base), base);
	for (int i = 0; i < 3; i++)
		if (flip & (1 << i))
			n += snprintf(src + n, sizeof src - n, "o_tc%d.y = 1.0 - o_tc%d.y;\n", i, i);
	snprintf(src + n, sizeof src - n, "}\n");
	return *vs = compile(GL_VERTEX_SHADER, src);
}

// The half-size fog's program needs full precision for the depth (half overflows in z / w), so it is
// compiled without fastprecision, which would turn every float to half. Its colour work is written
// in half by hand instead, in s[0..]: only the texture coordinates and the depth test stay float.
static int half_types(char* s)
{
	char* w = s;
	for (const char* r = s; *r;) {
		if (!strncmp(r, "float", 5) && strncmp(r + 5, "2(o_tc", 6)) {
			memcpy(w, "half", 4);
			w += 4, r += 5;
		} else {
			*w++ = *r++;
		}
	}
	*w = 0;
	return (int)(w - s);
}

static Program* build(const FragKey* k)
{
	static char src[16384];
	int len = 0;
	bool half_fog = k->halffog && !k->fog_float;
	if (k->halffog && k->fog_float)
		APPEND("// FOGFLOAT\n");
	APPEND(LUT_AT);
	APPEND("%s main(%s o_col : COLOR, float4 o_tc0 : TEXCOORD0, float4 o_tc1 : TEXCOORD1,\n"
	       "float4 o_tc2 : TEXCOORD2, float4 o_view : TEXCOORD4, float4 o_nq : TEXCOORD5, float2 o_z : TEXCOORD6,\n",
	       half_fog ? "half4" : "float4", half_fog ? "half4" : "float4");
	if (k->halffog > 1)
		APPEND("float4 o_scr : TEXCOORD7, uniform sampler2D zbuf,\n");
	APPEND(
	       "uniform sampler2D tex0, uniform sampler2D tex1, uniform sampler2D tex2, uniform sampler2D lut,\n"
	       "uniform sampler2D ptcol, uniform float4 ptparam, uniform float4 ptc, uniform float4 k[6], uniform float4 fogc,\n"
	       "uniform float4 aref, uniform float4 amb, uniform float4 L[48], uniform float4 d0) : COLOR\n{\n");
	int body = len;
	// The game's textures keep the PICA's row order (vertex_shader): their t is turned over, the same
	// on every wrap mode as turning the rows over, and ETC1 stays exactly as packed. Render targets
	// are GL's own rows.
	// only the units some stage reads: the rest are not sampled at all
	bool used[3] = { false, false, false };
	for (int st = 0; st < k->nstages; st++)
		for (int j = 0; j < 3; j++) {
			int a = (k->st[st].src_rgb >> (4 * j)) & 15, b = (k->st[st].src_a >> (4 * j)) & 15;
			for (int i = 0; i < 3; i++)
				used[i] |= a == GPU_TEXTURE0 + i || b == GPU_TEXTURE0 + i;
		}
	// Turned over in the vertex program, unless the procedural texture reads the same coordinate
	// (below, as the game wrote it) or the unit holds a render target. The clamp to border's test
	// below gives the same answer for t and 1 - t.
	int vflip = 0;
	for (int i = 0; i < 3; i++)
		if (s_vflip && used[i] && !(k->rt_mask & (1 << i)) && !(k->pt_on && (k->pt_tc ? 1 : 0) == i))
			vflip |= 1 << i;
	for (int i = 0; i < 3; i++) {
		if (!used[i]) {
			APPEND("float4 t%d = float4(0.0);\n", i);
			continue;
		}
		if ((k->rt_mask | vflip) & (1 << i))
			APPEND("float4 t%d = tex2D(tex%d, o_tc%d.xy);\n", i, i, i);
		else
			APPEND("float4 t%d = tex2D(tex%d, float2(o_tc%d.x, 1.0 - o_tc%d.y));\n", i, i, i, i);
		// GPU_CLAMP_TO_BORDER: outside the texture its border colour, citro3d's default 0 (the movie
		// frames' black surround, movie.c), where CLAMP_TO_EDGE would repeat the last texels
		if (k->border_mask & (1 << i))
			APPEND("if (o_tc%d.x < 0.0 || o_tc%d.x > 1.0 || o_tc%d.y < 0.0 || o_tc%d.y > 1.0) t%d = float4(0.0);\n",
			       i, i, i, i, i);
	}
	APPEND("float4 t3 = float4(0.0);\n");
	if (k->pt_on) {
		const char* tc = k->pt_tc ? "o_tc1" : "o_tc0";
		APPEND("{ float x = abs(%s.x); float u = %s; x = abs(%s.y); float v = %s;\n", tc, pt_clamp(k->pt_clamp_u), tc,
		       pt_clamp(k->pt_clamp_v));
		// Tables that are one colour, or a line, are worked out rather than read: the fog's sprites
		// (fog.c) take their fade from a white colour table and linear maps, and the three reads a
		// pixel were most of what their many overlapping sprites cost the GPU (upload_proctex_consts)
		if (k->pt_const) {
			APPEND("t3 = ptc;\n");
		} else {
			APPEND("float g = %s; float gc = lutat(lut, %d.0, clamp(g, 0.0, 1.0) * 127.0);\n", pt_map(k->pt_rgb),
			       LUT_PT_RGB);
			APPEND("t3 = tex2D(ptcol, float2((ptparam.x + gc * ptparam.y + 0.5) / 256.0, 0.5));\n");
		}
		if (k->pt_sep && k->pt_alin)
			APPEND("float ga = %s; t3.a = clamp(ptparam.z + ptparam.w * clamp(ga, 0.0, 1.0), 0.0, 1.0); }\n",
			       pt_map(k->pt_a));
		else if (k->pt_sep)
			APPEND("float ga = %s; t3.a = lutat(lut, %d.0, clamp(ga, 0.0, 1.0) * 127.0); }\n", pt_map(k->pt_a), LUT_PT_ALPHA);
		else
			APPEND("}\n");
	}
	APPEND("float4 fp = float4(0.0, 0.0, 0.0, 1.0), fs = float4(0.0, 0.0, 0.0, 1.0);\n");
	if (k->lights) {
		APPEND("float4 q = normalize(o_nq);\n"
		       "float3 N = float3(0.0, 0.0, 1.0) + 2.0 * cross(q.xyz, cross(q.xyz, float3(0.0, 0.0, 1.0)) + q.w * float3(0.0, 0.0, 1.0));\n"
		       "float3 V = normalize(o_view.xyz);\n"
		       "float3 dsum = float3(0.0), ssum = float3(0.0);\n");
		for (int i = 0; i < k->lights; i++) {
			int b = i * 6;
			u8 f = k->lflags[i];
			APPEND("{\n");
			if (f & 1)
				APPEND("float3 Lv = L[%d].xyz + o_view.xyz;\n", b);
			else
				APPEND("float3 Lv = L[%d].xyz;\n", b);
			// Ld, not L: a local L would hide the light uniforms L[]
			APPEND("float3 Ld = normalize(Lv); float3 H = normalize(Ld + V); float3 sd = L[%d].xyz;\n", b + 1);
			APPEND("float nl = dot(Ld, N); float dif = max(nl, 0.0); float att = 1.0;\n");
			if (f & 2)
				APPEND("att *= lutat(lut, %d.0, clamp(dot(-Ld, sd), -1.0, 0.996) * 128.0 + 128.0);\n", LUT_LIGHT0 + i * 2);
			if (f & 4)
				APPEND("att *= lutat(lut, %d.0, clamp(length(Lv) * L[%d].x + L[%d].y, 0.0, 1.0) * 255.0);\n",
				       LUT_LIGHT0 + i * 2 + 1, b + 5, b + 5);
			if (k->d0_on) {
				if (k->d0_abs)
					APPEND("float dd = lutat(lut, %d.0, min(abs(%s), 0.996) * 256.0);\n", LUT_D0, lut_input(k->d0_input));
				else
					APPEND("float dd = lutat(lut, %d.0, clamp(%s, -1.0, 0.996) * 128.0 + 128.0);\n", LUT_D0,
					       lut_input(k->d0_input));
			} else {
				APPEND("float dd = 1.0;\n");
			}
			APPEND("dsum += (L[%d].rgb * dif + L[%d].rgb) * att;\n", b + 2, b + 3);
			APPEND("ssum += L[%d].rgb * dd * (nl > 0.0 ? 1.0 : 0.0) * att;\n}\n", b + 4);
		}
		APPEND("fp = float4(clamp(dsum + amb.rgb, 0.0, 1.0), 1.0);\nfs = float4(clamp(ssum, 0.0, 1.0), 1.0);\n");
	}
	APPEND("float4 prev = float4(0.0);\nfloat4 kc;\n");
	static const char* const SCALE[4] = { "1.0", "2.0", "4.0", "1.0" };
	for (int i = 0; i < k->nstages; i++) {
		char s[3][24], a[3][48], b[3][48], e[512], ea[512];
		for (int j = 0; j < 3; j++) {
			char v[24];
			src_expr(v, sizeof v, (k->st[i].src_rgb >> (j * 4)) & 15);
			op_rgb(a[j], sizeof a[j], v, (k->st[i].op_rgb >> (j * 4)) & 15);
			src_expr(s[j], sizeof s[j], (k->st[i].src_a >> (j * 4)) & 15);
			op_a(b[j], sizeof b[j], s[j], (k->st[i].op_a >> (j * 4)) & 7);
		}
		APPEND("kc = k[%d];\n", i);
		bool dot3a = k->st[i].f_rgb == GPU_DOT3_RGBA;
		func_expr(e, sizeof e, k->st[i].f_rgb, a[0], a[1], a[2], false);
		if (dot3a) {
			snprintf(ea, sizeof ea, "4.0 * dot(%s - 0.5, %s - 0.5)", a[0], a[1]);
		} else {
			func_expr(ea, sizeof ea, k->st[i].f_a, b[0], b[1], b[2], true);
			if (!strcmp(ea, "dot3a"))
				snprintf(ea, sizeof ea, "%s", b[0]);
		}
		APPEND("{ float3 cr = clamp((%s) * %s, 0.0, 1.0); float ca = clamp((%s) * %s, 0.0, 1.0); prev = float4(cr, ca); }\n",
		       e, SCALE[k->st[i].s_rgb & 3], ea, SCALE[k->st[i].s_a & 3]);
	}
	if (half_fog)
		len = body + half_types(src + body);
	if (k->halffog > 1) {
		// the half-size fog's depth test against the scene's depth (the frame's own depth buffer as a
		// texture): a sprite behind the map leaves the pixel alone, as GL's test would have
		// (abs: the depth format keeps its mask bit in the sign, so stored values read negative)
		static const char* const CMPZ[8] = { "false", "true", "==", "!=", "<", "<=", ">", ">=" };
		int f = k->halffog - 2;
		APPEND("{ float2 suv = o_scr.xy / o_scr.w * 0.5 + 0.5; float sz = abs(tex2D(zbuf, suv).x);\n"
		       "float fz = o_scr.z / o_scr.w * 0.5 + 0.5;\n");
		if (f <= 1)
			APPEND("if (!%s) prev.a = 0.0; }\n", CMPZ[f]);
		else
			APPEND("if (!(fz %s sz)) prev.a = 0.0; }\n", CMPZ[f]);
	}
	if (k->alpha_func) {
		static const char* const CMP[8] = { "false", "true", "==", "!=", "<", "<=", ">", ">=" };
		int f = k->alpha_func - 1;
		if (f == GPU_NEVER)
			APPEND("discard;\n");
		else if (f != GPU_ALWAYS)
			APPEND("if (!(floor(prev.a * 255.0 + 0.5) %s aref.x)) discard;\n", CMP[f]);
	}
	if (k->fog) {
		APPEND("{ float dz = clamp(o_z.x / o_z.y, 0.0, 1.0);\n");
		if (k->fog_flip)
			APPEND("dz = 1.0 - dz;\n");
		APPEND("float ff = lutat(lut, %d.0, dz * 128.0);\nprev.rgb = ff * prev.rgb + (1.0 - ff) * fogc.rgb; }\n", LUT_FOG);
	}
	APPEND("return prev;\n}\n");

	if (s_nprogs >= MAX_PROGS) {
		vita_log("fragment program cache full");
		s_nprogs = 0;
	}
	GLuint vs = vertex_shader(k->vprog, vflip);
	static int dump = -1;             // ux0:data/sh2-vita/dumpshaders.txt: every fragment program into the log
	if (dump < 0) {
		FILE* f = fopen(VITA_DATA_DIR "/dumpshaders.txt", "r");
		dump = f != NULL;
		if (f)
			fclose(f);
	}
	if (dump)
		vita_log("fragment program for %s (%d stages%s%s):\n%s", c3d_vertex_programs[k->vprog].name, k->nstages,
		         k->pt_on ? ", proctex" : "", k->halffog ? ", half fog" : "", src);
	GLuint fs = compile(GL_FRAGMENT_SHADER, src);
	if (!fs && half_fog) {
		vita_log("half-size fog in half precision did not compile: float throughout");
		s_fog_float = true;
		FragKey fk = *k;
		fk.fog_float = 1;
		return build(&fk);
	}
	Program* p = &s_progs[s_nprogs++];
	memset(p, 0, sizeof *p);
	p->key = *k;
	p->prog = glCreateProgram();
	if (!link(p->prog, vs, fs)) {
		vita_log("program %s not built", c3d_vertex_programs[k->vprog].name);
		glDeleteProgram(p->prog);
		p->prog = 0;
		return p;
	}
	for (int i = 0; i < c3d_vertex_programs[k->vprog].ninputs; i++) {
		char n[4] = { 'v', (char)('0' + i), 0 };
		glBindAttribLocation(p->prog, i, n);
	}
	glLinkProgram(p->prog);
	p->c = glGetUniformLocation(p->prog, "c");
	p->depth = glGetUniformLocation(p->prog, "u_depth");
	p->k = glGetUniformLocation(p->prog, "k");
	p->fogc = glGetUniformLocation(p->prog, "fogc");
	p->aref = glGetUniformLocation(p->prog, "aref");
	p->amb = glGetUniformLocation(p->prog, "amb");
	p->L = glGetUniformLocation(p->prog, "L");
	p->d0 = glGetUniformLocation(p->prog, "d0");
	p->lut = glGetUniformLocation(p->prog, "lut");
	p->ptcol = glGetUniformLocation(p->prog, "ptcol");
	p->ptparam = glGetUniformLocation(p->prog, "ptparam");
	p->ptc = glGetUniformLocation(p->prog, "ptc");
	static const char* const TN[3] = { "tex0", "tex1", "tex2" };
	for (int i = 0; i < 3; i++)
		p->tex[i] = glGetUniformLocation(p->prog, TN[i]);
	glUseProgram(p->prog);
	for (int i = 0; i < 3; i++)
		if (p->tex[i] >= 0)
			glUniform1i(p->tex[i], i);
	if (p->lut >= 0)
		glUniform1i(p->lut, 3);
	if (p->ptcol >= 0)
		glUniform1i(p->ptcol, 4);
	GLint zb = glGetUniformLocation(p->prog, "zbuf");
	if (zb >= 0)
		glUniform1i(zb, 6);
	p->unif_gen = (u32)-1;
	return p;
}

static bool stage_used(const C3D_TexEnv* e)
{
	// a stage that hands on the previous result unchanged
	return !(e->srcRgb == GPU_PREVIOUS && e->srcAlpha == GPU_PREVIOUS && e->funcRgb == GPU_REPLACE
	         && e->funcAlpha == GPU_REPLACE && (e->opAll & 0xFFF) == 0 && ((e->opAll >> 12) & 0xFFF) == 0
	         && e->scaleRgb == 0 && e->scaleAlpha == 0);
}

// An alpha test that can't change the picture: alpha > 0 on a blend whose source counts by its
// alpha (SRC_ALPHA onto ONE_MINUS_SRC_ALPHA or ONE), with no depth or stencil written. A texel it
// would cut leaves the framebuffer as it was anyway. Left out of the fragment program: on the SGX
// a discard keeps the hidden surface removal from working for the draw, and the fog's sprites
// (fog.c), drawn just so over the screen many times, paid for it on every pixel.
static bool alpha_test_moot(void)
{
	if ((G.alpha_func != GPU_GREATER && G.alpha_func != GPU_NOTEQUAL) || G.alpha_ref != 0)
		return false;
	if ((F.depth_on && (F.depth_mask & GPU_WRITE_DEPTH)) || F.stencil_on || F.logic_copy)
		return false;
	bool keep_dst_c = F.bdst_c == GPU_ONE_MINUS_SRC_ALPHA || F.bdst_c == GPU_ONE;
	bool keep_dst_a = F.bdst_a == GPU_ONE_MINUS_SRC_ALPHA || F.bdst_a == GPU_ONE;
	if (F.beq_c != GPU_BLEND_ADD || F.bsrc_c != GPU_SRC_ALPHA || !keep_dst_c)
		return false;
	if ((F.depth_mask & 8) && (F.beq_a != GPU_BLEND_ADD || F.bsrc_a != GPU_SRC_ALPHA || !keep_dst_a))
		return false;
	return true;
}

// The procedural texture's colour table over its span, when it is one colour
static u32 s_pt_colour;
static float s_pt_a0, s_pt_a1;          // the alpha table as a line, a0 + a1 x (x 0..1)

static bool pt_colour_const(u32* c)
{
	if (!G.pt_color || !G.proctex)
		return false;
	int o = G.proctex->offset, n = G.proctex->width;
	if (o < 0 || n < 1 || o + n > 256)
		return false;
	for (int i = o + 1; i < o + n; i++)
		if (G.pt_color->color[i] != G.pt_color->color[o])
			return false;
	*c = G.pt_color->color[o];
	return true;
}

// The 128-entry table as v0 + (v127 - v0) x to within half a step of its 8-bit texture, which
// lutat reads at x * 127 with linear filtering
static bool lut_line(const C3D_ProcTexLut* l, float* a, float* b)
{
	float v0 = (*l)[0], v1 = (*l)[127];
	for (int i = 1; i < 127; i++) {
		float e = (*l)[i] - (v0 + (v1 - v0) * i / 127.0f);
		if (e > 0.5f / 255.0f || e < -0.5f / 255.0f)
			return false;
	}
	*a = v0, *b = v1 - v0;
	return true;
}

static Program* current_program(void)
{
	FragKey k;
	memset(&k, 0, sizeof k);
	k.vprog = (u8)G.program;
	for (int i = 0; i < 3; i++)
		if (G.tex[i] && G.tex[i]->render_target)
			k.rt_mask |= (u8)(1 << i);
	for (int i = 0; i < 3; i++)
		if (G.tex[i] && (((G.tex[i]->param >> 12) & 3) == GPU_CLAMP_TO_BORDER
		                 || ((G.tex[i]->param >> 8) & 3) == GPU_CLAMP_TO_BORDER))
			k.border_mask |= (u8)(1 << i);
	int last = -1;
	for (int i = 0; i < 6; i++)
		if (stage_used(&G.env[i]))
			last = i;
	k.nstages = (u8)(last + 1);
	for (int i = 0; i < k.nstages; i++) {
		const C3D_TexEnv* e = &G.env[i];
		k.st[i].src_rgb = e->srcRgb;
		k.st[i].src_a = e->srcAlpha;
		k.st[i].op_rgb = e->opAll & 0xFFF;
		k.st[i].op_a = (e->opAll >> 12) & 0xFFF;
		k.st[i].f_rgb = (u8)e->funcRgb;
		k.st[i].f_a = (u8)e->funcAlpha;
		k.st[i].s_rgb = (u8)e->scaleRgb;
		k.st[i].s_a = (u8)e->scaleAlpha;
	}
	C3D_LightEnv* env = G.lenv;
	if (env && c3d_vertex_programs[G.program].outputs_light) {
		int n = 0;
		for (int i = 0; i < 8; i++) {
			C3D_Light* l = env->lights[i];
			if (!l || !l->enabled)
				continue;
			k.lflags[n] = (l->positional ? 1 : 0) | (l->spot_on && l->spot ? 2 : 0) | (l->dist_on && l->da ? 4 : 0);
			n++;
		}
		k.lights = (u8)n;
		if (env->lut[GPU_LUT_D0]) {
			k.d0_on = 1;
			k.d0_input = (u8)env->lut_input[GPU_LUT_D0];
			k.d0_abs = env->lut_abs[GPU_LUT_D0];
		}
	}
	if (s_gpu_test == 1)
		k.lights = 0, k.d0_on = 0;
	if (HF.active)
		k.halffog = (u8)(F.depth_on && !HF.no_test ? 2 + (F.depth_func & 7) : 1), k.fog_float = s_fog_float;
	k.fog = G.fog_mode == GPU_FOG && G.fog_lut;
	k.fog_flip = k.fog && G.fog_flip;
	k.alpha_func = G.alpha_test && !alpha_test_moot() ? (u8)(G.alpha_func + 1) : 0;
	if (G.proctex) {
		k.pt_on = 1;
		k.pt_tc = (u8)G.proctex_tc;
		k.pt_clamp_u = (u8)G.proctex->clampU;
		k.pt_clamp_v = (u8)G.proctex->clampV;
		k.pt_rgb = (u8)G.proctex->mapRgbU;
		k.pt_a = (u8)G.proctex->mapAlphaU;
		k.pt_sep = G.proctex->separateAlpha;
		k.pt_const = pt_colour_const(&s_pt_colour);
		k.pt_alin = k.pt_sep && G.pt_alpha && lut_line(G.pt_alpha, &s_pt_a0, &s_pt_a1);
	}
	for (int i = 0; i < s_nprogs; i++)
		if (!memcmp(&s_progs[i].key, &k, sizeof k))
			return &s_progs[i];
	return build(&k);
}

static void colour3(float* o, const float* a, const float* b)
{
	o[0] = a[0] * b[0];
	o[1] = a[1] * b[1];
	o[2] = a[2] * b[2];
}

static void upload_luts(void)
{
	C3D_LightEnv* env = G.lenv;
	if (env && env->lut[GPU_LUT_D0])
		for (int i = 0; i < 256; i++)
			G.lut_img[LUT_D0][i] = lut8(env->lut[GPU_LUT_D0]->v[i]);
	if (G.fog_lut)
		for (int i = 0; i < 256; i++)
			G.lut_img[LUT_FOG][i] = lut8(G.fog_lut->v[i < 128 ? i : 128]);
	if (env) {
		int n = 0;
		for (int i = 0; i < 8; i++) {
			C3D_Light* l = env->lights[i];
			if (!l || !l->enabled)
				continue;
			if (l->spot)
				for (int j = 0; j < 256; j++)
					G.lut_img[LUT_LIGHT0 + n * 2][j] = lut8(l->spot->v[j]);
			if (l->da)
				for (int j = 0; j < 256; j++)
					G.lut_img[LUT_LIGHT0 + n * 2 + 1][j] = lut8(l->da->lut.v[j]);
			n++;
		}
	}
	if (G.proctex) {
		for (int i = 0; i < 128; i++) {
			G.lut_img[LUT_PT_RGB][i] = G.pt_rgb ? lut8((*G.pt_rgb)[i]) : 0;
			G.lut_img[LUT_PT_ALPHA][i] = G.pt_alpha ? lut8((*G.pt_alpha)[i]) : 0;
		}
		if (G.pt_color) {
			for (int i = 0; i < 256; i++)
				G.ptcol_img[i] = G.pt_color->color[i];
			if (memcmp(G.ptcol_img, G.ptcol_sent, sizeof G.ptcol_img)) {
				glActiveTexture(GL_TEXTURE4);
				glBindTexture(GL_TEXTURE_2D, G.ptcol_tex);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, G.ptcol_img);
				memcpy(G.ptcol_sent, G.ptcol_img, sizeof G.ptcol_img);
			}
		}
	}
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, G.lut_tex);
	if (memcmp(G.lut_img, G.lut_sent, sizeof G.lut_img)) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, LUT_ROWS, GL_LUMINANCE, GL_UNSIGNED_BYTE, G.lut_img);
		memcpy(G.lut_sent, G.lut_img, sizeof G.lut_img);
	}
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, G.ptcol_tex);
}

static void upload_uniforms(Program* p)
{
	const VertexProgram* vp = &c3d_vertex_programs[p->key.vprog];
	static float buf[NREGS][4];
	if (p->unif_gen != G.unif_gen) {
		for (int i = 0; i < vp->nregs; i++) {
			buf[i][0] = G.unif[i].x;
			buf[i][1] = G.unif[i].y;
			buf[i][2] = G.unif[i].z;
			buf[i][3] = G.unif[i].w;
		}
		glUniform4fv(p->c, vp->nregs, &buf[0][0]);
		p->unif_gen = G.unif_gen;
	}
	glUniform4f(p->depth, G.depth_scale, G.depth_offset, 0.0f, 0.0f);
	float kc[6][4];
	for (int i = 0; i < 6; i++) {
		u32 c = G.env[i].color;
		kc[i][0] = (c & 255) / 255.0f;
		kc[i][1] = (c >> 8 & 255) / 255.0f;
		kc[i][2] = (c >> 16 & 255) / 255.0f;
		kc[i][3] = (c >> 24) / 255.0f;
	}
	if (p->k >= 0)
		glUniform4fv(p->k, 6, &kc[0][0]);
	if (p->fogc >= 0) {
		u32 c = G.fog_color;
		glUniform4f(p->fogc, (c & 255) / 255.0f, (c >> 8 & 255) / 255.0f, (c >> 16 & 255) / 255.0f, 1.0f);
	}
	if (p->aref >= 0)
		glUniform4f(p->aref, (float)G.alpha_ref, 0.0f, 0.0f, 0.0f);
	if (p->ptparam >= 0 && G.proctex)
		glUniform4f(p->ptparam, (float)G.proctex->offset, (float)(G.proctex->width - 1), s_pt_a0, s_pt_a1);
	if (p->ptc >= 0 && p->key.pt_const) {
		u32 c = s_pt_colour;
		glUniform4f(p->ptc, (c & 255) / 255.0f, (c >> 8 & 255) / 255.0f, (c >> 16 & 255) / 255.0f, (c >> 24) / 255.0f);
	}
	C3D_LightEnv* env = G.lenv;
	if (p->key.lights && env) {
		float amb[3];
		colour3(amb, env->mat_ambient, env->ambient);
		for (int c = 0; c < 3; c++)
			amb[c] += env->mat_emission[c];
		glUniform4f(p->amb, amb[0], amb[1], amb[2], 1.0f);
		static float L[48][4];
		memset(L, 0, sizeof L);
		int n = 0;
		for (int i = 0; i < 8; i++) {
			C3D_Light* l = env->lights[i];
			if (!l || !l->enabled)
				continue;
			float* b = L[n * 6];
			memcpy(b, l->pos, 3 * sizeof(float));
			memcpy(b + 4, l->spot_dir, 3 * sizeof(float));
			colour3(b + 8, l->diffuse, env->mat_diffuse);
			colour3(b + 12, l->ambient, env->mat_ambient);
			colour3(b + 16, l->specular0, env->mat_specular0);
			if (l->da) {
				b[20] = l->da->scale;
				b[21] = l->da->bias;
			}
			n++;
		}
		glUniform4fv(p->L, n * 6, &L[0][0]);
	}
}

// Options' Resolution: at once, and the file vita_gl_init reads before the options are, at a start
static int s_res_want = -1;

// At a frame's start: the game view's surface made again at the size chosen, under the same target and
// texture the game holds, and the half-size fog's after it
static void res_apply(void)
{
	if (s_res_want < 0)
		return;
	int w = s_res_want ? 416 : 544, h = s_res_want ? 720 : 960;
	s_res_want = -1;
	if (w == s_view_w)
		return;
	glFinish();
	for (int i = 0; i < MAX_TARGETS; i++) {
		C3D_RenderTarget* t = G.targets[i];
		if (!t || !((t->width == 480 && t->height == 800) || (t->width == 240 && t->height == 400)))
			continue;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &t->fbo);
		bool depth = t->depth_rb != 0;
		if (depth)
			glDeleteRenderbuffers(1, &t->depth_rb);
		t->fbo = t->depth_rb = 0;
		glDeleteTextures(1, &t->tex->gl);
		t->tex->gl = 0;
		t->gl_w = (u16)w, t->gl_h = (u16)h;
		t->sx = (float)w / t->width, t->sy = (float)h / t->height;
		c3d_tex_make_target(t->tex, w, h);
		make_fbo(t, depth ? GPU_RB_DEPTH24_STENCIL8 : (GPU_DEPTHBUF)-1);
	}
	if (HF.rt.fbo)
		hf_free();
	s_view_w = w, s_view_h = h;
	G.target = NULL;
}

void vita_res_save(int low)
{
	static int saved = -1;
	if (low == saved)
		return;
	if (saved >= 0)
		s_res_want = low;            // a change in the Options: now (the file is for the next start)
	saved = low;
	if (low) {
		FILE* f = fopen(VITA_DATA_DIR "/res416.txt", "w");
		if (f)
			fclose(f);
	} else {
		sceIoRemove(VITA_DATA_DIR "/res416.txt");
	}
}

// false when there is nothing to draw with: no shader bound, or its program failed to build
// ---------------------------------------------------------------- the fog at half size
// The fog's sprites (fog.c) are most of the GPU's frame on the fogged roads: hundreds of large
// blended sprites over the scene, 10 to 12 screens of them. Drawn into a target of half the size
// each way they cost a quarter. The small target has no depth of its own: by default the sprites
// are not tested at all (c3d_half_fog_toggle), and with the test on the scene's depth is made a
// texture (vglTexImageDepthBuffer) and each sprite pixel tests itself against it (build: halffog). The target starts transparent and gathers the sprites as the screen would, the
// colour by the game's blend and the alpha as coverage (ONE, ONE_MINUS_SRC_ALPHA), so laying it
// over the scene with ONE, ONE_MINUS_SRC_ALPHA (half_fog_end) gives the sprites' own result, at
// half the resolution. SELECT + L (ctru.c) turns it off and on to compare.
static void bind_target(C3D_RenderTarget* t);
static void make_fbo(C3D_RenderTarget* t, GPU_DEPTHBUF depth);

static void half_fog_end(void)
{
	if (!HF.active)
		return;
	HF.active = false;
	G.target = NULL;
	C3D_RenderTarget* m = HF.main;
	bind_target(m);
	glViewport(0, 0, m->gl_w, m->gl_h);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(s_present_prog);
	glUniform1i(s_present_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, HF.tex.gl);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	static const float pos[8] = { -1, -1, 1, -1, -1, 1, 1, 1 }, uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (HF.show_depth) {             // the test state: the depth texture over everything
		glDisable(GL_BLEND);
		glBindTexture(GL_TEXTURE_2D, HF.ztex);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
}

static int s_fog_div = 3;

void vita_fog_div(int div) { s_fog_div = div < 2 ? 2 : div > 4 ? 4 : div; }

static void hf_free(void)
{
	glDeleteFramebuffers(1, &HF.rt.fbo);
	glDeleteTextures(1, &HF.tex.gl);
	glDeleteTextures(1, &HF.ztex);
	memset(&HF.rt, 0, sizeof HF.rt);
	memset(&HF.tex, 0, sizeof HF.tex);
	HF.ztex = 0, HF.zdata = NULL;
}

static void half_fog_begin(void)
{
	C3D_RenderTarget* m = G.target;
	// whole multiples of 8: a third's 181 left a surface wider than the picture, its last column a
	// light line along the top of the screen
	int w = (m->gl_w / s_fog_div) & ~7, h = (m->gl_h / s_fog_div) & ~7;
	if (HF.rt.fbo && (HF.rt.gl_w != w || HF.rt.gl_h != h))
		hf_free();
	if (!HF.rt.fbo) {
		memset(&HF.tex, 0, sizeof HF.tex);
		HF.tex.width = (u16)w, HF.tex.height = (u16)h;
		HF.tex.param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR);
		c3d_tex_make_target(&HF.tex, w, h);
		HF.rt.tex = &HF.tex;
		HF.rt.width = HF.rt.gl_w = (u16)w, HF.rt.height = HF.rt.gl_h = (u16)h;
		HF.rt.sx = HF.rt.sy = 1.0f;
		HF.rt.screen = -1;
		make_fbo(&HF.rt, (GPU_DEPTHBUF)-1);
		glGenTextures(1, &HF.ztex);
	}
	HF.passes++;
	HF.zdata = NULL;
	if (!HF.no_test) {
		// the scene's depth as a texture: m is bound for reading and drawing
		glActiveTexture(GL_TEXTURE6);
		glBindTexture(GL_TEXTURE_2D, HF.ztex);
		vglTexImageDepthBuffer(GL_TEXTURE_2D);
		HF.zdata = sceGxmTextureGetData(vglGetGxmTexture(GL_TEXTURE_2D));   // vitaGL keeps no data pointer for it
		HF.zw = m->gl_w, HF.zh = m->gl_h;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glActiveTexture(GL_TEXTURE0);
	}
	HF.main = m;
	G.target = NULL;
	bind_target(&HF.rt);
	glViewport(0, 0, w, h);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	HF.active = true;
}

// Before a draw: the fog's draws on the screen's 2x2 target go to the half-size one, and the first
// other draw lays it back over the scene
static void half_fog_route(void)
{
	if (HF.fog_prog == -2) {
		HF.fog_prog = -1;
		for (int i = 0; i < c3d_vertex_program_count; i++)
			if (!strcmp(c3d_vertex_programs[i].name, "fog"))
				HF.fog_prog = i;
	}
	bool fog = G.program == HF.fog_prog;
	if (HF.active && !fog)
		half_fog_end();
	else if (!HF.active && fog && HF.on && G.target && G.target->gl_w == s_view_w && G.target->gl_h == s_view_h)
		half_fog_begin();
}

// The current target's picture larger round its middle, 1 as usual: what falls outside is cut off
void vita_view_zoom(float zoom)
{
	C3D_RenderTarget* t = G.target;
	if (!t)
		return;
	int w = (int)(t->gl_w * zoom + 0.5f), h = (int)(t->gl_h * zoom + 0.5f);
	glViewport((t->gl_w - w) / 2, (t->gl_h - h) / 2, w, h);
}

// The item screen's still (0x46f180 samples a texture of the frame, 0x46a150): the game view copied
// once into a small target as the menu opens, then laid into the status panel. Both copies keep the
// surface's portrait rows, as half_fog_end's does; blit says how they lie on the display.
static struct { C3D_RenderTarget rt; C3D_Tex tex; bool ready; } CAP;

bool vita_capture_ready(void) { return CAP.ready; }
void vita_capture_reset(void) { CAP.ready = false; }

static void quad_state(void)
{
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_BLEND);
	glUseProgram(s_present_prog);
	glUniform1i(s_present_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
}

void vita_capture_view(void)
{
	half_fog_end();                      // the fog's own target is the one bound until then
	C3D_RenderTarget* m = G.target;
	if (CAP.ready || !m || m->gl_w != s_view_w || m->gl_h != s_view_h)
		return;
	if (!CAP.rt.fbo) {
		CAP.tex.width = 272, CAP.tex.height = 480;
		CAP.tex.param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR);
		c3d_tex_make_target(&CAP.tex, 272, 480);
		CAP.rt.tex = &CAP.tex;
		CAP.rt.width = CAP.rt.gl_w = 272, CAP.rt.height = CAP.rt.gl_h = 480;
		CAP.rt.sx = CAP.rt.sy = 1.0f;
		CAP.rt.screen = -1;
		make_fbo(&CAP.rt, (GPU_DEPTHBUF)-1);
	}
	G.target = NULL;
	bind_target(&CAP.rt);
	glViewport(0, 0, 272, 480);
	quad_state();
	glBindTexture(GL_TEXTURE_2D, m->tex->gl);
	static const float pos[8] = { -1, -1, 1, -1, -1, 1, 1, 1 }, uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	G.target = NULL;
	bind_target(m);
	glViewport(0, 0, m->gl_w, m->gl_h);
	CAP.ready = true;
}

// Into the game view, in the 400 x 240 layout: a display position (xs, ys) of 960 x 544 is the surface's
// (1 - ys / 544, 1 - xs / 960) (blit), and the still's whole picture fills the rectangle
void vita_capture_draw(float x0, float y0, float x1, float y1)
{
	if (!CAP.ready || !G.target)
		return;
	quad_state();
	glBindTexture(GL_TEXTURE_2D, CAP.tex.gl);
	float pos[8], uv[8];
	static const float F[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
	for (int i = 0; i < 4; i++) {
		float fx = F[i][0], fy = F[i][1];
		float xs = (x0 + (x1 - x0) * fx) / 400.0f, ys = (y0 + (y1 - y0) * fy) / 240.0f;
		pos[i * 2] = 2.0f * (1.0f - ys) - 1.0f, pos[i * 2 + 1] = 2.0f * (1.0f - xs) - 1.0f;
		uv[i * 2] = 1.0f - fy, uv[i * 2 + 1] = 1.0f - fx;
	}
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// The PC's 2D filters (0x479840), cut down to what the Vita can pay for: each is a copy of the view into
// a small target and one blended quad back. They run only while a scene asks for them, so play has no
// cost. The surface's portrait rows are kept, as in vita_capture_view.
static struct { C3D_RenderTarget rt; C3D_Tex tex; bool ready; } FXM, FXS;
static GLuint s_fx_prog;
static GLint s_fx_tex, s_fx_alpha;
static GLuint s_fx4_prog;            // vita_fx_blur2
static GLint s_fx4_tex, s_fx4_off, s_fx4_alpha;

static void fx_target(C3D_RenderTarget* rt, C3D_Tex* tex, int w, int h)
{
	if (rt->fbo)
		return;
	tex->width = w, tex->height = h;
	tex->param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR);
	c3d_tex_make_target(tex, w, h);
	rt->tex = tex;
	rt->width = rt->gl_w = w, rt->height = rt->gl_h = h;
	rt->sx = rt->sy = 1.0f;
	rt->screen = -1;
	make_fbo(rt, (GPU_DEPTHBUF)-1);
}

static const float FX_POS[8] = { -1, -1, 1, -1, -1, 1, 1, 1 }, FX_UV[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };

static void fx_copy(C3D_RenderTarget* m, C3D_RenderTarget* to)
{
	G.target = NULL;
	bind_target(to);
	glViewport(0, 0, to->gl_w, to->gl_h);
	quad_state();
	glBindTexture(GL_TEXTURE_2D, m->tex->gl);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, FX_POS);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, FX_UV);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	G.target = NULL;
	bind_target(m);
	glViewport(0, 0, m->gl_w, m->gl_h);
}

static void fx_over(GLuint tex, float alpha)
{
	quad_state();
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(s_fx_prog);
	glUniform1i(s_fx_tex, 0);
	glUniform1f(s_fx_alpha, alpha);
	glBindTexture(GL_TEXTURE_2D, tex);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, FX_POS);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, FX_UV);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static C3D_RenderTarget* fx_view(void)
{
	half_fog_end();                      // the fog's own target is the one bound until then
	C3D_RenderTarget* m = G.target;
	return s_fx_prog && m && m->gl_w == s_view_w && m->gl_h == s_view_h ? m : NULL;
}

void vita_fx_motion_reset(void) { FXM.ready = false; }

// 0x478de0 (mode 2): the last result over this frame, at the game's alpha * 2 of 255 (alpha << 25), and
// the result kept for the next. The PC keeps it full size; here it is half each way.
void vita_fx_motion(int alpha)
{
	C3D_RenderTarget* m = fx_view();
	if (!m)
		return;
	fx_target(&FXM.rt, &FXM.tex, 272, 480);
	if (FXM.ready)
		fx_over(FXM.tex.gl, alpha * 2 / 255.0f);
	fx_copy(m, &FXM.rt);
	FXM.ready = true;
}

// Stand-in for 0x4791e0 + 0x477a60 (modes 5, 6, 8): the PC shrinks the view and lays it back in several
// offset passes; here it is one copy at a quarter each way, its bilinear filter giving the softness.
void vita_fx_soft(float alpha)
{
	C3D_RenderTarget* m = fx_view();
	if (!m)
		return;
	fx_target(&FXS.rt, &FXS.tex, 136, 240);
	fx_copy(m, &FXS.rt);
	fx_over(FXS.tex.gl, alpha);
}

// 0x479840 mode 8 (0x4791e0, 0x477a60): the view shrunk by 2^level (up to 3) for each of two strengths and
// laid back through four taps a side, across and then down, at -1, -1/3, 1/3 and 1 of the strength in the
// PC's 640 x 480 pixels. The small level 0 of the PC isn't kept: at least half size here. The second is
// blended by the frame's alpha on the PC; half and half here (stand-in, port/STATUS.md).
static struct { C3D_RenderTarget rt; C3D_Tex tex; } FXB[3], FXT[3];

static void fx_blur_pass(C3D_RenderTarget* to, GLuint tex, float ou, float ov, float alpha)
{
	G.target = NULL;
	bind_target(to);
	glViewport(0, 0, to->gl_w, to->gl_h);
	quad_state();
	if (alpha < 1.0f) {
		glEnable(GL_BLEND);
		glBlendEquation(GL_FUNC_ADD);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	glUseProgram(s_fx4_prog);
	glUniform1i(s_fx4_tex, 0);
	glUniform1f(s_fx4_alpha, alpha);
	glUniform4f(s_fx4_off, ou, ov, ou / 3.0f, ov / 3.0f);
	glBindTexture(GL_TEXTURE_2D, tex);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, FX_POS);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, FX_UV);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void vita_fx_blur2(float a, float b)
{
	C3D_RenderTarget* m = fx_view();
	if (!m || !s_fx4_prog)
		return;
	float str[2] = { a, b };
	int lv[2], top = 0;
	for (int i = 0; i < 2; i++) {
		lv[i] = (int)str[i] < 1 ? 1 : (int)str[i] > 3 ? 3 : (int)str[i];
		top = lv[i] > top ? lv[i] : top;
	}
	C3D_RenderTarget* from = m;
	for (int l = 0; l < top; l++) {
		fx_target(&FXB[l].rt, &FXB[l].tex, s_view_w >> (l + 1), s_view_h >> (l + 1));
		fx_target(&FXT[l].rt, &FXT[l].tex, s_view_w >> (l + 1), s_view_h >> (l + 1));
		G.target = NULL;
		bind_target(&FXB[l].rt);
		glViewport(0, 0, FXB[l].rt.gl_w, FXB[l].rt.gl_h);
		quad_state();
		glBindTexture(GL_TEXTURE_2D, from->tex->gl);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, FX_POS);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, FX_UV);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		from = &FXB[l].rt;
	}
	for (int i = 0; i < 2; i++) {
		int l = lv[i] - 1;
		// the surface is portrait: its u runs along the display's height, its v along the width
		fx_blur_pass(&FXT[l].rt, FXB[l].tex.gl, 0.0f, str[i] / 640.0f, 1.0f);
		fx_blur_pass(m, FXT[l].tex.gl, str[i] / 480.0f, 0.0f, i ? 0.5f : 1.0f);
	}
	glViewport(0, 0, m->gl_w, m->gl_h);
	G.unif_gen++;
}

// GPU cost tests (SELECT + R while playing, vita_log.txt says which): 1 the fragment lighting off,
// 2 the fog's draws left out, 3 the shadows' left out; the log's frames lines then tell which pass
// the slow scenes wait on (s_gpu_test is declared with the fragment programs)

void c3d_gpu_test_next(void)
{
	static const char* const NAME[4] = { "normal", "no fragment lighting", "no fog", "no shadows" };
	s_gpu_test = (s_gpu_test + 1) % 4;
	vita_log("GPU test %d: %s", s_gpu_test, NAME[s_gpu_test]);
}

static bool test_skips(void)
{
	const char* name = c3d_vertex_programs[G.program].name;
	return (s_gpu_test == 2 && !strcmp(name, "fog")) || (s_gpu_test == 3 && !strcmp(name, "shadow"));
}

static bool draw_setup(u32 nverts)
{
	if (G.program < 0) {
		s_draws[1]++;
		return false;
	}
	if (s_gpu_test && test_skips())
		return false;
	Program* p = current_program();
	if (!p->prog) {
		s_draws[2]++;
		return false;
	}
	s_draws[0]++;
	glUseProgram(p->prog);
	for (int i = 0; i < 3; i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, c3d_tex_gl(G.tex[i]));
	}
	upload_luts();
	upload_uniforms(p);
	apply_fixed();
	if (HF.active)                   // coverage in the alpha, for half_fog_end
		HF.draws++, glBlendFuncSeparate(BF[F.bsrc_c % 15], BF[F.bdst_c % 15], GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	apply_attribs(c3d_vertex_programs[G.program].ninputs, nverts);
	return true;
}

static GLenum prim(GPU_Primitive_t p)
{
	switch (p) {
	case GPU_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
	case GPU_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
	default: return GL_TRIANGLES;
	}
}

void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size)
{
	if (G.program < 0 || size <= 0 || !G.target)
		return;
	half_fog_route();
	if (!draw_setup((u32)(first + size)))
		return;
	u64 t0 = sceKernelGetProcessTimeWide();
	glDrawArrays(prim(primitive), first, size);
	c3d_stats.draw_n++;
	c3d_stats.draw_us += sceKernelGetProcessTimeWide() - t0;
}

void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices)
{
	if (G.program < 0 || count <= 0 || !G.target)
		return;
	half_fog_route();
	u32 top = 0;
	if (type == C3D_UNSIGNED_SHORT)
		for (int i = 0; i < count; i++)
			top = ((const u16*)indices)[i] > top ? ((const u16*)indices)[i] : top;
	else
		for (int i = 0; i < count; i++)
			top = ((const u8*)indices)[i] > top ? ((const u8*)indices)[i] : top;
	if (!draw_setup(top + 1))
		return;
	// The indices from a buffer object too: the maps' and characters' are written once, into
	// linearAlloc blocks of their own, and were copied into vitaGL's pool on every draw (a suspect
	// for the forest's slow frames, where the draws waited with the CPU and GPU near idle)
	const u8* blk;
	u32 blk_size;
	bool ibo = c3d_linear_block(indices, &blk, &blk_size) && blk_size >= 4096;
	if (ibo)
		vbo_bind_to(GL_ELEMENT_ARRAY_BUFFER, blk, blk_size);
	u64 t0 = sceKernelGetProcessTimeWide();
	glDrawElements(prim(primitive), count, type == C3D_UNSIGNED_SHORT ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
	               ibo ? (const void*)(uintptr_t)((const u8*)indices - blk) : indices);
	if (ibo)
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	c3d_stats.draw_n++;
	c3d_stats.draw_us += sceKernelGetProcessTimeWide() - t0;
}

// ---------------------------------------------------------------- presenting
// The Vita's screen is 960 x 544. Until the single-screen interface is done (port/vita/README.md),
// the top screen shows at 640 x 384 on the left and the touch screen at 320 x 240 on the right,
// where the front panel touches it; VITA_LAYOUT_SINGLE shows only the top screen, at 5:3.

int vita_layout = VITA_LAYOUT_SINGLE;

typedef struct { int x, y, w, h; } Rect;

static void layout(Rect* top, Rect* bottom)
{
	if (vita_layout == VITA_LAYOUT_SINGLE) {
		// the game screen fills the display (C3D_AspectRatioTop); the touch page is laid over its
		// right side at 1.5x, and only while it has something to choose (touchui_overlay)
		*top = (Rect){ 0, 0, 960, 544 };
		*bottom = touchui_overlay() ? (Rect){ 960 - 480 - 40, 92, 480, 360 } : (Rect){ 0, 0, 0, 0 };
	} else {
		*top = (Rect){ 0, 80, 640, 384 };
		*bottom = (Rect){ 640, 152, 320, 240 };
	}
}

bool vita_touch_to_bottom(int x, int y, u16* px, u16* py)
{
	Rect t, b;
	layout(&t, &b);
	if (b.w <= 0 || x < b.x || y < b.y || x >= b.x + b.w || y >= b.y + b.h)
		return false;
	*px = (u16)((x - b.x) * 320 / b.w);
	*py = (u16)((y - b.y) * 240 / b.h);
	return true;
}

static const char PRESENT_VS[] =
	"void main(float2 pos, float2 uv, out float4 gl_Position : POSITION, out float2 o_uv : TEXCOORD0)\n"
	"{ gl_Position = float4(pos, 0.0, 1.0); o_uv = uv; }\n";
static const char PRESENT_FS[] =
	"float4 main(float2 o_uv : TEXCOORD0, uniform sampler2D tex) : COLOR { return tex2D(tex, o_uv); }\n";

static const char FX_FS[] =
	"float4 main(float2 o_uv : TEXCOORD0, uniform sampler2D tex, uniform float alpha) : COLOR\n"
	"{ return float4(tex2D(tex, o_uv).rgb, alpha); }\n";

static const char FX4_FS[] =
	"float4 main(float2 o_uv : TEXCOORD0, uniform sampler2D tex, uniform float4 off, uniform float alpha) : COLOR\n"
	"{ float3 c = tex2D(tex, o_uv - off.xy).rgb + tex2D(tex, o_uv - off.zw).rgb + tex2D(tex, o_uv + off.zw).rgb +\n"
	"tex2D(tex, o_uv + off.xy).rgb; return float4(c * 0.25, alpha); }\n";

static void present_init(void)
{
	s_present_prog = glCreateProgram();
	if (!link(s_present_prog, compile(GL_VERTEX_SHADER, PRESENT_VS), compile(GL_FRAGMENT_SHADER, PRESENT_FS))) {
		vita_log("present program not built");
		glDeleteProgram(s_present_prog);
		s_present_prog = 0;
		return;
	}
	glBindAttribLocation(s_present_prog, 0, "pos");
	glBindAttribLocation(s_present_prog, 1, "uv");
	glLinkProgram(s_present_prog);
	s_present_tex = glGetUniformLocation(s_present_prog, "tex");
	s_fx_prog = glCreateProgram();
	if (!link(s_fx_prog, compile(GL_VERTEX_SHADER, PRESENT_VS), compile(GL_FRAGMENT_SHADER, FX_FS))) {
		vita_log("filter program not built");
		glDeleteProgram(s_fx_prog);
		s_fx_prog = 0;
		return;
	}
	glBindAttribLocation(s_fx_prog, 0, "pos");
	glBindAttribLocation(s_fx_prog, 1, "uv");
	glLinkProgram(s_fx_prog);
	s_fx_tex = glGetUniformLocation(s_fx_prog, "tex");
	s_fx_alpha = glGetUniformLocation(s_fx_prog, "alpha");
	s_fx4_prog = glCreateProgram();
	if (!link(s_fx4_prog, compile(GL_VERTEX_SHADER, PRESENT_VS), compile(GL_FRAGMENT_SHADER, FX4_FS))) {
		vita_log("blur program not built");
		glDeleteProgram(s_fx4_prog);
		s_fx4_prog = 0;
		return;
	}
	glBindAttribLocation(s_fx4_prog, 0, "pos");
	glBindAttribLocation(s_fx4_prog, 1, "uv");
	glLinkProgram(s_fx4_prog);
	s_fx4_tex = glGetUniformLocation(s_fx4_prog, "tex");
	s_fx4_off = glGetUniformLocation(s_fx4_prog, "off");
	s_fx4_alpha = glGetUniformLocation(s_fx4_prog, "alpha");
}

// One screen's texture into r. portrait: a PICA render target (u along the screen's height from
// the bottom, v along its width from the right); otherwise a 3DS framebuffer (v from the left).
static void blit(GLuint tex, Rect r, bool portrait)
{
	if (r.w <= 0 || !tex || !s_present_prog)
		return;
	float x0 = r.x / 480.0f - 1.0f, x1 = (r.x + r.w) / 480.0f - 1.0f;
	float y0 = 1.0f - (r.y + r.h) / 272.0f, y1 = 1.0f - r.y / 272.0f;
	// corners: bottom left, bottom right, top left, top right
	float pos[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
	float uv_rt[8] = { 0, 1, 0, 0, 1, 1, 1, 0 };
	float uv_fb[8] = { 0, 0, 0, 1, 1, 0, 1, 1 };
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, portrait ? uv_rt : uv_fb);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void present_begin(void)
{
	bind_target(NULL);
	glViewport(0, 0, 960, 544);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(s_present_prog);
	glUniform1i(s_present_tex, 0);
}

static void present(void)
{
	Rect rt, rb;
	layout(&rt, &rb);
	present_begin();
	// the game screen opaque, then the touch page blended over it (its ground is translucent)
	for (int pass = 0; pass < 2; pass++) {
		if (pass)
			glEnable(GL_BLEND), glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		for (int i = 0; i < MAX_TARGETS; i++) {
			C3D_RenderTarget* t = G.targets[i];
			if (!t || t->screen != (pass ? GFX_BOTTOM : GFX_TOP))
				continue;
			blit(t->tex->gl, pass ? rb : rt, true);
		}
	}
	glDisable(GL_BLEND);
	G.unif_gen++;
}

// nodata.c's screens: the 3DS framebuffers, BGR8, 240 pixels per column
void vita_present_framebuffers(const u8* top, const u8* bottom)
{
	static GLuint tex[2];
	static u8 rgb[240 * 400 * 3];
	if (!tex[0])
		glGenTextures(2, tex);
	const u8* fb[2] = { top, bottom };
	int cols[2] = { 400, 320 };
	for (int s = 0; s < 2; s++) {
		int n = 240 * cols[s];
		for (int i = 0; i < n; i++) {
			rgb[i * 3] = fb[s][i * 3 + 2];
			rgb[i * 3 + 1] = fb[s][i * 3 + 1];
			rgb[i * 3 + 2] = fb[s][i * 3];
		}
		glBindTexture(GL_TEXTURE_2D, tex[s]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 240, cols[s], 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	Rect rt, rb;
	int was = vita_layout;
	vita_layout = VITA_LAYOUT_DUAL;          // this screen's text runs over both screens
	layout(&rt, &rb);
	vita_layout = was;
	present_begin();
	blit(tex[0], rt, false);
	blit(tex[1], rb, false);
	vglSwapBuffers(GL_FALSE);
}

// gpusync.h waits for the GPU to finish the last frame before freeing what it drew with
void C3Di_RenderQueueWaitDone(void) { glFinish(); }
