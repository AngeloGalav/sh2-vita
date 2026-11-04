// citro3d textures and Tex3DS for the Vita build. A C3D_Tex keeps its pixels in the PICA's layout
// in tex->data, as on the 3DS, because the game writes and reads them there (movie frames, the
// film grain, font widths, room.c's half-size copies). Before a draw samples it, a texture whose
// data changed is untiled into GL row order and uploaded: ETC1 stays compressed (vitaGL takes it
// as GL_ETC1_RGB8_OES), ETC1A4 and the 4-bit formats are expanded, the rest keep their size.
//
// PICA layout, as tex3ds writes it: 8 x 8 tiles in row order, pixels Morton-ordered inside a tile;
// the rows go to GL in the same order, top row first, and the fragment programs sample the game's
// textures at 1 - t (c3d_gl.c), since the PICA reads its row 0 at t = 1. ETC1
// tiles hold four 4 x 4 blocks in row order, each a little-endian 64-bit ETC1 word, ETC1A4 with
// 8 bytes of 4-bit alpha (column-major) before each block. A pack converted by
// tools/sh2conv/vitatex.py has its textures already in GL's form, format 0x80 | the PICA's
// (GPU_VITA_FORM): those go up as they are, ETC1A4's as DXT5 with its levels. Vita-only.
#include <citro3d.h>
#include <tex3ds.h>
#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <stdlib.h>
#include <string.h>
#include "vita.h"
#include "c3d_internal.h"

static const u8 BPP[16] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 0, 0 };

static size_t level_size(const C3D_Tex* t, int level)
{
	size_t w = t->width >> level, h = t->height >> level;
	return w * h * BPP[t->fmt & 15] / 8;
}

static size_t total_size(const C3D_Tex* t)
{
	size_t s = 0;
	for (int i = 0; i <= t->maxLevel; i++)
		s += level_size(t, i);
	return s;
}

// ---------------------------------------------------------------- texture registry
// GSPGPU_FlushDataCache on a texture's data marks it changed, as movie.c and others write a
// texture and flush it rather than calling C3D_TexFlush. The registry is keyed by the data, not the
// C3D_Tex: the game copies the struct (room.c imports a refined texture into a local and assigns
// it), and a copy keeps its data and its entry. Each flush over an entry counts a write; a texture
// is sent again when the count moved since it was last sent (c3d_tex_gl).

#define MAX_TEX 4096
static struct { const u8* data; u32 size; u32 writes; } s_reg[MAX_TEX];
static int s_free[MAX_TEX], s_nfree, s_top;

static void reg_add(C3D_Tex* t, u32 size)
{
	int i = s_nfree ? s_free[--s_nfree] : s_top < MAX_TEX ? s_top++ : -1;
	t->reg = 0;
	if (i < 0)
		return;                          // full: the texture is still sent when dirty, by its flag
	s_reg[i].data = t->data, s_reg[i].size = size, s_reg[i].writes = 0;
	t->reg = i + 1, t->seen = 0;
}

static bool reg_ok(const C3D_Tex* t) { return t->reg > 0 && s_reg[t->reg - 1].data == t->data; }

static void reg_del(C3D_Tex* t)
{
	if (reg_ok(t)) {
		s_reg[t->reg - 1].data = NULL;
		s_free[s_nfree++] = t->reg - 1;
	}
	t->reg = 0;
}

void c3d_tex_mark_range(const void* adr, u32 size)
{
	c3d_buf_mark_range(adr, size);
	const u8* a = adr;
	for (int i = 0; i < s_top; i++)
		if (s_reg[i].data && a < s_reg[i].data + s_reg[i].size && a + size > s_reg[i].data)
			s_reg[i].writes++;
}

// ---------------------------------------------------------------- formats

static inline u32 morton(u32 x, u32 y)
{
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static inline u32 pica_index(u32 x, u32 y, u32 w)
{
	return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7);
}

static const int ETC_MOD[8][4] = {
	{ 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
	{ 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 }, { 47, 183, -47, -183 },
};

static inline u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }

// One ETC1 block (the 64-bit word as read little-endian from the PICA) into 4 x 4 RGBA, row y
// first; alpha left for the caller
static void etc1_block(u64 w, u8 out[4][4][4])
{
	u32 hi = (u32)(w >> 32), lo = (u32)w;
	bool diff = hi & 2, flip = hi & 1;
	int base[2][3];
	if (diff) {
		for (int c = 0; c < 3; c++) {
			int b = (hi >> (27 - c * 8)) & 31;
			int d = (hi >> (24 - c * 8)) & 7;
			d = d >= 4 ? d - 8 : d;
			base[0][c] = (b << 3) | (b >> 2);
			int b2 = (b + d) & 31;
			base[1][c] = (b2 << 3) | (b2 >> 2);
		}
	} else {
		for (int c = 0; c < 3; c++) {
			int b1 = (hi >> (28 - c * 8)) & 15, b2 = (hi >> (24 - c * 8)) & 15;
			base[0][c] = b1 * 17;
			base[1][c] = b2 * 17;
		}
	}
	int table[2] = { (hi >> 5) & 7, (hi >> 2) & 7 };
	for (int x = 0; x < 4; x++)
		for (int y = 0; y < 4; y++) {
			int i = x * 4 + y;
			int sub = flip ? y >= 2 : x >= 2;
			int idx = (((lo >> (i + 16)) & 1) << 1) | ((lo >> i) & 1);
			int m = ETC_MOD[table[sub]][idx];
			out[y][x][0] = clamp8(base[sub][0] + m);
			out[y][x][1] = clamp8(base[sub][1] + m);
			out[y][x][2] = clamp8(base[sub][2] + m);
			out[y][x][3] = 255;
		}
}

// One ETC1A4 block straight into RGBA4444, row stride w texels: the block's eight colours (two
// halves, four modifiers) worked out once, then each texel a lookup, where etc1_block works out
// every texel's (a 512 x 512 texture took 35 ms to decode, a hitch each time one came in)
static void etc1a4_block(u64 word, u64 alpha, u16* o, u32 w)
{
	u32 hi = (u32)(word >> 32), lo = (u32)word;
	bool diff = hi & 2, flip = hi & 1;
	int base[2][3];
	for (int c = 0; c < 3; c++) {
		if (diff) {
			int b = (hi >> (27 - c * 8)) & 31, d = (hi >> (24 - c * 8)) & 7;
			d = d >= 4 ? d - 8 : d;
			int b2 = (b + d) & 31;
			base[0][c] = (b << 3) | (b >> 2);
			base[1][c] = (b2 << 3) | (b2 >> 2);
		} else {
			base[0][c] = ((hi >> (28 - c * 8)) & 15) * 17;
			base[1][c] = ((hi >> (24 - c * 8)) & 15) * 17;
		}
	}
	int table[2] = { (hi >> 5) & 7, (hi >> 2) & 7 };
	u16 pal[2][4];
	for (int sub = 0; sub < 2; sub++)
		for (int i = 0; i < 4; i++) {
			int m = ETC_MOD[table[sub]][i];
			pal[sub][i] = (u16)((clamp8(base[sub][0] + m) >> 4) << 12 | (clamp8(base[sub][1] + m) >> 4) << 8 |
			                    (clamp8(base[sub][2] + m) >> 4) << 4);
		}
	for (u32 x = 0; x < 4; x++)
		for (u32 y = 0; y < 4; y++) {
			u32 i = x * 4 + y;
			int sub = flip ? y >= 2 : x >= 2;
			int idx = (((lo >> (i + 16)) & 1) << 1) | ((lo >> i) & 1);
			o[y * w + x] = pal[sub][idx] | (u16)((alpha >> (i * 4)) & 15);
		}
}

static inline u64 rd64(const u8* p)
{
	u64 v;
	memcpy(&v, p, 8);
	return v;
}

// A whole level into GL upload form. Returns the GL format (internal = format) and type, and
// whether it is compressed; out holds w * h pixels in GL row order.
typedef struct { GLenum format, type; bool compressed; size_t bytes; } GlForm;

static GlForm gl_form(GPU_TEXCOLOR f, u32 w, u32 h)
{
	switch (f) {
	case GPU_RGB565: return (GlForm){ GL_RGB, GL_UNSIGNED_SHORT_5_6_5, false, w * h * 2 };
	case GPU_RGBA5551: return (GlForm){ GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, false, w * h * 2 };
	case GPU_RGBA4: return (GlForm){ GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, false, w * h * 2 };
	case GPU_L8: case GPU_L4: return (GlForm){ GL_LUMINANCE, GL_UNSIGNED_BYTE, false, w * h };
	case GPU_A8: case GPU_A4: return (GlForm){ GL_ALPHA, GL_UNSIGNED_BYTE, false, w * h };
	case GPU_LA8: case GPU_LA4: case GPU_HILO8: return (GlForm){ GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, false, w * h * 2 };
	case GPU_RGB8: return (GlForm){ GL_RGB, GL_UNSIGNED_BYTE, false, w * h * 3 };
	case GPU_ETC1: return (GlForm){ GL_ETC1_RGB8_OES, 0, true, w * h / 2 };
	// ETC1A4 decoded to 16 bits a texel: its alpha has 4 bits anyway, and 32-bit texels took 1 MB
	// for every 512 x 512 one (the town's trees and decals), more than vitaGL had to give
	case GPU_ETC1A4: return (GlForm){ GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, false, w * h * 2 };
	default: return (GlForm){ GL_RGBA, GL_UNSIGNED_BYTE, false, w * h * 4 };
	}
}

// The level in GL upload order (rows as the PICA keeps them, see the top).
static void untile(GPU_TEXCOLOR f, const u8* src, u8* dst, u32 w, u32 h)
{
	if (f == GPU_ETC1 || f == GPU_ETC1A4) {
		bool alpha = f == GPU_ETC1A4;
		u32 bw = w / 4, bh = h / 4, blk = alpha ? 16 : 8;
		for (u32 by = 0; by < bh; by++)
			for (u32 bx = 0; bx < bw; bx++) {
				u32 tile = (by / 2) * (w / 8) + bx / 2;
				u32 sub = (bx & 1) + 2 * (by & 1);
				const u8* b = src + (tile * 4 + sub) * blk;
				if (!alpha) {
					// GL wants the word big-endian
					u8* o = dst + (by * bw + bx) * 8;
					for (int i = 0; i < 8; i++)
						o[i] = b[7 - i];
					continue;
				}
				etc1a4_block(rd64(b + 8), rd64(b), (u16*)dst + by * 4 * w + bx * 4, w);
			}
		return;
	}
	// Tile by tile, each tile's 64 texels in the order they are stored (Morton), each written where
	// its row and column put it: the format's loop runs with no index sums or switch per texel
	static u8 mx[64], my[64];
	if (!mx[63])
		for (u32 y = 0; y < 8; y++)
			for (u32 x = 0; x < 8; x++)
				mx[morton(x, y)] = (u8)x, my[morton(x, y)] = (u8)y;
#define TEXELS(body)                                                                             \
	for (u32 ty = 0, i = 0; ty < h / 8; ty++)                                                    \
		for (u32 tx = 0; tx < w / 8; tx++)                                                       \
			for (u32 k = 0; k < 64; k++, i++) {                                                  \
				u32 o = (ty * 8 + my[k]) * w + tx * 8 + mx[k];                                   \
				body                                                                             \
			}
	switch (f) {
	case GPU_RGBA8:
		TEXELS(const u8* p = src + i * 4; u8* q = dst + o * 4;    /* A B G R */
		       q[0] = p[3]; q[1] = p[2]; q[2] = p[1]; q[3] = p[0];)
		break;
	case GPU_RGB8:
		TEXELS(const u8* p = src + i * 3; u8* q = dst + o * 3;    /* B G R */
		       q[0] = p[2]; q[1] = p[1]; q[2] = p[0];)
		break;
	case GPU_RGB565: case GPU_RGBA5551: case GPU_RGBA4:           // same bit order as GL's packed types
		TEXELS(((u16*)dst)[o] = ((const u16*)src)[i];)
		break;
	case GPU_LA8: case GPU_HILO8:                                  // A L
		TEXELS(dst[o * 2] = src[i * 2 + 1]; dst[o * 2 + 1] = src[i * 2];)
		break;
	case GPU_L8: case GPU_A8:
		TEXELS(dst[o] = src[i];)
		break;
	case GPU_LA4:
		TEXELS(u8 v = src[i]; dst[o * 2] = (v >> 4) * 17; dst[o * 2 + 1] = (v & 15) * 17;)
		break;
	case GPU_L4: case GPU_A4:
		TEXELS(u8 v = src[i >> 1]; dst[o] = ((i & 1) ? v >> 4 : v & 15) * 17;)
		break;
	default:
		break;
	}
#undef TEXELS
}

static GLint gl_wrap(u32 v)
{
	switch (v) {
	case GPU_REPEAT: return GL_REPEAT;
	case GPU_MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
	default: return GL_CLAMP_TO_EDGE;            // CLAMP_TO_BORDER: the borders the game sets are black
	}
}

static void apply_params(C3D_Tex* t)
{
	u32 p = t->param;
	bool mag = (p >> 1) & 1, min = (p >> 2) & 1, mip = (p >> 24) & 1;
	GLint minf = min ? GL_LINEAR : GL_NEAREST;
	if (t->maxLevel > 0 && (t->fmt & 15) != GPU_ETC1)   // upload(): mipmaps for all but ETC1
		minf = min ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
		           : (mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap((p >> 12) & 3));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap((p >> 8) & 3));
}

static void upload_timed(C3D_Tex* t);

static void upload(C3D_Tex* t)
{
	u64 t0 = sceKernelGetProcessTimeWide();
	upload_timed(t);
	c3d_stats.tex_n++;
	c3d_stats.tex_kb += (u32)(t->size >> 10);
	c3d_stats.tex_us += sceKernelGetProcessTimeWide() - t0;
}

// A texture already in the Vita's form (vitatex.py): no untiling, DXT5 with the pack's own levels
static void upload_vita_form(C3D_Tex* t)
{
	GPU_TEXCOLOR base = (GPU_TEXCOLOR)(t->fmt & 15);
	const u8* d = t->data;
	if (base == GPU_ETC1A4) {
		for (int k = 0; k <= t->maxLevel; k++) {
			u32 w = t->width >> k, h = t->height >> k;
			glCompressedTexImage2D(GL_TEXTURE_2D, k, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, w, h, 0, w * h, d);
			d += w * h;
		}
		return;
	}
	GlForm g = gl_form(base, t->width, t->height);
	if (g.compressed)
		glCompressedTexImage2D(GL_TEXTURE_2D, 0, g.format, t->width, t->height, 0, g.bytes, d);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, g.format, t->width, t->height, 0, g.format, g.type, d);
	if (t->maxLevel > 0 && !g.compressed)
		glGenerateMipmap(GL_TEXTURE_2D);
}

static void upload_timed(C3D_Tex* t)
{
	if (!t->gl)
		glGenTextures(1, &t->gl);
	glBindTexture(GL_TEXTURE_2D, t->gl);
	if (t->data && (t->fmt & GPU_VITA_FORM)) {
		upload_vita_form(t);
		GLenum e = glGetError();
		static unsigned failures;
		if (e != GL_NO_ERROR && failures++ < 50)
			vita_log("texture %ux%u format 0x%x not uploaded: GL error 0x%x, vitaGL %u KB free", t->width,
			         t->height, (unsigned)t->fmt, e, (unsigned)(vglMemFree(VGL_MEM_ALL) >> 10));
	} else if (t->data) {
		GlForm g = gl_form(t->fmt, t->width, t->height);
		u8* tmp = malloc(g.bytes);
		// Level 0, and the smaller levels from glGenerateMipmap where the texture has some and is
		// not ETC1 (vitaGL's compressed levels are untried). They were all left out for a while, as
		// the suspect for coloured blocks on far textures, which stayed without them and went with
		// the registry and heap fixes; a texture sampled far smaller than its size without them
		// reads scattered texels (the fog's 256 x 256 cloud on every sprite), slow for the GPU.
		if (tmp) {
			u64 t0 = sceKernelGetProcessTimeWide();
			untile(t->fmt, t->data, tmp, t->width, t->height);
			c3d_stats.untile_us += sceKernelGetProcessTimeWide() - t0;
			if (g.compressed)
				glCompressedTexImage2D(GL_TEXTURE_2D, 0, g.format, t->width, t->height, 0, g.bytes, tmp);
			else
				glTexImage2D(GL_TEXTURE_2D, 0, g.format, t->width, t->height, 0, g.format, g.type, tmp);
			if (t->maxLevel > 0 && !g.compressed)
				glGenerateMipmap(GL_TEXTURE_2D);
			GLenum e = glGetError();
			static unsigned failures;
			if (e != GL_NO_ERROR && failures++ < 50)
				vita_log("texture %ux%u format %d not uploaded: GL error 0x%x, vitaGL %u KB free", t->width,
				         t->height, (int)t->fmt, e, (unsigned)(vglMemFree(VGL_MEM_ALL) >> 10));
		} else {
			vita_log("texture %ux%u: no memory to untile it", t->width, t->height);
		}
		free(tmp);
	}
	apply_params(t);
	t->dirty = false;
}

// ---------------------------------------------------------------- C3D_Tex

bool C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p)
{
	(void)cube;
	if ((p.width & 7) || (p.height & 7) || p.type != GPU_TEX_2D)
		return false;
	memset(tex, 0, sizeof *tex);
	tex->width = p.width;
	tex->height = p.height;
	tex->fmt = p.format;
	tex->maxLevel = p.maxLevel;
	tex->size = level_size(tex, 0);
	tex->param = GPU_TEXTURE_MODE(GPU_TEX_2D) | GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST)
	           | GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
	tex->on_vram = p.onVram;
	tex->data = linearAlloc(total_size(tex));
	if (!tex->data)
		return false;
	tex->dirty = true;
	reg_add(tex, (u32)total_size(tex));
	return true;
}

void* C3D_TexGetImagePtr(C3D_Tex* tex, void* data, int level, u32* size)
{
	u8* p = data;
	if (level < 0) {
		if (size)
			*size = (u32)total_size(tex);
		return data;
	}
	for (int i = 0; i < level; i++)
		p += level_size(tex, i);
	if (size)
		*size = (u32)level_size(tex, level);
	return p;
}

void C3D_TexLoadImage(C3D_Tex* tex, const void* data, GPU_TEXFACE face, int level)
{
	(void)face;
	u32 size;
	void* dst = C3D_TexGetImagePtr(tex, tex->data, level, &size);
	memcpy(dst, data, size);
	tex->dirty = true;
}

void C3D_TexFlush(C3D_Tex* tex) { tex->dirty = true; }

void C3D_TexDelete(C3D_Tex* tex)
{
	reg_del(tex);
	c3d_forget_tex(tex);
	if (tex->gl)
		glDeleteTextures(1, &tex->gl);
	tex->gl = 0;
	if (tex->data)
		linearFree(tex->data);
	tex->data = NULL;
}

void C3D_TexSetFilter(C3D_Tex* tex, GPU_TEXTURE_FILTER_PARAM mag, GPU_TEXTURE_FILTER_PARAM min)
{
	tex->param = (tex->param & ~(GPU_TEXTURE_MAG_FILTER(1) | GPU_TEXTURE_MIN_FILTER(1)))
	           | GPU_TEXTURE_MAG_FILTER(mag) | GPU_TEXTURE_MIN_FILTER(min);
	tex->params_dirty = true;
}

void C3D_TexSetFilterMipmap(C3D_Tex* tex, GPU_TEXTURE_FILTER_PARAM filter)
{
	tex->param = (tex->param & ~GPU_TEXTURE_MIP_FILTER(1)) | GPU_TEXTURE_MIP_FILTER(filter);
	tex->params_dirty = true;
}

void C3D_TexSetWrap(C3D_Tex* tex, GPU_TEXTURE_WRAP_PARAM s, GPU_TEXTURE_WRAP_PARAM t)
{
	tex->param = (tex->param & ~(GPU_TEXTURE_WRAP_S(3) | GPU_TEXTURE_WRAP_T(3)))
	           | GPU_TEXTURE_WRAP_S(s) | GPU_TEXTURE_WRAP_T(t);
	tex->params_dirty = true;
}

// Before a draw: the GL texture for tex, uploaded or re-parameterised if the game changed it
u32 c3d_tex_gl(C3D_Tex* tex)
{
	if (!tex)
		return 0;
	if (tex->render_target) {             // drawn into by the GPU: its GL texture is the truth
		if (tex->params_dirty) {
			glBindTexture(GL_TEXTURE_2D, tex->gl);
			apply_params(tex);
			tex->params_dirty = false;
		}
		return tex->gl;
	}
	bool written = reg_ok(tex) && s_reg[tex->reg - 1].writes != tex->seen;
	if (tex->dirty || !tex->gl || written) {
		upload(tex);
		if (reg_ok(tex))
			tex->seen = s_reg[tex->reg - 1].writes;
		tex->params_dirty = false;
	} else if (tex->params_dirty) {
		glBindTexture(GL_TEXTURE_2D, tex->gl);
		apply_params(tex);
		tex->params_dirty = false;
	}
	return tex->gl;
}

// A render target's colour texture: GL storage only, sampled as the PICA would sample it
void c3d_tex_make_target(C3D_Tex* tex, u32 w, u32 h)
{
	if (!tex->gl)
		glGenTextures(1, &tex->gl);
	glBindTexture(GL_TEXTURE_2D, tex->gl);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	apply_params(tex);
	tex->render_target = true;
	tex->dirty = false;
}

// ---------------------------------------------------------------- Tex3DS

struct Tex3DS_Texture_s {
	u16 count;
	Tex3DS_SubTexture sub[];
};

Tex3DS_Texture Tex3DS_TextureImport(const void* input, size_t insize, C3D_Tex* tex, C3D_TexCube* cube, bool vram)
{
	(void)cube;
	const u8* p = input;
	if (insize < 6)
		return NULL;
	u16 count = p[0] | p[1] << 8;
	u8 dims = p[2], fmt = p[3], mips = p[4];
	if (dims & 0x40)                       // cube maps: the pack has none
		return NULL;
	size_t hdr = 5 + count * 12;
	if (insize < hdr + 4)
		return NULL;
	Tex3DS_Texture t = malloc(sizeof *t + count * sizeof(Tex3DS_SubTexture));
	if (!t)
		return NULL;
	t->count = count;
	for (u16 i = 0; i < count; i++) {
		const u8* s = p + 5 + i * 12;
		u16 v[6];
		for (int k = 0; k < 6; k++)
			v[k] = s[k * 2] | s[k * 2 + 1] << 8;
		t->sub[i] = (Tex3DS_SubTexture){ v[0], v[1], v[2], v[3], v[4], v[5] };
	}
	C3D_TexInitParams params = { (u16)(1 << ((dims & 7) + 3)), (u16)(1 << (((dims >> 3) & 7) + 3)),
	                             mips, (GPU_TEXCOLOR)fmt, GPU_TEX_2D, vram };
	if (!C3D_TexInitWithParams(tex, NULL, params)) {
		free(t);
		return NULL;
	}
	// the body: libctru's compression header (type byte, 24-bit size); the packer writes type 0
	const u8* body = p + hdr;
	u32 csize = body[1] | body[2] << 8 | body[3] << 16;
	size_t want = total_size(tex);
	if (body[0] != 0x00) {
		vita_log("tex3ds: compressed body (type %02x) not supported", body[0]);
		C3D_TexDelete(tex);
		free(t);
		return NULL;
	}
	size_t have = insize - hdr - 4;
	if (csize < want)
		want = csize;
	memcpy(tex->data, body + 4, want < have ? want : have);
	tex->dirty = true;
	return t;
}

Tex3DS_Texture Tex3DS_TextureImportStdio(FILE* fp, C3D_Tex* tex, C3D_TexCube* cube, bool vram)
{
	long start = ftell(fp);
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp) - start;
	fseek(fp, start, SEEK_SET);
	if (n <= 0)
		return NULL;
	u8* buf = malloc(n);
	if (!buf)
		return NULL;
	Tex3DS_Texture t = NULL;
	if (fread(buf, 1, n, fp) == (size_t)n)
		t = Tex3DS_TextureImport(buf, n, tex, cube, vram);
	free(buf);
	return t;
}

Tex3DS_Texture Tex3DS_TextureImportFD(int fd, C3D_Tex* tex, C3D_TexCube* cube, bool vram)
{
	(void)fd; (void)tex; (void)cube; (void)vram;
	return NULL;
}

size_t Tex3DS_GetNumSubTextures(const Tex3DS_Texture t) { return t ? t->count : 0; }
const Tex3DS_SubTexture* Tex3DS_GetSubTexture(const Tex3DS_Texture t, size_t i)
{
	return t && i < t->count ? &t->sub[i] : NULL;
}
void Tex3DS_TextureFree(Tex3DS_Texture t) { free(t); }
