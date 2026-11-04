// Tex3DS: the .t3x files in the pack (tex3ds' header, then the PICA-tiled pixels of every mip
// level). compat/tex3ds.c reads them into a C3D_Tex the way libctru's tex3ds.c does and uploads
// them through compat/c3d_tex.c, which untiles them and decodes ETC1 / ETC1A4.
#pragma once
#include <citro3d.h>

typedef struct Tex3DS_Texture_s* Tex3DS_Texture;
typedef struct {
	u16 width, height;
	u16 left, top, right, bottom;   // texture coordinates * 1024
} Tex3DS_SubTexture;

Tex3DS_Texture Tex3DS_TextureImport(const void* input, size_t insize, C3D_Tex* tex, C3D_TexCube* texcube, bool vram);
Tex3DS_Texture Tex3DS_TextureImportStdio(FILE* fp, C3D_Tex* tex, C3D_TexCube* texcube, bool vram);
Tex3DS_Texture Tex3DS_TextureImportFD(int fd, C3D_Tex* tex, C3D_TexCube* texcube, bool vram);
size_t Tex3DS_GetNumSubTextures(const Tex3DS_Texture texture);
const Tex3DS_SubTexture* Tex3DS_GetSubTexture(const Tex3DS_Texture texture, size_t index);
void Tex3DS_TextureFree(Tex3DS_Texture texture);
