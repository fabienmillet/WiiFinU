/* -----------------------------------------------------------------------
 * Gfx3ds — GX (immediate mode) and GRRLIB on the 3DS's GPU, see Gfx3ds.h
 * and compat/ogc/gx.h.
 *
 * Vertices go to a buffer in linear memory (reset every frame: the GPU has
 * drawn the previous one when the next begins), each GX_End one draw call.
 * GX's projection, model-view matrix and viewport, the half-size mapping
 * to the screen and the screen's rotation are one matrix, computed on the
 * CPU when one of them changes.
 * ----------------------------------------------------------------------- */
#include "Gfx3ds.h"
#include "../../core/Text.h"

#include <3ds.h>
#include <citro3d.h>
#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

extern "C" {
extern const u8  wiifin_shbin[];
extern const u32 wiifin_shbin_size;
}

/* App.cpp: leave (the views unwind on it) */
extern volatile bool g_app_reset;

struct Tex3ds {
    u32     magic;          /* TEX_MAGIC: GX_InitTexObj tells it from texels */
    C3D_Tex tex;
    u16     w, h;           /* the picture's size (the texture is a power of 2) */
    u8      fmt;            /* Gfx3ds::Fmt */
    float   s, t;           /* texcoord extent of the picture */
};
static const u32 TEX_MAGIC = 0x54583344;   /* 'TX3D' */
namespace Gfx3ds { void freePending(); }

namespace {

struct Vertex { float x, y, z; float u, v; float r, g, b, a; };

const int    MAX_VERTS = 96 * 1024;
Vertex*      s_vbo = nullptr;
int          s_vcount = 0;           /* used this frame */
bool         s_overflowLogged = false;

DVLB_s*         s_dvlb = nullptr;
shaderProgram_s s_prog;
int             s_uProj = -1;

C3D_RenderTarget* s_targetTop = nullptr;
C3D_RenderTarget* s_targetBottom = nullptr;
bool  s_inFrame = false;
bool  s_onTop = false;               /* drawing the top screen (its drawer) */
unsigned s_frames = 0;
void (*s_topDrawer)() = nullptr;
bool  s_bottomOff = false, s_bottomIsOff = false;

/* GX state */
float s_proj[4][4];                  /* GX projection (guOrtho) */
float s_mv[3][4];                    /* GX model-view */
float s_vp[4] = { 0, 0, 640, 480 };  /* viewport, frame pixels */
bool  s_matDirty = true;
bool  s_texOn = false;               /* GX_VA_TEX0 direct */
u8    s_tevMode = GX_PASSCLR;
Tex3ds* s_curTex = nullptr;
u8    s_curFilter = GX_LINEAR;
int   s_envState = -1;               /* last TexEnv set: 0 colour, 1 texture */
Tex3ds* s_boundTex = nullptr;
u8    s_boundFilter = 0xFF;
/* scissor in frame pixels; w < 0: none */
int   s_clip[4] = { 0, 0, -1, -1 };
bool  s_clipDirty = true;

/* immediate mode: a primitive's vertices are gathered, made triangles at
 * GX_End and added to the batch, drawn in one call until the state
 * (texture, matrix, scissor) changes: the GPU's command buffer holds a
 * frame of WiiFin's thousands of small shapes and glyphs that way */
u8    s_prim = 0;
Vertex s_cur;
const int MAX_PRIM = 4096;
Vertex s_prim_v[MAX_PRIM];
int   s_primN = 0;
int   s_batchFirst = 0, s_batchN = 0;
bool  s_batchTex = false;

/* frame (GX's frame pixels) -> screen pixels */
const float FRAME_TO_SCREEN = 0.5f;

void identityMv(float m[3][4])
{
    memset(m, 0, sizeof(float) * 12);
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void orthoProj(float p[4][4], float t, float b, float l, float r)
{
    memset(p, 0, sizeof(float) * 16);
    p[0][0] = 2.0f / (r - l); p[0][3] = -(r + l) / (r - l);
    p[1][1] = 2.0f / (t - b); p[1][3] = -(t + b) / (t - b);
    p[2][2] = -1.0f;          p[3][3] = 1.0f;
}

void applyMatrix()
{
    /* drawing (x, y) -> model-view -> projection (NDC) -> viewport (frame
     * pixels) -> screen pixels: an affine map, rows a (x) and b (y) */
    float A[2][3];
    for (int row = 0; row < 2; ++row) {
        /* NDC row of P * MV */
        float pr[3];   /* coefficients of x, y and the constant */
        for (int c = 0; c < 3; ++c) {
            int col = c == 2 ? 3 : c;
            pr[c] = s_proj[row][0] * s_mv[0][col] + s_proj[row][1] * s_mv[1][col] +
                    s_proj[row][2] * s_mv[2][col];
        }
        pr[2] += s_proj[row][3];
        if (row == 0) {   /* fx = vx + (ndc + 1) * vw / 2 */
            float k = s_vp[2] * 0.5f;
            A[0][0] = pr[0] * k; A[0][1] = pr[1] * k; A[0][2] = pr[2] * k + s_vp[0] + k;
        } else {          /* fy = vy + (1 - ndc) * vh / 2 */
            float k = s_vp[3] * 0.5f;
            A[1][0] = -pr[0] * k; A[1][1] = -pr[1] * k; A[1][2] = -pr[2] * k + s_vp[1] + k;
        }
    }
    C3D_Mtx a, tilt, out;
    Mtx_Zeros(&a);
    a.r[0].x = A[0][0] * FRAME_TO_SCREEN; a.r[0].y = A[0][1] * FRAME_TO_SCREEN; a.r[0].w = A[0][2] * FRAME_TO_SCREEN;
    a.r[1].x = A[1][0] * FRAME_TO_SCREEN; a.r[1].y = A[1][1] * FRAME_TO_SCREEN; a.r[1].w = A[1][2] * FRAME_TO_SCREEN;
    a.r[2].z = 1.0f;
    a.r[3].w = 1.0f;
    Mtx_OrthoTilt(&tilt, 0.0f, s_onTop ? 400.0f : 320.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
    Mtx_Multiply(&out, &tilt, &a);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uProj, &out);
    s_matDirty = false;
}

void applyClip()
{
    if (s_clip[2] < 0) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    } else {
        /* frame pixels -> screen pixels -> the framebuffer (240 wide,
         * rotated: the screen's x is its y) */
        int x = (int)floorf(s_clip[0] * FRAME_TO_SCREEN), y = (int)floorf(s_clip[1] * FRAME_TO_SCREEN);
        int x2 = (int)ceilf((s_clip[0] + s_clip[2]) * FRAME_TO_SCREEN);
        int y2 = (int)ceilf((s_clip[1] + s_clip[3]) * FRAME_TO_SCREEN);
        const int W = s_onTop ? 400 : 320;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x2 > W) x2 = W;
        if (y2 > 240) y2 = 240;
        if (x2 < x) x2 = x;
        if (y2 < y) y2 = y;
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 240 - y2, x, 240 - y, x2);
    }
    s_clipDirty = false;
}

void beginFrame()
{
    if (s_inFrame) return;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    Gfx3ds::freePending();
    s_vcount = 0;
    s_inFrame = true;
    s_onTop = false;
    C3D_RenderTargetClear(s_targetBottom, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_RenderTargetClear(s_targetTop, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(s_targetBottom);
    s_matDirty = s_clipDirty = true;
    s_envState = -1;
    s_boundTex = nullptr;
    s_batchN = 0;
    s_batchFirst = 0;
}

void setEnv(bool tex)
{
    int want = tex ? 1 : 0;
    if (want == s_envState) return;
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (tex) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
    s_envState = want;
}

inline void colorOf(u32 c, Vertex& v)
{
    v.r = ((c >> 24) & 0xFF) / 255.0f;
    v.g = ((c >> 16) & 0xFF) / 255.0f;
    v.b = ((c >> 8) & 0xFF) / 255.0f;
    v.a = (c & 0xFF) / 255.0f;
}

inline void put(const Vertex& v) { s_vbo[s_vcount++] = v; }

void flushBatch()
{
    if (s_batchN > 0) C3D_DrawArrays(GPU_TRIANGLES, s_batchFirst, s_batchN);
    s_batchN = 0;
    s_batchFirst = s_vcount;
}

/* ---- textures ------------------------------------------------------------ */

int pow2(int v)
{
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

/* offset of texel (x, y) in a 3DS texture of width pw: 8x8 tiles, Morton
 * order inside.  y counts from the top of the picture, the texture's first
 * row in memory, which the GPU samples at t = 1 (as citro2d's pictures:
 * top 1.0) */
inline u32 tileOffset(u32 x, u32 y, u32 pw, u32 ph)
{
    (void)ph;
    u32 morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return (((y >> 3) * (pw >> 3) + (x >> 3)) << 6) + morton;
}

GPU_TEXCOLOR gpuFmt(Gfx3ds::Fmt f)
{
    switch (f) {
        case Gfx3ds::Fmt::RGB565: return GPU_RGB565;
        case Gfx3ds::Fmt::LA8:    return GPU_LA8;
        default:                  return GPU_RGBA8;
    }
}


/* pixel (x, y) of the source (shrunk by `shift`: 1 = every other) into the
 * texture, the edges repeated over the padding */
void fill(Tex3ds* t, const u8* src, int stride, int shift)
{
    const int pw = t->tex.width, ph = t->tex.height;
    const Gfx3ds::Fmt fmt = (Gfx3ds::Fmt)t->fmt;
    const int w = t->w, h = t->h;
    /* the padding gets the last row and column once (linear filtering at
     * the picture's edge), not the whole texture */
    const int fw = w < pw ? w + 1 : w, fh = h < ph ? h + 1 : h;
    if (fmt == Gfx3ds::Fmt::RGBA8) {
        u32* d = (u32*)t->tex.data;
        for (int y = 0; y < fh; ++y) {
            const u8* row = src + (size_t)((y < h ? y : h - 1) << shift) * stride;
            for (int x = 0; x < fw; ++x) {
                const u8* p = row + ((x < w ? x : w - 1) << shift) * 4;
                d[tileOffset(x, y, pw, ph)] = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
            }
        }
    } else {
        u16* d = (u16*)t->tex.data;
        for (int y = 0; y < fh; ++y) {
            const u8* row = src + (size_t)((y < h ? y : h - 1) << shift) * stride;
            for (int x = 0; x < fw; ++x) {
                const u8* p = row + ((x < w ? x : w - 1) << shift) * 2;
                u16 v = fmt == Gfx3ds::Fmt::RGB565 ? *(const u16*)p : (u16)(p[0] | (p[1] << 8));
                d[tileOffset(x, y, pw, ph)] = v;
            }
        }
    }
    C3D_TexFlush(&t->tex);
}

/* GX-format texels (4x4 tiles; RGBA8 in two halves: AR then GB) to rows */
std::vector<u8> gxToRows(const u8* gx, int w, int h, u8 fmt, Gfx3ds::Fmt& out)
{
    std::vector<u8> rows;
    if (fmt == GX_TF_RGBA8) {
        out = Gfx3ds::Fmt::RGBA8;
        rows.assign((size_t)w * h * 4, 0);
        const int tw = (w + 3) / 4;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const u8* tile = gx + ((y / 4) * tw + x / 4) * 64;
                int i = ((y & 3) * 4 + (x & 3)) * 2;
                u8* p = &rows[((size_t)y * w + x) * 4];
                p[3] = tile[i]; p[0] = tile[i + 1]; p[1] = tile[32 + i]; p[2] = tile[32 + i + 1];
            }
    } else if (fmt == GX_TF_RGB565 || fmt == GX_TF_IA8) {
        out = fmt == GX_TF_RGB565 ? Gfx3ds::Fmt::RGB565 : Gfx3ds::Fmt::LA8;
        rows.assign((size_t)w * h * 2, 0);
        const int tw = (w + 3) / 4;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const u8* tile = gx + ((y / 4) * tw + x / 4) * 32;
                int i = ((y & 3) * 4 + (x & 3)) * 2;
                u8* p = &rows[((size_t)y * w + x) * 2];
                if (fmt == GX_TF_RGB565) { u16 v = (u16)((tile[i] << 8) | tile[i + 1]); memcpy(p, &v, 2); }
                else { p[0] = tile[i]; p[1] = tile[i + 1]; }   /* alpha, intensity */
            }
    } else if (fmt == GX_TF_I8) {
        out = Gfx3ds::Fmt::LA8;
        rows.assign((size_t)w * h * 2, 0);
        const int tw = (w + 7) / 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const u8* tile = gx + ((y / 4) * tw + x / 8) * 32;
                u8 v = tile[(y & 3) * 8 + (x & 7)];
                u8* p = &rows[((size_t)y * w + x) * 2];
                p[0] = 0xFF; p[1] = v;
            }
    }
    return rows;
}

bool s_gfxUp = false;

} // namespace

/* ---- Gfx3ds -------------------------------------------------------------- */

namespace Gfx3ds {

void init()
{
    if (s_gfxUp) return;
    gfxInitDefault();
    gfxSet3D(false);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                      GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    s_targetTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s_targetTop, GFX_TOP, GFX_LEFT, flags);
    s_targetBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s_targetBottom, GFX_BOTTOM, GFX_LEFT, flags);

    s_dvlb = DVLB_ParseFile((u32*)wiifin_shbin, wiifin_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    C3D_BindProgram(&s_prog);
    s_uProj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 4);

    s_vbo = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_VERTS);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, s_vbo, sizeof(Vertex), 3, 0x210);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);

    orthoProj(s_proj, 0, 480, 0, 640);
    identityMv(s_mv);
    identityMv(GXmodelView2D);
    s_gfxUp = true;
    beginFrame();
}

void exit()
{
    if (!s_gfxUp) return;
    if (s_inFrame) { C3D_FrameEnd(0); s_inFrame = false; }
    if (s_bottomIsOff) { gspLcdInit(); GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); gspLcdExit(); }
    linearFree(s_vbo);
    shaderProgramFree(&s_prog);
    DVLB_Free(s_dvlb);
    C3D_Fini();
    gfxExit();
    s_gfxUp = false;
}

/* Textures live in linear memory (never VRAM): they are made and freed by
 * any thread (the image loaders), and the CPU fills them.  Freeing waits
 * for the frame being drawn: done at the next frame's start. */
static LightLock s_texLock;
static bool s_texLockInit = false;
static std::vector<Tex3ds*> s_toFree;

static void texLock()
{
    if (!s_texLockInit) { LightLock_Init(&s_texLock); s_texLockInit = true; }
    LightLock_Lock(&s_texLock);
}
static void texUnlock() { LightLock_Unlock(&s_texLock); }

Tex3ds* make(Fmt fmt, int w, int h, const void* pixels, int stride, bool vram)
{
    (void)vram;
    if (w <= 0 || h <= 0) return nullptr;
    int shift = 0;
    while ((w >> shift) > 1024 || (h >> shift) > 1024) ++shift;
    Tex3ds* t = (Tex3ds*)calloc(1, sizeof(Tex3ds));
    if (!t) return nullptr;
    t->magic = TEX_MAGIC;
    t->w = (u16)(w >> shift);
    t->h = (u16)(h >> shift);
    t->fmt = (u8)fmt;
    const int pw = pow2(t->w), ph = pow2(t->h);
    texLock();
    bool ok = C3D_TexInit(&t->tex, pw, ph, gpuFmt(fmt));
    texUnlock();
    if (!ok) { free(t); return nullptr; }
    if (pixels) fill(t, (const u8*)pixels, stride, shift);
    else { memset(t->tex.data, 0, t->tex.size); C3D_TexFlush(&t->tex); }
    C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    t->s = (float)t->w / pw;
    t->t = (float)t->h / ph;
    return t;
}

void update(Tex3ds* t, const void* pixels, int stride)
{
    if (t && pixels) fill(t, (const u8*)pixels, stride, 0);
}

void destroy(Tex3ds* t)
{
    if (!t || t->magic != TEX_MAGIC) return;
    t->magic = 0;
    texLock();
    s_toFree.push_back(t);
    texUnlock();
}

/* at a frame's start (the previous one drawn): what was freed since */
void freePending()
{
    texLock();
    std::vector<Tex3ds*> v;
    v.swap(s_toFree);
    for (Tex3ds* t : v) {
        if (s_curTex == t) s_curTex = nullptr;
        if (s_boundTex == t) s_boundTex = nullptr;
        C3D_TexDelete(&t->tex);
        free(t);
    }
    texUnlock();
}

void* pixels(Tex3ds* t, int& pw, int& ph)
{
    pw = t->tex.width;
    ph = t->tex.height;
    return t->tex.data;
}

int width(const Tex3ds* t)  { return t ? t->w : 0; }
int height(const Tex3ds* t) { return t ? t->h : 0; }

GRRLIB_texImg* wrap(Tex3ds* t)
{
    if (!t) return nullptr;
    GRRLIB_texImg* g = (GRRLIB_texImg*)calloc(1, sizeof(GRRLIB_texImg));
    if (!g) { destroy(t); return nullptr; }
    g->w = t->w;
    g->h = t->h;
    g->format = GX_TF_3DS;
    g->data = t;
    GRRLIB_SetHandle(g, 0, 0);
    return g;
}

void setTopDrawer(void (*draw)()) { s_topDrawer = draw; }
void setBottomOff(bool off) { s_bottomOff = off; }
unsigned frames() { return s_frames; }

} // namespace Gfx3ds

/* ---- GX ------------------------------------------------------------------ */

extern "C" {

Mtx GXmodelView2D;

void guMtxIdentity(Mtx mt) { identityMv(mt); }
void guMtxCopy(Mtx src, Mtx dst) { if (src != dst) memcpy(dst, src, sizeof(Mtx)); }
void guMtxTransApply(Mtx src, Mtx dst, f32 x, f32 y, f32 z)
{
    if (src != dst) memcpy(dst, src, sizeof(Mtx));
    dst[0][3] += x; dst[1][3] += y; dst[2][3] += z;
}
void guMtxScaleApply(Mtx src, Mtx dst, f32 x, f32 y, f32 z)
{
    for (int c = 0; c < 4; ++c) { dst[0][c] = src[0][c] * x; dst[1][c] = src[1][c] * y; dst[2][c] = src[2][c] * z; }
}
void guOrtho(Mtx44 mt, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    (void)n; (void)f;
    orthoProj(mt, t, b, l, r);
}

void GX_LoadProjectionMtx(Mtx44 mt, u8 type)
{
    (void)type;
    memcpy(s_proj, mt, sizeof(s_proj));
    s_matDirty = true;
}

void GX_LoadPosMtxImm(Mtx mt, u32 idx)
{
    (void)idx;
    memcpy(s_mv, mt, sizeof(s_mv));
    s_matDirty = true;
}

void GX_SetViewport(f32 x, f32 y, f32 w, f32 h, f32 n, f32 f)
{
    (void)n; (void)f;
    s_vp[0] = x; s_vp[1] = y; s_vp[2] = w; s_vp[3] = h;
    s_matDirty = true;
}

void GX_SetTevOp(u8 stage, u8 mode) { (void)stage; s_tevMode = mode; }
void GX_SetVtxDesc(u8 attr, u8 type) { if (attr == GX_VA_TEX0) s_texOn = type != GX_NONE; }

u32 GX_GetTexBufferSize(u16 w, u16 h, u32 fmt, u8 mip, u8 maxlod)
{
    (void)mip; (void)maxlod;
    u32 tw = (w + 3) & ~3u, th = (h + 3) & ~3u;
    switch (fmt) {
        case GX_TF_RGBA8: return tw * th * 4;
        case GX_TF_I8:    return ((w + 7) & ~7u) * th;
        default:          return tw * th * 2;
    }
}

void GX_InitTexObj(GXTexObj* obj, void* img, u16 w, u16 h, u8 fmt, u8 ws, u8 wt, u8 mip)
{
    (void)ws; (void)wt; (void)mip;
    obj->tex = nullptr; obj->owned = 0; obj->filter = GX_LINEAR;
    if (!img) return;
    if (((Tex3ds*)img)->magic == TEX_MAGIC) { obj->tex = img; return; }
    Gfx3ds::Fmt f;
    std::vector<u8> rows = gxToRows((const u8*)img, w, h, fmt, f);
    if (rows.empty()) return;
    obj->tex = Gfx3ds::make(f, w, h, rows.data(), w * (f == Gfx3ds::Fmt::RGBA8 ? 4 : 2));
    obj->owned = obj->tex != nullptr;
}

void GX_InitTexObjLOD(GXTexObj* obj, u8 minf, u8 magf, f32 a, f32 b, f32 c, u8 d, u8 e, u8 g)
{
    (void)minf; (void)a; (void)b; (void)c; (void)d; (void)e; (void)g;
    obj->filter = magf;
}

void GX_LoadTexObj(GXTexObj* obj, u8 map)
{
    if (map != GX_TEXMAP0) return;
    s_curTex = (Tex3ds*)obj->tex;
    s_curFilter = obj->filter;
}

void GX_DestroyTexObj(GXTexObj* obj)
{
    if (obj->owned && obj->tex) Gfx3ds::destroy((Tex3ds*)obj->tex);
    obj->tex = nullptr;
    obj->owned = 0;
}

void GX_Begin(u8 prim, u8 fmt, u16 n)
{
    (void)fmt; (void)n;
    beginFrame();
    s_prim = prim;
    s_primN = 0;
    memset(&s_cur, 0, sizeof(s_cur));
    s_cur.r = s_cur.g = s_cur.b = s_cur.a = 1.0f;
}

/* GX sends position, colour, texcoord: a vertex is complete at its last
 * attribute (texcoord when texturing, else colour) */
static void emit()
{
    if (s_primN < MAX_PRIM) s_prim_v[s_primN++] = s_cur;
}

static bool texturing() { return s_texOn && s_tevMode != GX_PASSCLR && s_curTex; }

void GX_Position3f32(f32 x, f32 y, f32 z) { s_cur.x = x; s_cur.y = y; s_cur.z = z; }

void GX_Color1u32(u32 c)
{
    colorOf(c, s_cur);
    if (!s_texOn) emit();
}

void GX_TexCoord2f32(f32 s, f32 t)
{
    if (s_curTex) { s_cur.u = s * s_curTex->s; s_cur.v = 1.0f - t * s_curTex->t; }
    if (s_texOn) emit();
}

void GX_End(void)
{
    const int n = s_primN;
    int tris = s_prim == GX_QUADS ? n / 4 * 2 : s_prim == GX_TRIANGLES ? n / 3 : n >= 3 ? n - 2 : 0;
    if (tris <= 0) return;
    if (s_vcount + tris * 3 > MAX_VERTS) {
        if (!s_overflowLogged) { s_overflowLogged = true; SYS_Report("[Gfx3ds] vertex buffer full\n"); }
        return;
    }
    const bool tex = texturing();
    GPU_TEXTURE_FILTER_PARAM f = s_curFilter == GX_NEAR ? GPU_NEAREST : GPU_LINEAR;
    const bool texChange = tex && (s_boundTex != s_curTex || s_boundFilter != s_curFilter);
    /* another state: what was gathered is drawn first */
    if (s_matDirty || s_clipDirty || tex != s_batchTex || texChange) {
        flushBatch();
        if (s_matDirty) applyMatrix();
        if (s_clipDirty) applyClip();
        setEnv(tex);
        if (texChange) {
            C3D_TexSetFilter(&s_curTex->tex, f, f);
            C3D_TexBind(0, &s_curTex->tex);
            s_boundTex = s_curTex;
            s_boundFilter = s_curFilter;
        }
        s_batchTex = tex;
    }
    const Vertex* v = s_prim_v;
    switch (s_prim) {
    case GX_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            put(v[i]); put(v[i + 1]); put(v[i + 2]);
            put(v[i]); put(v[i + 2]); put(v[i + 3]);
        }
        break;
    case GX_TRIANGLEFAN:
        for (int i = 1; i + 1 < n; ++i) { put(v[0]); put(v[i]); put(v[i + 1]); }
        break;
    case GX_TRIANGLESTRIP:
        for (int i = 0; i + 2 < n; ++i) { put(v[i]); put(v[i + 1]); put(v[i + 2]); }
        break;
    default:
        for (int i = 0; i + 2 < n; i += 3) { put(v[i]); put(v[i + 1]); put(v[i + 2]); }
        break;
    }
    s_batchN += tris * 3;
}

/* ---- GRRLIB -------------------------------------------------------------- */

static FT_Library s_ft = nullptr;

int GRRLIB_Init(void)
{
    Gfx3ds::init();
    if (!s_ft) FT_Init_FreeType(&s_ft);
    return 0;
}

void GRRLIB_Exit(void)
{
    if (s_ft) { FT_Done_FreeType(s_ft); s_ft = nullptr; }
    Gfx3ds::exit();
}

void GRRLIB_Render(void)
{
    beginFrame();
    flushBatch();
    if (s_topDrawer) {
        /* the top screen: its own 800x480 space */
        float proj[4][4], mv[3][4], vp[4], gmv[3][4];
        int clip[4];
        memcpy(proj, s_proj, sizeof(proj)); memcpy(mv, s_mv, sizeof(mv));
        memcpy(vp, s_vp, sizeof(vp)); memcpy(clip, s_clip, sizeof(clip));
        memcpy(gmv, GXmodelView2D, sizeof(gmv));
        C3D_FrameDrawOn(s_targetTop);
        s_onTop = true;
        orthoProj(s_proj, 0, 480, 0, 800);
        identityMv(s_mv);
        identityMv(GXmodelView2D);
        s_vp[0] = 0; s_vp[1] = 0; s_vp[2] = 800; s_vp[3] = 480;
        s_clip[2] = -1;
        s_matDirty = s_clipDirty = true;
        s_topDrawer();
        flushBatch();
        memcpy(s_proj, proj, sizeof(proj)); memcpy(s_mv, mv, sizeof(mv));
        memcpy(s_vp, vp, sizeof(vp)); memcpy(s_clip, clip, sizeof(clip));
        memcpy(GXmodelView2D, gmv, sizeof(gmv));
        s_onTop = false;
        s_matDirty = s_clipDirty = true;
    }
    flushBatch();
    {   /* how full the GPU's command buffer got (it drops what doesn't fit) */
        static float s_maxUse = 0;
        float use = C3D_GetCmdBufUsage();
        if (use > s_maxUse + 0.1f) { s_maxUse = use; SYS_Report("[Gfx3ds] command buffer %.0f%% used\n", use * 100); }
    }
    C3D_FrameEnd(0);
    s_inFrame = false;
    ++s_frames;
    /* the system's events (HOME menu, sleep, the app closed from there) */
    if (!aptMainLoop()) g_app_reset = true;
    if (s_bottomOff != s_bottomIsOff) {
        gspLcdInit();
        if (s_bottomOff) GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
        else             GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
        gspLcdExit();
        s_bottomIsOff = s_bottomOff;
    }
    beginFrame();
}

void GRRLIB_FillScreen(u32 color)
{
    GRRLIB_Rectangle(-200, -40, 1200, 560, color, true);
}

void GRRLIB_Rectangle(f32 x, f32 y, f32 w, f32 h, u32 color, bool filled)
{
    if (!filled) {
        const float t = 1.0f;
        GRRLIB_Rectangle(x, y, w, t, color, true);
        GRRLIB_Rectangle(x, y + h - t, w, t, color, true);
        GRRLIB_Rectangle(x, y, t, h, color, true);
        GRRLIB_Rectangle(x + w - t, y, t, h, color, true);
        return;
    }
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, 0);         GX_Color1u32(color);
    GX_Position3f32(x + w, y, 0);     GX_Color1u32(color);
    GX_Position3f32(x + w, y + h, 0); GX_Color1u32(color);
    GX_Position3f32(x, y + h, 0);     GX_Color1u32(color);
    GX_End();
}

void GRRLIB_ClipDrawing(int x, int y, int w, int h)
{
    s_clip[0] = x; s_clip[1] = y; s_clip[2] = w < 0 ? 0 : w; s_clip[3] = h < 0 ? 0 : h;
    s_clipDirty = true;
}

void GRRLIB_ClipReset(void)
{
    s_clip[2] = -1;
    s_clipDirty = true;
}

void GRRLIB_SetHandle(GRRLIB_texImg* tex, int x, int y)
{
    tex->handlex = -((int)tex->w / 2) + x;
    tex->handley = -((int)tex->h / 2) + y;
}

void GRRLIB_SetMidHandle(GRRLIB_texImg* tex, bool enabled)
{
    if (enabled) {
        tex->offsetx = (int)tex->w / 2;
        tex->offsety = (int)tex->h / 2;
        GRRLIB_SetHandle(tex, tex->offsetx, tex->offsety);
    } else {
        GRRLIB_SetHandle(tex, 0, 0);
        tex->offsetx = 0;
        tex->offsety = 0;
    }
}

void GRRLIB_DrawImg(f32 xpos, f32 ypos, const GRRLIB_texImg* tex, f32 degrees,
                    f32 scaleX, f32 scaleY, u32 color)
{
    if (!tex || !tex->data) return;
    GXTexObj obj;
    GX_InitTexObj(&obj, tex->data, tex->w, tex->h, tex->format, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_LoadTexObj(&obj, GX_TEXMAP0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    const float rad = -degrees * (float)M_PI / 180.0f;
    const float width = tex->w * 0.5f, height = tex->h * 0.5f;
    const float tx = xpos + width + tex->handlex - tex->offsetx +
                     scaleX * (-tex->handley * sinf(rad) - tex->handlex * cosf(rad));
    const float ty = ypos + height + tex->handley - tex->offsety +
                     scaleY * (-tex->handley * cosf(rad) + tex->handlex * sinf(rad));
    /* scale, rotate, translate, then the 2D model-view */
    const float c = cosf(degrees * (float)M_PI / 180.0f), s = sinf(degrees * (float)M_PI / 180.0f);
    Mtx m;
    m[0][0] = c * scaleX; m[0][1] = -s * scaleY; m[0][2] = 0; m[0][3] = tx;
    m[1][0] = s * scaleX; m[1][1] =  c * scaleY; m[1][2] = 0; m[1][3] = ty;
    m[2][0] = 0;          m[2][1] = 0;           m[2][2] = 1; m[2][3] = 0;
    Mtx mv;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k)
            mv[r][k] = GXmodelView2D[r][0] * m[0][k] + GXmodelView2D[r][1] * m[1][k] +
                       GXmodelView2D[r][2] * m[2][k] + (k == 3 ? GXmodelView2D[r][3] : 0.0f);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(-width, -height, 0); GX_Color1u32(color); GX_TexCoord2f32(0, 0);
    GX_Position3f32(width, -height, 0);  GX_Color1u32(color); GX_TexCoord2f32(1, 0);
    GX_Position3f32(width, height, 0);   GX_Color1u32(color); GX_TexCoord2f32(1, 1);
    GX_Position3f32(-width, height, 0);  GX_Color1u32(color); GX_TexCoord2f32(0, 1);
    GX_End();
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
}

void GRRLIB_FreeTexture(GRRLIB_texImg* tex)
{
    if (!tex) return;
    if (tex->data && ((Tex3ds*)tex->data)->magic == TEX_MAGIC) Gfx3ds::destroy((Tex3ds*)tex->data);
    else free(tex->data);
    free(tex);
}

/* A texture made on the CPU in a GX format becomes a GPU one */
void GRRLIB_FlushTex(GRRLIB_texImg* tex)
{
    if (!tex || !tex->data || ((Tex3ds*)tex->data)->magic == TEX_MAGIC) return;
    GXTexObj obj;
    GX_InitTexObj(&obj, tex->data, tex->w, tex->h, tex->format, GX_CLAMP, GX_CLAMP, GX_FALSE);
    if (!obj.tex) return;
    free(tex->data);
    tex->data = obj.tex;
    tex->format = GX_TF_3DS;
}

GRRLIB_texImg* GRRLIB_CreateEmptyTexture(u32 w, u32 h)
{
    return Gfx3ds::wrap(Gfx3ds::make(Gfx3ds::Fmt::RGBA8, (int)w, (int)h, nullptr, 0));
}

/* PNG: its size from its chunks (GRRLIB_LoadTexture gets no size) */
static size_t pngSize(const u8* d)
{
    size_t off = 8;
    for (int i = 0; i < 100000; ++i) {
        u32 len = ((u32)d[off] << 24) | ((u32)d[off + 1] << 16) | ((u32)d[off + 2] << 8) | d[off + 3];
        bool end = memcmp(d + off + 4, "IEND", 4) == 0;
        off += 12 + len;
        if (end) return off;
    }
    return 0;
}

GRRLIB_texImg* GRRLIB_LoadTexturePNG(const u8* data)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, data, pngSize(data))) return nullptr;
    img.format = PNG_FORMAT_RGBA;
    std::vector<u8> px(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, nullptr, px.data(), 0, nullptr)) { png_image_free(&img); return nullptr; }
    return Gfx3ds::wrap(Gfx3ds::make(Gfx3ds::Fmt::RGBA8, img.width, img.height, px.data(), img.width * 4));
}

struct JpegErr { jpeg_error_mgr pub; jmp_buf buf; };
static void jpegErrExit(j_common_ptr c) { longjmp(((JpegErr*)c->err)->buf, 1); }
static void jpegNoOutput(j_common_ptr) {}

GRRLIB_texImg* GRRLIB_LoadTextureJPGEx(const u8* data, int size)
{
    jpeg_decompress_struct cinfo;
    JpegErr err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpegErrExit;
    err.pub.output_message = jpegNoOutput;
    std::vector<u8> px;
    if (setjmp(err.buf)) { jpeg_destroy_decompress(&cinfo); return nullptr; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, size);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_RGBA;
    while ((cinfo.image_width / cinfo.scale_denom > 1024 || cinfo.image_height / cinfo.scale_denom > 1024) &&
           cinfo.scale_denom < 8)
        cinfo.scale_denom *= 2;
    jpeg_start_decompress(&cinfo);
    const int w = cinfo.output_width, h = cinfo.output_height;
    px.resize((size_t)w * h * 4);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = &px[(size_t)cinfo.output_scanline * w * 4];
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return Gfx3ds::wrap(Gfx3ds::make(Gfx3ds::Fmt::RGBA8, w, h, px.data(), w * 4));
}

GRRLIB_texImg* GRRLIB_LoadTexture(const u8* data)
{
    if (!data) return nullptr;
    if (data[0] == 0xFF && data[1] == 0xD8) {
        int n = 2;
        while (n < 64 * 1024 * 1024 && !(data[n] == 0xFF && data[n + 1] == 0xD9)) ++n;
        return GRRLIB_LoadTextureJPGEx(data, n + 2);
    }
    return GRRLIB_LoadTexturePNG(data);
}

GRRLIB_ttfFont* GRRLIB_LoadTTF(const u8* base, s32 size)
{
    if (!s_ft && FT_Init_FreeType(&s_ft) != 0) return nullptr;
    FT_Face face;
    if (FT_New_Memory_Face(s_ft, base, size, 0, &face) != 0) return nullptr;
    GRRLIB_ttfFont* f = (GRRLIB_ttfFont*)malloc(sizeof(GRRLIB_ttfFont));
    if (!f) { FT_Done_Face(face); return nullptr; }
    f->face = face;
    f->kerning = FT_HAS_KERNING(face);
    return f;
}

void GRRLIB_FreeTTF(GRRLIB_ttfFont* font)
{
    if (!font) return;
    FT_Done_Face((FT_Face)font->face);
    free(font);
}

void GRRLIB_PrintfTTF(int x, int y, GRRLIB_ttfFont* font, const char* s, unsigned int size, const u32 color)
{
    Text::print(x, y, font, s, size, color);
}

u32 GRRLIB_WidthTTF(GRRLIB_ttfFont* font, const char* s, unsigned int size)
{
    return Text::width(font, s, size);
}

} // extern "C"
