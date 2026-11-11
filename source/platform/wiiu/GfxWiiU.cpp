/* -----------------------------------------------------------------------
 * GfxWiiU — GX (immediate mode) and GRRLIB on the Wii U's GPU (GX2), see
 * GfxWiiU.h and compat/ogc/gx.h.
 *
 * A frame is recorded: vertices (moved by GX's projection, model-view and
 * viewport on the CPU, into the Wii's 640x480 frame pixels) and batches
 * (a texture, a filter, a scissor, the vertices drawn with them).  At
 * GRRLIB_Render the frame is drawn on each screen of the mode, stretched
 * to it (the frame's 640x480 is the 16:9 drawing space: Ui widens GX's
 * projection), with one shader: SDL's texture x colour, a white texel for
 * what has no texture.  The video's NV12 pictures are drawn with
 * Moonlight's shader, then brought from video range to full range by two
 * blended quads.  Remote mode: the GamePad's own frame, recorded by its
 * drawer at GRRLIB_Render, drawn on the GamePad only.
 *
 * The system (ProcUI, through WHBProc) is asked at each GRRLIB_Render:
 * the HOME menu (WiiFin waits there, in the background) and the app
 * closed from it (g_app_reset: WiiFin unwinds, drawing nothing more).
 * ----------------------------------------------------------------------- */
#include "GfxWiiU.h"
#include "../../core/Text.h"
#include <ogc/system.h>
#include "shaders/texture_gsh.h"
#include "shaders/nv12_gsh.h"

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <proc_ui/procui.h>
#include <whb/gfx.h>
#include <whb/proc.h>

#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <vector>

/* App.cpp: leave (the views unwind on it) */
extern volatile bool g_app_reset;

struct TexWiiU {
    u32        magic;        /* TEX_MAGIC: GX_InitTexObj tells it from texels */
    GX2Texture tex;
    u16        w, h;
    u8         fmt;          /* GfxWiiU::Fmt */
    u8         bpp;
    bool       freeImage;    /* its image is its own (else someone else's) */
};
static const u32 TEX_MAGIC = 0x54585755;   /* 'TXWU' */

namespace {

struct Vtx { float x, y, r, g, b, a, u, v; };
static_assert(sizeof(Vtx) == 32, "vertex layout");

enum Kind : u8 { K_TEX, K_NV12, K_POST_SUB, K_POST_MUL };

struct Batch {
    int      first, count;
    TexWiiU* tex;            /* nullptr: the white texel */
    TexWiiU* tex2;           /* NV12: the UV plane */
    u8       kind, filter;
    bool     bt709;
    int      clip[4];        /* frame pixels; clip[2] < 0: none */
};

const int  MAX_VERTS = 192 * 1024;
Vtx*       s_vbuf = nullptr;
int        s_vcount = 0;
bool       s_overflowLogged = false;

std::vector<Batch> s_main, s_remote;
std::vector<Batch>* s_list = &s_main;   /* recording into */

WHBGfxShaderGroup s_texShader, s_nv12Shader[2];
u32  s_texProjLoc = 0, s_texSamplerLoc = 0;
u32  s_nv12SizeLoc = 0;
GX2Sampler s_sampLinear, s_sampNear;
TexWiiU* s_white = nullptr;

bool  s_up = false;
bool  s_inFrame = false;
bool  s_exiting = false;
unsigned s_frames = 0;
GfxWiiU::Mode s_mode = GfxWiiU::Mode::Mirror;
void (*s_remoteDrawer)() = nullptr;
void (*s_fgHook)(bool) = nullptr;
const GRRLIB_texImg* s_cursorTex = nullptr;
bool  s_hideCursor = false;

/* GX state */
float s_proj[4][4];
float s_mv[3][4];
float s_vp[4] = { 0, 0, 640, 480 };
float s_A[2][3];                     /* drawing -> frame pixels */
bool  s_matDirty = true;
bool  s_texOn = false;
u8    s_tevMode = GX_PASSCLR;
TexWiiU* s_curTex = nullptr;
u8    s_curFilter = GX_LINEAR;
int   s_clip[4] = { 0, 0, -1, -1 };

/* immediate mode: a primitive's vertices, made triangles at GX_End */
u8    s_prim = 0;
Vtx   s_cur;
float s_curX = 0, s_curY = 0;
const int MAX_PRIM = 4096;
Vtx   s_primV[MAX_PRIM];
int   s_primN = 0;

/* textures to free once the GPU is done with them */
std::mutex s_texMutex;
std::vector<TexWiiU*> s_toFree;

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

/* drawing (x, y) -> model-view -> projection (NDC) -> viewport (frame
 * pixels): an affine map */
void applyMatrix()
{
    for (int row = 0; row < 2; ++row) {
        float pr[3];
        for (int c = 0; c < 3; ++c) {
            int col = c == 2 ? 3 : c;
            pr[c] = s_proj[row][0] * s_mv[0][col] + s_proj[row][1] * s_mv[1][col] +
                    s_proj[row][2] * s_mv[2][col];
        }
        pr[2] += s_proj[row][3];
        if (row == 0) {
            float k = s_vp[2] * 0.5f;
            s_A[0][0] = pr[0] * k; s_A[0][1] = pr[1] * k; s_A[0][2] = pr[2] * k + s_vp[0] + k;
        } else {
            float k = s_vp[3] * 0.5f;
            s_A[1][0] = -pr[0] * k; s_A[1][1] = -pr[1] * k; s_A[1][2] = -pr[2] * k + s_vp[1] + k;
        }
    }
    s_matDirty = false;
}

void freePending()
{
    std::vector<TexWiiU*> v;
    {
        std::lock_guard<std::mutex> g(s_texMutex);
        v.swap(s_toFree);
    }
    for (TexWiiU* t : v) {
        if (t->freeImage) free(t->tex.surface.image);
        free(t);
    }
}

void beginFrame()
{
    if (s_inFrame) return;
    s_inFrame = true;
    s_vcount = 0;
    s_main.clear();
    s_remote.clear();
    s_list = &s_main;
    s_matDirty = true;
}

/* the batch the next triangles go to: the last one if its state is theirs */
Batch& batchFor(u8 kind, TexWiiU* tex, TexWiiU* tex2, u8 filter, bool bt709)
{
    std::vector<Batch>& l = *s_list;
    if (!l.empty()) {
        Batch& b = l.back();
        if (b.kind == kind && kind == K_TEX && b.tex == tex && b.filter == filter &&
            b.first + b.count == s_vcount && memcmp(b.clip, s_clip, sizeof(s_clip)) == 0)
            return b;
    }
    Batch b;
    b.first = s_vcount; b.count = 0;
    b.tex = tex; b.tex2 = tex2; b.kind = kind; b.filter = filter; b.bt709 = bt709;
    memcpy(b.clip, s_clip, sizeof(s_clip));
    l.push_back(b);
    return l.back();
}

inline void colorOf(u32 c, Vtx& v)
{
    v.r = ((c >> 24) & 0xFF) * (1.0f / 255.0f);
    v.g = ((c >> 16) & 0xFF) * (1.0f / 255.0f);
    v.b = ((c >> 8) & 0xFF) * (1.0f / 255.0f);
    v.a = (c & 0xFF) * (1.0f / 255.0f);
}

/* ---- textures ------------------------------------------------------------ */

GX2SurfaceFormat gx2Fmt(GfxWiiU::Fmt f)
{
    switch (f) {
        case GfxWiiU::Fmt::LA8: case GfxWiiU::Fmt::RG8: return GX2_SURFACE_FORMAT_UNORM_R8_G8;
        case GfxWiiU::Fmt::R8:  return GX2_SURFACE_FORMAT_UNORM_R8;
        default:                return GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    }
}

u32 compMap(GfxWiiU::Fmt f)
{
    switch (f) {
        case GfxWiiU::Fmt::LA8: return GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_R, GX2_SQ_SEL_R, GX2_SQ_SEL_G);
        case GfxWiiU::Fmt::RG8: return GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
        case GfxWiiU::Fmt::R8:  return GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
        default:                return GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
    }
}

int bppOf(GfxWiiU::Fmt f)
{
    switch (f) {
        case GfxWiiU::Fmt::LA8: case GfxWiiU::Fmt::RG8: return 2;
        case GfxWiiU::Fmt::R8:  return 1;
        default:                return 4;
    }
}

/* rows (shrunk by `shift`) into the texture */
void fill(TexWiiU* t, const u8* src, int stride, int shift)
{
    u8* dst = (u8*)t->tex.surface.image;
    const int pitchB = t->tex.surface.pitch * t->bpp, rowB = t->w * t->bpp;
    for (int y = 0; y < t->h; ++y) {
        const u8* row = src + (size_t)(y << shift) * stride;
        u8* d = dst + (size_t)y * pitchB;
        if (shift == 0) memcpy(d, row, rowB);
        else for (int x = 0; x < t->w; ++x) memcpy(d + x * t->bpp, row + ((x << shift) * t->bpp), t->bpp);
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, t->tex.surface.image, t->tex.surface.imageSize);
}

/* GX-format texels (4x4 tiles; RGBA8 in two halves: AR then GB) to rows */
std::vector<u8> gxToRows(const u8* gx, int w, int h, u8 fmt, GfxWiiU::Fmt& out)
{
    std::vector<u8> rows;
    if (fmt == GX_TF_RGBA8) {
        out = GfxWiiU::Fmt::RGBA8;
        rows.assign((size_t)w * h * 4, 0);
        const int tw = (w + 3) / 4;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const u8* tile = gx + ((y / 4) * tw + x / 4) * 64;
                int i = ((y & 3) * 4 + (x & 3)) * 2;
                u8* p = &rows[((size_t)y * w + x) * 4];
                p[3] = tile[i]; p[0] = tile[i + 1]; p[1] = tile[32 + i]; p[2] = tile[32 + i + 1];
            }
    } else if (fmt == GX_TF_RGB565) {
        out = GfxWiiU::Fmt::RGBA8;
        rows.assign((size_t)w * h * 4, 0);
        const int tw = (w + 3) / 4;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const u8* tile = gx + ((y / 4) * tw + x / 4) * 32;
                int i = ((y & 3) * 4 + (x & 3)) * 2;
                u16 v = (u16)((tile[i] << 8) | tile[i + 1]);
                u8* p = &rows[((size_t)y * w + x) * 4];
                p[0] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
                p[1] = (u8)(((v >> 5) & 0x3F) * 255 / 63);
                p[2] = (u8)((v & 0x1F) * 255 / 31);
                p[3] = 0xFF;
            }
    } else if (fmt == GX_TF_IA8) {
        out = GfxWiiU::Fmt::LA8;
        rows.assign((size_t)w * h * 2, 0);
        const int tw = (w + 3) / 4;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const u8* tile = gx + ((y / 4) * tw + x / 4) * 32;
                int i = ((y & 3) * 4 + (x & 3)) * 2;
                u8* p = &rows[((size_t)y * w + x) * 2];
                p[0] = tile[i + 1]; p[1] = tile[i];   /* luminance, alpha */
            }
    } else if (fmt == GX_TF_I8) {
        out = GfxWiiU::Fmt::LA8;
        rows.assign((size_t)w * h * 2, 0);
        const int tw = (w + 7) / 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const u8* tile = gx + ((y / 4) * tw + x / 8) * 32;
                u8* p = &rows[((size_t)y * w + x) * 2];
                p[0] = tile[(y & 3) * 8 + (x & 7)]; p[1] = 0xFF;
            }
    }
    return rows;
}

/* ---- drawing on a screen ---------------------------------------------------- */


void setCommonState()
{
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD);
}

void useTexShader()
{
    GX2SetFetchShader(&s_texShader.fetchShader);
    GX2SetVertexShader(s_texShader.vertexShader);
    GX2SetPixelShader(s_texShader.pixelShader);
    /* frame pixels (640x480) -> NDC, the whole screen: x * C0 + y * C1 + C3 */
    static float proj[16] = {
        2.0f / 640.0f, 0, 0, 0,
        0, -2.0f / 480.0f, 0, 0,
        0, 0, 1, 0,
        -1.0f, 1.0f, 0, 1,
    };
    GX2SetVertexUniformReg(s_texProjLoc, 16, proj);
    GX2SetAttribBuffer(0, (u32)(s_vcount * sizeof(Vtx)), sizeof(Vtx), s_vbuf);
}

/* the frame's batches on the colour buffer of w x h pixels */
void drawList(const std::vector<Batch>& list, int w, int h)
{
    const float sx = w / 640.0f, sy = h / 480.0f;
    int clipNow[4] = { -2, -2, -2, -2 };
    TexWiiU* texNow = nullptr;
    int filterNow = -1;
    int shaderNow = 0;   /* 0 texture, 1 NV12 */
    useTexShader();
    for (const Batch& b : list) {
        if (b.count <= 0) continue;
        if (memcmp(clipNow, b.clip, sizeof(clipNow)) != 0) {
            memcpy(clipNow, b.clip, sizeof(clipNow));
            if (b.clip[2] < 0) GX2SetScissor(0, 0, w, h);
            else {
                int x0 = (int)floorf(b.clip[0] * sx), y0 = (int)floorf(b.clip[1] * sy);
                int x1 = (int)ceilf((b.clip[0] + b.clip[2]) * sx), y1 = (int)ceilf((b.clip[1] + b.clip[3]) * sy);
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > w) x1 = w;
                if (y1 > h) y1 = h;
                if (x1 < x0) x1 = x0;
                if (y1 < y0) y1 = y0;
                GX2SetScissor(x0, y0, x1 - x0, y1 - y0);
            }
        }
        if (b.kind == K_NV12) {
            WHBGfxShaderGroup& sh = s_nv12Shader[b.bt709 ? 1 : 0];
            GX2SetFetchShader(&sh.fetchShader);
            GX2SetVertexShader(sh.vertexShader);
            GX2SetPixelShader(sh.pixelShader);
            static float size[4] = { 1.0f / 640.0f, 1.0f / 480.0f, 0, 0 };
            GX2SetVertexUniformReg(s_nv12SizeLoc, 4, size);
            GX2SetAttribBuffer(0, (u32)(s_vcount * sizeof(Vtx)), sizeof(Vtx), s_vbuf);
            GX2SetPixelTexture(&b.tex->tex, 0);
            GX2SetPixelTexture(&b.tex2->tex, 1);
            GX2SetPixelSampler(&s_sampLinear, 0);
            GX2SetPixelSampler(&s_sampLinear, 1);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);   /* no blending */
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, b.count, b.first, 1);
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
            shaderNow = 1;
            texNow = nullptr;
            continue;
        }
        if (shaderNow != 0) { useTexShader(); shaderNow = 0; texNow = nullptr; filterNow = -1; }
        TexWiiU* t = b.tex ? b.tex : s_white;
        if (t != texNow) { GX2SetPixelTexture(&t->tex, s_texSamplerLoc); texNow = t; }
        if (b.filter != filterNow) {
            GX2SetPixelSampler(b.filter == GX_NEAR ? &s_sampNear : &s_sampLinear, s_texSamplerLoc);
            filterNow = b.filter;
        }
        if (b.kind == K_POST_SUB) {
            /* dst - src: the video's black (16) to 0 */
            GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ONE,
                               GX2_BLEND_COMBINE_MODE_REV_SUB, TRUE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE,
                               GX2_BLEND_COMBINE_MODE_ADD);
        } else if (b.kind == K_POST_MUL) {
            /* dst + src * dst: its white (235) to 255 */
            GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_DST_COLOR, GX2_BLEND_MODE_ONE,
                               GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE,
                               GX2_BLEND_COMBINE_MODE_ADD);
        }
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, b.count, b.first, 1);
        if (b.kind == K_POST_SUB || b.kind == K_POST_MUL) setCommonState();
    }
}

/* the main frame's text: rasterised for the screen it is looked at on */
float s_mainSX = 1.0f, s_mainSY = 1.0f;
void mainMappingFor(int w, int h)
{
    s_mainSX = w / (640.0f * 4.0f / 3.0f);
    s_mainSY = h / 480.0f;
    float l, sx, sy;
    Text::frameMapping(l, sx, sy);
    Text::setFrameMapping(l, s_mainSX, s_mainSY);
}

void mainScreenSize(int& w, int& h)
{
    GX2ColorBuffer* cb = s_mode == GfxWiiU::Mode::Remote ? WHBGfxGetTVColourBuffer() : WHBGfxGetDRCColourBuffer();
    w = (int)cb->surface.width;
    h = (int)cb->surface.height;
}

/* ProcUI: leaving for the HOME menu, coming back */
uint32_t onRelease(void*)
{
    if (s_fgHook) s_fgHook(true);
    return 0;
}
uint32_t onAcquire(void*)
{
    if (s_fgHook) s_fgHook(false);
    return 0;
}

/* Moonlight's NV12 shader with BT.601 or BT.709's coefficients, for a
 * picture whose levels are then stretched from 16-235 (x 1.164, see
 * drawNV12): its four literals (little-endian floats in the program) */
bool loadNV12(WHBGfxShaderGroup& g, bool bt709)
{
    std::vector<u8> bin(nv12_gsh, nv12_gsh + sizeof(nv12_gsh));
    /* R = Y + a V', G = Y - c V' - b U', B = Y + d U' (U', V' = x - 0.5) */
    const float orig[4] = { 1.13983f, 0.58060f, 2.03211f, 0.39465f };
    const float k601[4] = { 1.596f / 1.164f, 0.813f / 1.164f, 2.017f / 1.164f, 0.392f / 1.164f };
    const float k709[4] = { 1.793f / 1.164f, 0.533f / 1.164f, 2.112f / 1.164f, 0.213f / 1.164f };
    const float* k = bt709 ? k709 : k601;
    int patched = 0;
    for (int i = 0; i < 4; ++i) {
        u8 le[4];
        u32 bits;
        memcpy(&bits, &orig[i], 4);
        le[0] = bits & 0xFF; le[1] = (bits >> 8) & 0xFF; le[2] = (bits >> 16) & 0xFF; le[3] = bits >> 24;
        for (size_t p = 0; p + 4 <= bin.size(); ++p) {
            if (memcmp(&bin[p], le, 4) != 0) continue;
            u32 nb;
            memcpy(&nb, &k[i], 4);
            bin[p] = nb & 0xFF; bin[p + 1] = (nb >> 8) & 0xFF; bin[p + 2] = (nb >> 16) & 0xFF; bin[p + 3] = nb >> 24;
            ++patched;
            break;
        }
    }
    if (patched != 4) SYS_Report("[GfxWiiU] NV12 shader: %d of 4 coefficients patched\n", patched);
    if (!WHBGfxLoadGFDShaderGroup(&g, 0, bin.data())) return false;
    WHBGfxInitShaderAttribute(&g, "in_pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&g, "in_texCoord", 0, 24, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    return WHBGfxInitFetchShader(&g);
}

} // namespace

/* ---- GfxWiiU -------------------------------------------------------------- */

namespace GfxWiiU {

void init()
{
    if (s_up) return;
    WHBProcInit();
    WHBGfxInit();
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, onRelease, nullptr, 50);
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, onAcquire, nullptr, 50);

    if (!WHBGfxLoadGFDShaderGroup(&s_texShader, 0, texture_gsh))
        SYS_Report("[GfxWiiU] texture shader not loaded\n");
    WHBGfxInitShaderAttribute(&s_texShader, "a_position", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&s_texShader, "a_color", 0, 8, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitShaderAttribute(&s_texShader, "a_texcoord", 0, 24, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitFetchShader(&s_texShader);
    s_texProjLoc = s_texShader.vertexShader->uniformVars[0].offset;
    s_texSamplerLoc = s_texShader.pixelShader->samplerVars[0].location;
    if (!loadNV12(s_nv12Shader[0], false) || !loadNV12(s_nv12Shader[1], true))
        SYS_Report("[GfxWiiU] NV12 shader not loaded\n");
    s_nv12SizeLoc = s_nv12Shader[0].vertexShader->uniformVars[0].offset;

    GX2InitSampler(&s_sampLinear, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    GX2InitSampler(&s_sampNear, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);

    s_vbuf = (Vtx*)memalign(GX2_VERTEX_BUFFER_ALIGNMENT, sizeof(Vtx) * MAX_VERTS);
    const u32 white = 0xFFFFFFFF;
    s_white = make(Fmt::RGBA8, 1, 1, &white, 4);

    orthoProj(s_proj, 0, 480, 0, 640);
    identityMv(s_mv);
    identityMv(GXmodelView2D);
    s_up = true;
    {
        int w, h;
        mainScreenSize(w, h);
        GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
        SYS_Report("[GfxWiiU] TV %ux%u, GamePad %dx%d\n", (unsigned)tv->surface.width,
                   (unsigned)tv->surface.height, 854, 480);
        s_mainSX = w / (640.0f * 4.0f / 3.0f);
        s_mainSY = h / 480.0f;
    }
    beginFrame();
}

void exit()
{
    if (!s_up) return;
    s_up = false;
    if (!s_exiting) {
        /* leaving by ourselves: back to where we were started from (the
         * HOME menu's own exit has already been asked) */
        WHBProcStopRunning();
    }
    WHBGfxFreeShaderGroup(&s_texShader);
    WHBGfxFreeShaderGroup(&s_nv12Shader[0]);
    WHBGfxFreeShaderGroup(&s_nv12Shader[1]);
    WHBGfxShutdown();
    WHBProcShutdown();
}

TexWiiU* make(Fmt fmt, int w, int h, const void* pixels, int stride)
{
    if (w <= 0 || h <= 0) return nullptr;
    int shift = 0;
    while ((w >> shift) > 2048 || (h >> shift) > 2048) ++shift;
    TexWiiU* t = (TexWiiU*)calloc(1, sizeof(TexWiiU));
    if (!t) return nullptr;
    t->magic = TEX_MAGIC;
    t->w = (u16)(w >> shift);
    t->h = (u16)(h >> shift);
    t->fmt = (u8)fmt;
    t->bpp = (u8)bppOf(fmt);
    GX2Surface& s = t->tex.surface;
    s.width = t->w;
    s.height = t->h;
    s.depth = 1;
    s.mipLevels = 1;
    s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    s.format = gx2Fmt(fmt);
    s.use = GX2_SURFACE_USE_TEXTURE;
    s.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    t->tex.viewNumMips = 1;
    t->tex.viewNumSlices = 1;
    t->tex.compMap = compMap(fmt);
    GX2CalcSurfaceSizeAndAlignment(&s);
    GX2InitTextureRegs(&t->tex);
    s.image = memalign(s.alignment, s.imageSize);
    if (!s.image) { free(t); return nullptr; }
    t->freeImage = true;
    if (pixels) fill(t, (const u8*)pixels, stride, shift);
    else {
        memset(s.image, 0, s.imageSize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, s.image, s.imageSize);
    }
    return t;
}

TexWiiU* wrapMemory(Fmt fmt, int w, int h, void* image, int pitchBytes, bool ownsImage)
{
    if (w <= 0 || h <= 0 || !image) return nullptr;
    TexWiiU* t = (TexWiiU*)calloc(1, sizeof(TexWiiU));
    if (!t) return nullptr;
    t->magic = TEX_MAGIC;
    t->w = (u16)w;
    t->h = (u16)h;
    t->fmt = (u8)fmt;
    t->bpp = (u8)bppOf(fmt);
    GX2Surface& s = t->tex.surface;
    s.width = w;
    s.height = h;
    s.depth = 1;
    s.mipLevels = 1;
    s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    s.format = gx2Fmt(fmt);
    s.use = GX2_SURFACE_USE_TEXTURE;
    s.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    t->tex.viewNumMips = 1;
    t->tex.viewNumSlices = 1;
    t->tex.compMap = compMap(fmt);
    GX2CalcSurfaceSizeAndAlignment(&s);
    if ((int)s.pitch * t->bpp != pitchBytes) {
        static bool told = false;
        if (!told) {
            told = true;
            SYS_Report("[GfxWiiU] memory texture: pitch %d bytes, GX2's %u\n", pitchBytes, (unsigned)(s.pitch * t->bpp));
        }
        s.pitch = pitchBytes / t->bpp;
    }
    GX2InitTextureRegs(&t->tex);
    s.image = image;
    t->freeImage = ownsImage;
    return t;
}

void update(TexWiiU* t, const void* px, int stride)
{
    if (t && px) fill(t, (const u8*)px, stride, 0);
}

void* pixels(TexWiiU* t) { return t ? t->tex.surface.image : nullptr; }
int   pitch(const TexWiiU* t) { return t ? (int)t->tex.surface.pitch : 0; }
void  flush(TexWiiU* t)
{
    if (t) GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, t->tex.surface.image, t->tex.surface.imageSize);
}

void destroy(TexWiiU* t)
{
    if (!t || t->magic != TEX_MAGIC) return;
    t->magic = 0;
    std::lock_guard<std::mutex> g(s_texMutex);
    s_toFree.push_back(t);
}

int width(const TexWiiU* t)  { return t ? t->w : 0; }
int height(const TexWiiU* t) { return t ? t->h : 0; }

GRRLIB_texImg* wrap(TexWiiU* t)
{
    if (!t) return nullptr;
    GRRLIB_texImg* g = (GRRLIB_texImg*)calloc(1, sizeof(GRRLIB_texImg));
    if (!g) { destroy(t); return nullptr; }
    g->w = t->w;
    g->h = t->h;
    g->format = GX_TF_WIIU;
    g->data = t;
    GRRLIB_SetHandle(g, 0, 0);
    return g;
}

void setMode(Mode m)
{
    if (m == s_mode) return;
    s_mode = m;
    int w, h;
    mainScreenSize(w, h);
    mainMappingFor(w, h);
    SYS_Report("[GfxWiiU] screens: %s\n", m == Mode::Remote ? "TV + GamePad remote" : "GamePad (and the TV the same)");
}
Mode mode() { return s_mode; }
void setRemoteDrawer(void (*draw)()) { s_remoteDrawer = draw; }
float mainScale() { return s_mainSX; }
float mainScaleY() { return s_mainSY; }
void setCursorTexture(GRRLIB_texImg* tex) { s_cursorTex = tex; }
void hideCursor(bool hide) { s_hideCursor = hide; }
void tvSize(int& w, int& h)
{
    GX2ColorBuffer* cb = WHBGfxGetTVColourBuffer();
    w = (int)cb->surface.width;
    h = (int)cb->surface.height;
}
unsigned frames() { return s_frames; }
void setForegroundHook(void (*hook)(bool)) { s_fgHook = hook; }

void drawNV12(TexWiiU* y, TexWiiU* uv, float x, float y0, float w, float h, bool bt709)
{
    if (!y || !uv) return;
    beginFrame();
    if (s_matDirty) applyMatrix();
    if (s_vcount + 18 > MAX_VERTS) return;
    auto at = [](float px, float py, float u, float v) {
        Vtx o;
        o.x = s_A[0][0] * px + s_A[0][1] * py + s_A[0][2];
        o.y = s_A[1][0] * px + s_A[1][1] * py + s_A[1][2];
        o.r = o.g = o.b = o.a = 1.0f;
        o.u = u; o.v = v;
        return o;
    };
    const Vtx q[4] = { at(x, y0, 0, 0), at(x + w, y0, 1, 0), at(x + w, y0 + h, 1, 1), at(x, y0 + h, 0, 1) };
    const int order[6] = { 0, 1, 2, 0, 2, 3 };
    /* the picture */
    Batch& b = batchFor(K_NV12, y, uv, GX_LINEAR, bt709);
    for (int i : order) s_vbuf[s_vcount++] = q[i];
    b.count += 6;
    /* video range to full: - 16/255, then x 255/219 (dst + 0.164 dst) */
    const float sub = 16.0f / 255.0f, mul = 255.0f / 219.0f - 1.0f;
    for (int pass = 0; pass < 2; ++pass) {
        Batch& p = batchFor(pass == 0 ? K_POST_SUB : K_POST_MUL, nullptr, nullptr, GX_LINEAR, false);
        const float c = pass == 0 ? sub : mul;
        for (int i : order) {
            Vtx v = q[i];
            v.r = v.g = v.b = c; v.a = 1.0f;
            s_vbuf[s_vcount++] = v;
        }
        p.count += 6;
    }
}

} // namespace GfxWiiU

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
    if (((TexWiiU*)img)->magic == TEX_MAGIC) { obj->tex = img; return; }
    GfxWiiU::Fmt f;
    std::vector<u8> rows = gxToRows((const u8*)img, w, h, fmt, f);
    if (rows.empty()) return;
    obj->tex = GfxWiiU::make(f, w, h, rows.data(), w * bppOf(f));
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
    s_curTex = (TexWiiU*)obj->tex;
    s_curFilter = obj->filter;
}

void GX_DestroyTexObj(GXTexObj* obj)
{
    if (obj->owned && obj->tex) GfxWiiU::destroy((TexWiiU*)obj->tex);
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
    if (s_primN < MAX_PRIM) {
        s_cur.x = s_curX; s_cur.y = s_curY;
        s_primV[s_primN++] = s_cur;
    }
}

static bool texturing() { return s_texOn && s_tevMode != GX_PASSCLR && s_curTex; }

void GX_Position3f32(f32 x, f32 y, f32 z) { (void)z; s_curX = x; s_curY = y; }

void GX_Color1u32(u32 c)
{
    colorOf(c, s_cur);
    if (!s_texOn) emit();
}

void GX_TexCoord2f32(f32 s, f32 t)
{
    s_cur.u = s; s_cur.v = t;
    if (s_texOn) emit();
}

void GX_End(void)
{
    const int n = s_primN;
    const int tris = s_prim == GX_QUADS ? n / 4 * 2 : s_prim == GX_TRIANGLES ? n / 3 : n >= 3 ? n - 2 : 0;
    if (tris <= 0) return;
    if (s_vcount + tris * 3 > MAX_VERTS) {
        if (!s_overflowLogged) { s_overflowLogged = true; SYS_Report("[GfxWiiU] vertex buffer full\n"); }
        return;
    }
    if (s_matDirty) applyMatrix();
    const bool tex = texturing();
    Vtx* v = s_primV;
    for (int i = 0; i < n; ++i) {
        const float x = v[i].x, y = v[i].y;
        v[i].x = s_A[0][0] * x + s_A[0][1] * y + s_A[0][2];
        v[i].y = s_A[1][0] * x + s_A[1][1] * y + s_A[1][2];
        if (!tex) { v[i].u = 0.5f; v[i].v = 0.5f; }
    }
    Batch& b = batchFor(K_TEX, tex ? s_curTex : nullptr, nullptr, tex ? s_curFilter : GX_LINEAR, false);
    Vtx* out = s_vbuf + s_vcount;
    int k = 0;
    switch (s_prim) {
    case GX_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            out[k++] = v[i]; out[k++] = v[i + 1]; out[k++] = v[i + 2];
            out[k++] = v[i]; out[k++] = v[i + 2]; out[k++] = v[i + 3];
        }
        break;
    case GX_TRIANGLEFAN:
        for (int i = 1; i + 1 < n; ++i) { out[k++] = v[0]; out[k++] = v[i]; out[k++] = v[i + 1]; }
        break;
    case GX_TRIANGLESTRIP:
        for (int i = 0; i + 2 < n; ++i) { out[k++] = v[i]; out[k++] = v[i + 1]; out[k++] = v[i + 2]; }
        break;
    default:
        for (int i = 0; i + 2 < n; i += 3) { out[k++] = v[i]; out[k++] = v[i + 1]; out[k++] = v[i + 2]; }
        break;
    }
    s_vcount += k;
    b.count += k;
}

/* ---- GRRLIB -------------------------------------------------------------- */

static FT_Library s_ft = nullptr;

int GRRLIB_Init(void)
{
    GfxWiiU::init();
    if (!s_ft) FT_Init_FreeType(&s_ft);
    return 0;
}

void GRRLIB_Exit(void)
{
    if (s_ft) { FT_Done_FreeType(s_ft); s_ft = nullptr; }
    GfxWiiU::exit();
}

static void recordRemote()
{
    if (s_mode != GfxWiiU::Mode::Remote || !s_remoteDrawer) return;
    /* the GamePad: its own 854x480 space, glyphs 1:1 */
    float proj[4][4], mv[3][4], vp[4], gmv[3][4];
    int clip[4];
    memcpy(proj, s_proj, sizeof(proj)); memcpy(mv, s_mv, sizeof(mv));
    memcpy(vp, s_vp, sizeof(vp)); memcpy(clip, s_clip, sizeof(clip));
    memcpy(gmv, GXmodelView2D, sizeof(gmv));
    float l, sx, sy;
    Text::frameMapping(l, sx, sy);
    s_list = &s_remote;
    orthoProj(s_proj, 0, 480, 0, 854);
    identityMv(s_mv);
    identityMv(GXmodelView2D);
    s_vp[0] = 0; s_vp[1] = 0; s_vp[2] = 640; s_vp[3] = 480;
    s_clip[2] = -1;
    s_matDirty = true;
    Text::setFrameMapping(0.0f, 1.0f, 1.0f);
    s_remoteDrawer();
    Text::setFrameMapping(l, sx, sy);
    memcpy(s_proj, proj, sizeof(proj)); memcpy(s_mv, mv, sizeof(mv));
    memcpy(s_vp, vp, sizeof(vp)); memcpy(s_clip, clip, sizeof(clip));
    memcpy(GXmodelView2D, gmv, sizeof(gmv));
    s_list = &s_main;
    s_matDirty = true;
}

void GRRLIB_Render(void)
{
    beginFrame();
    if (s_exiting) {
        /* closed from the HOME menu: WiiFin unwinds, nothing is drawn */
        s_inFrame = false;
        freePending();
        ++s_frames;
        return;
    }
    recordRemote();
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, s_vbuf, s_vcount * sizeof(Vtx));

    WHBGfxBeginRender();
    {
        GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        setCommonState();
        drawList(s_main, (int)tv->surface.width, (int)tv->surface.height);
        WHBGfxFinishRenderTV();
    }
    {
        GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        setCommonState();
        drawList(s_mode == GfxWiiU::Mode::Remote ? s_remote : s_main,
                 (int)drc->surface.width, (int)drc->surface.height);
        WHBGfxFinishRenderDRC();
    }
    WHBGfxFinishRender();
    s_inFrame = false;
    ++s_frames;
    freePending();
    /* the system: the HOME menu (waits there), the app closed */
    if (!WHBProcIsRunning()) {
        s_exiting = true;
        g_app_reset = true;
    }
    beginFrame();
}

void GRRLIB_FillScreen(u32 color)
{
    GRRLIB_Rectangle(-300, -60, 1500, 600, color, true);
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
}

void GRRLIB_ClipReset(void)
{
    s_clip[0] = s_clip[1] = 0;
    s_clip[2] = s_clip[3] = -1;
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
    if (tex == s_cursorTex && s_hideCursor) return;   /* a finger points: no arrow under it */
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
    if (obj.owned) GX_DestroyTexObj(&obj);
}

void GRRLIB_FreeTexture(GRRLIB_texImg* tex)
{
    if (!tex) return;
    if (tex->data && ((TexWiiU*)tex->data)->magic == TEX_MAGIC) GfxWiiU::destroy((TexWiiU*)tex->data);
    else free(tex->data);
    free(tex);
}

/* A texture made on the CPU in a GX format becomes a GPU one */
void GRRLIB_FlushTex(GRRLIB_texImg* tex)
{
    if (!tex || !tex->data || ((TexWiiU*)tex->data)->magic == TEX_MAGIC) return;
    GXTexObj obj;
    GX_InitTexObj(&obj, tex->data, tex->w, tex->h, tex->format, GX_CLAMP, GX_CLAMP, GX_FALSE);
    if (!obj.tex) return;
    free(tex->data);
    tex->data = obj.tex;
    tex->format = GX_TF_WIIU;
}

GRRLIB_texImg* GRRLIB_CreateEmptyTexture(u32 w, u32 h)
{
    return GfxWiiU::wrap(GfxWiiU::make(GfxWiiU::Fmt::RGBA8, (int)w, (int)h, nullptr, 0));
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
    return GfxWiiU::wrap(GfxWiiU::make(GfxWiiU::Fmt::RGBA8, img.width, img.height, px.data(), img.width * 4));
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
    while ((cinfo.image_width / cinfo.scale_denom > 2048 || cinfo.image_height / cinfo.scale_denom > 2048) &&
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
    return GfxWiiU::wrap(GfxWiiU::make(GfxWiiU::Fmt::RGBA8, w, h, px.data(), w * 4));
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
