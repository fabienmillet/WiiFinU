/* -----------------------------------------------------------------------
 * Platform3ds — the 3DS's start, and what its top screen shows.
 *
 * The menus are on the bottom (touch) screen; the top one shows the video
 * while one plays, else WiiFin's logo on the theme's background, the
 * clock and what music plays.
 * ----------------------------------------------------------------------- */
#include "Platform3ds.h"
#include "Gfx3ds.h"
#include "Player3ds.h"
#include "../../ui/Ui.h"
#include "../../core/Lang.h"

#include <3ds.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

extern unsigned char data_logo_wiifin_png[];

namespace {

GRRLIB_texImg* s_logo = nullptr;
std::string    s_caption;

void quad(float x, float y, float w, float h, u32 top, u32 bottom)
{
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, 0);         GX_Color1u32(top);
    GX_Position3f32(x + w, y, 0);     GX_Color1u32(top);
    GX_Position3f32(x + w, y + h, 0); GX_Color1u32(bottom);
    GX_Position3f32(x, y + h, 0);     GX_Color1u32(bottom);
    GX_End();
}

void drawTop()
{
    if (Player3ds::drawTop()) return;
    const Ui::Palette& p = Ui::pal();
    quad(0, 0, 800, 480, p.bgTop, p.bgBottom);
    if (p.stripe & 0xFF)
        for (int y = 0; y < 480; y += 4) quad(0, (float)y, 800, 2, p.stripe, p.stripe);
    if (!s_logo) s_logo = GRRLIB_LoadTexture(data_logo_wiifin_png);
    if (s_logo) {
        const float w = 460, h = w * s_logo->h / s_logo->w;
        GRRLIB_DrawImg((800 - w) * 0.5f, 150 - h * 0.5f, s_logo, 0, w / s_logo->w, h / s_logo->h, 0xFFFFFFFF);
    }
    /* clock */
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char clock[16];
    if (Ui::clock12h())
        snprintf(clock, sizeof(clock), "%d:%02d %s", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min,
                 lt.tm_hour < 12 ? "AM" : "PM");
    else
        snprintf(clock, sizeof(clock), "%02d:%02d", lt.tm_hour, lt.tm_min);
    Ui::textCentered(400, 290, clock, 64, p.text);
    if (!s_caption.empty()) Ui::textCentered(400, 400, s_caption.c_str(), 28, p.textDim);
}

} // namespace

namespace Platform3ds {

void init()
{
    /* the New 3DS: its 804 MHz and L2 cache */
    osSetSpeedupEnable(true);
    /* the second core, 30% of it, for the threads that ask for it */
    APT_SetAppCpuTimeLimit(30);
    Gfx3ds::init();
    Gfx3ds::setTopDrawer(drawTop);
}

void setTopCaption(const std::string& s) { s_caption = s; }

bool isNew3DS()
{
    bool n = false;
    APT_CheckNew3DS(&n);
    return n;
}

}
