#pragma once
/* -----------------------------------------------------------------------
 * The part of the Wii's GX that WiiFin draws with, on the 3DS's GPU
 * (citro3d): immediate mode (GX_Begin / GX_Position / GX_Color /
 * GX_TexCoord / GX_End), one texture, colour or texture x colour, an
 * orthographic projection, the model-view matrix, the viewport.  See
 * Gfx3ds.cpp.
 * ----------------------------------------------------------------------- */
#include "../gctypes.h"
#include "gu.h"
#ifdef __cplusplus
extern "C" {
#endif

/* primitives */
#define GX_QUADS         0x80
#define GX_TRIANGLES     0x90
#define GX_TRIANGLESTRIP 0x98
#define GX_TRIANGLEFAN   0xA0
#define GX_LINES         0xA8
#define GX_LINESTRIP     0xB0
#define GX_POINTS        0xB8

#define GX_VTXFMT0 0
#define GX_VTXFMT1 1
#define GX_VTXFMT5 5
#define GX_PNMTX0  0
#define GX_TEVSTAGE0 0
#define GX_TEXMAP0 0
#define GX_TEXMAP1 1
#define GX_TEXMAP2 2
#define GX_TEXCOORD0 0
#define GX_COLOR0A0 4

#define GX_FALSE 0
#define GX_TRUE  1
#define GX_DISABLE 0
#define GX_ENABLE  1

/* vertex attributes */
#define GX_VA_POS  9
#define GX_VA_CLR0 11
#define GX_VA_TEX0 13
#define GX_NONE   0
#define GX_DIRECT 1

/* TEV modes */
#define GX_MODULATE 0
#define GX_DECAL    1
#define GX_BLEND    2
#define GX_REPLACE  3
#define GX_PASSCLR  4

/* texture formats (the Wii's numbers) */
#define GX_TF_I4     0x0
#define GX_TF_I8     0x1
#define GX_TF_IA4    0x2
#define GX_TF_IA8    0x3
#define GX_TF_RGB565 0x4
#define GX_TF_RGB5A3 0x5
#define GX_TF_RGBA8  0x6
/* not the Wii's: a texture already on the 3DS's GPU (Gfx3ds) */
#define GX_TF_3DS    0x7F

#define GX_CLAMP  0
#define GX_REPEAT 1
#define GX_MIRROR 2
#define GX_NEAR   0
#define GX_LINEAR 1
#define GX_ANISO_1 0
#define GX_ORTHOGRAPHIC 1
#define GX_PERSPECTIVE  0

/* A texture: on the 3DS, a GPU texture (Tex3ds, Gfx3ds.cpp).  From
 * GX-format texels (GX_InitTexObj on pixels: Text's glyphs) it is made
 * then and owned; GX_DestroyTexObj frees it. */
typedef struct {
    void* tex;     /* struct Tex3ds* */
    u8    owned;
    u8    filter;
} GXTexObj;

void GX_InitTexObj(GXTexObj* obj, void* img, u16 wd, u16 ht, u8 fmt, u8 wrap_s, u8 wrap_t, u8 mipmap);
void GX_InitTexObjLOD(GXTexObj* obj, u8 minfilt, u8 magfilt, f32 minlod, f32 maxlod, f32 lodbias,
                      u8 biasclamp, u8 edgelod, u8 maxaniso);
void GX_LoadTexObj(GXTexObj* obj, u8 mapid);
void GX_DestroyTexObj(GXTexObj* obj);
static inline void GX_InvalidateTexAll(void) {}
u32  GX_GetTexBufferSize(u16 wd, u16 ht, u32 fmt, u8 mipmap, u8 maxlod);

void GX_SetTevOp(u8 tevstage, u8 mode);
void GX_SetVtxDesc(u8 attr, u8 type);
static inline void GX_ClearVtxDesc(void) {}
static inline void GX_SetVtxAttrFmt(u8 a, u32 b, u32 c, u32 d, u32 e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void GX_LoadPosMtxImm(Mtx mt, u32 pnidx);
void GX_LoadProjectionMtx(Mtx44 mt, u8 type);
void GX_SetViewport(f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ);

void GX_Begin(u8 primitve, u8 vtxfmt, u16 vtxcnt);
void GX_Position3f32(f32 x, f32 y, f32 z);
static inline void GX_Position2f32(f32 x, f32 y) { GX_Position3f32(x, y, 0.0f); }
void GX_Color1u32(u32 clr);
static inline void GX_Color4u8(u8 r, u8 g, u8 b, u8 a)
{ GX_Color1u32(((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a); }
void GX_TexCoord2f32(f32 s, f32 t);
void GX_End(void);

static inline void GX_Init(void* fifo, u32 size) { (void)fifo; (void)size; }
static inline void GX_AbortFrame(void) {}
static inline void GX_Flush(void) {}
static inline void GX_DrawDone(void) {}

#ifdef __cplusplus
}
#endif
