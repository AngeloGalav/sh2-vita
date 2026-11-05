// What compat/c3d_*.c share. Vita-only.
#pragma once
#include <citro3d.h>

u32 c3d_tex_gl(C3D_Tex* tex);                 // the texture's GL name, uploaded if it changed
void c3d_tex_make_target(C3D_Tex* tex, u32 w, u32 h);
void c3d_forget_tex(C3D_Tex* tex);            // a deleted texture is unbound
void c3d_buf_mark_range(const void* adr, u32 size);   // vertex data rewritten (GSPGPU_FlushDataCache)
void c3d_buf_forget(const void* adr);         // vertex data freed (linearFree)
// The linearAlloc block holding p: its start and size (ctru.c)
bool c3d_linear_block(const void* p, const u8** base, u32* size);

// Where a frame's time goes in vitaGL, for the log's once-a-second line (c3d_gl.c C3D_FrameEnd):
// counts, KB and microseconds of texture uploads, buffer uploads and draw calls
typedef struct { u32 tex_n, tex_kb, buf_n, buf_kb, draw_n, flush_n, flush_kb, dirtied, buf_new; u64 tex_us, untile_us, buf_us, draw_us, swap_us; } C3dStats;
extern C3dStats c3d_stats;
void c3d_gpu_test_next(void);
void c3d_etc1_mips_toggle(void);         // SELECT + square (ctru.c): ETC1's smaller levels or level 0 alone
void c3d_fog_precision_toggle(void);     // SELECT + triangle (ctru.c): the half-size fog's colour in float or half
void c3d_half_fog_toggle(void);           // SELECT + L (ctru.c): the fog at half size on or off            // SELECT + R (ctru.c): the next GPU cost test

// c3d_shaders.c: the game's vertex programs in Cg
typedef struct {
	const char* name;                          // the .v.pica's name (world, skin, ...)
	const char* uniform_names[8];
	u8 uniform_regs[8];
	int nregs;                                 // registers c0.. it reads
	int ninputs;                               // v0 ..
	bool outputs_light;                        // writes the view vector and normal quaternion
	int ntexcoords;                            // texcoord0 .. n-1
	const char* source;
} VertexProgram;
extern const VertexProgram c3d_vertex_programs[];
extern const int c3d_vertex_program_count;
