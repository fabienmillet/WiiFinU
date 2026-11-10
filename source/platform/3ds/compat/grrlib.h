#pragma once
/* -----------------------------------------------------------------------
 * The part of GRRLIB WiiFin uses, on the 3DS (Gfx3ds.cpp).
 *
 * A texture's data is a Tex3ds (a texture on the 3DS's GPU), not GX
 * texels: free it with GRRLIB_FreeTexture.  GX_InitTexObj on it uses it
 * as it is.  Textures made on the CPU in a GX format (data = the texels,
 * format = GX_TF_*) become one at GRRLIB_FlushTex.
 * ----------------------------------------------------------------------- */
#include "gccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GRRLIB_texImg {
    u32   w, h;
    u32   format;
    int   handlex, handley;
    int   offsetx, offsety;
    bool  tiledtex;
    u32   tilew, tileh, nbtilew, nbtileh, tilestart;
    f32   ofnormaltexx, ofnormaltexy;
    void* data;
} GRRLIB_texImg;

typedef struct GRRLIB_Font {
    void* face;     /* FT_Face */
    bool  kerning;
} GRRLIB_ttfFont;

extern Mtx GXmodelView2D;

int  GRRLIB_Init(void);
void GRRLIB_Exit(void);
void GRRLIB_Render(void);
void GRRLIB_FillScreen(u32 color);

GRRLIB_texImg* GRRLIB_LoadTexture(const u8* data);
GRRLIB_texImg* GRRLIB_LoadTexturePNG(const u8* data);
GRRLIB_texImg* GRRLIB_LoadTextureJPGEx(const u8* data, int size);
GRRLIB_texImg* GRRLIB_CreateEmptyTexture(u32 w, u32 h);
void GRRLIB_FreeTexture(GRRLIB_texImg* tex);
void GRRLIB_FlushTex(GRRLIB_texImg* tex);
void GRRLIB_SetHandle(GRRLIB_texImg* tex, int x, int y);
void GRRLIB_SetMidHandle(GRRLIB_texImg* tex, bool enabled);

void GRRLIB_DrawImg(f32 xpos, f32 ypos, const GRRLIB_texImg* tex, f32 degrees,
                    f32 scaleX, f32 scaleY, u32 color);
void GRRLIB_Rectangle(f32 x, f32 y, f32 width, f32 height, u32 color, bool filled);
void GRRLIB_ClipDrawing(int x, int y, int width, int height);
void GRRLIB_ClipReset(void);

GRRLIB_ttfFont* GRRLIB_LoadTTF(const u8* file_base, s32 file_size);
void GRRLIB_FreeTTF(GRRLIB_ttfFont* font);
void GRRLIB_PrintfTTF(int x, int y, GRRLIB_ttfFont* font, const char* string,
                      unsigned int fontSize, const u32 color);
u32  GRRLIB_WidthTTF(GRRLIB_ttfFont* font, const char* string, unsigned int fontSize);

#define GRRLIB_BLEND_ALPHA 0
#define GRRLIB_BLEND_ADD   1
static inline void GRRLIB_SetBlend(int mode) { (void)mode; }

#ifdef __cplusplus
}
#endif
