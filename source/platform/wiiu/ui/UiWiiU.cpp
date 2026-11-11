#include "UiWiiU.h"
#include "../GfxWiiU.h"
#include "../InputWiiU.h"
#include "../../../ui/Ui.h"
#include "../../../core/Text.h"

#include <math.h>
#include <string.h>
#include <time.h>
#include <ogc/lwp_watchdog.h>

extern unsigned char data_logo_wiifin_png[];

namespace UiWiiU {

/* ---- spaces -------------------------------------------------------------- */

static float s_spaceW = PW, s_spaceH = PH;

static void space(float w, float h, float glyphScale)
{
    Mtx44 m;
    guOrtho(m, 0, h, 0, w, 0, 1000);
    GX_LoadProjectionMtx(m, GX_ORTHOGRAPHIC);
    GX_SetViewport(0, 0, 640, 480, 0, 1);
    guMtxIdentity(GXmodelView2D);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    Text::setFrameMapping(0.0f, glyphScale, glyphScale);
    GRRLIB_ClipReset();
    s_spaceW = w;
    s_spaceH = h;
}

void padSpace() { space(PW, PH, 1.0f); }

void tvSpace()
{
    int w = 1280, h = 720;
    GfxWiiU::tvSize(w, h);
    space(TW, TH, w / (float)TW);
}

void wiiSpace()
{
    int l, t, r, b;
    Ui::safeArea(l, t, r, b);
    Ui::initScreen(true);
    Ui::setSafeArea(l, t, r, b);
    guMtxIdentity(GXmodelView2D);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GRRLIB_ClipReset();
}

void clip(float x, float y, float w, float h)
{
    /* units -> GX's 640x480 frame pixels */
    const float kx = 640.0f / s_spaceW, ky = 480.0f / s_spaceH;
    const float x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const float x1 = x + w, y1 = y + h;
    GRRLIB_ClipDrawing((int)floorf(x0 * kx), (int)floorf(y0 * ky),
                       (int)ceilf((x1 - x0) * kx), (int)ceilf((y1 - y0) * ky));
}

void clipOff() { GRRLIB_ClipReset(); }

/* ---- touch ---------------------------------------------------------------- */

void readTouch(Touch& t)
{
    float x, y;
    bool tap = false, dragged = false;
    const bool down = InputWiiU::touch(x, y, tap, dragged);
    t.pressed = down && !t.down;
    if (t.pressed) { t.startX = x; t.startY = y; t.dx = t.dy = 0; }
    else if (down) { t.dx = x - t.x; t.dy = y - t.y; }
    else           { t.dx = t.dy = 0; }
    t.down = down;
    t.dragging = down && dragged;
    t.tap = tap;
    if (down || tap) { t.x = x; t.y = y; }
}

/* ---- drawing -------------------------------------------------------------- */

static void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                 u32 c0, u32 c1, u32 c2, u32 c3)
{
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x0, y0, 0); GX_Color1u32(c0);
    GX_Position3f32(x1, y1, 0); GX_Color1u32(c1);
    GX_Position3f32(x2, y2, 0); GX_Color1u32(c2);
    GX_Position3f32(x3, y3, 0); GX_Color1u32(c3);
    GX_End();
}

void rect(float x, float y, float w, float h, u32 c) { gradient(x, y, w, h, c, c); }

void gradient(float x, float y, float w, float h, u32 top, u32 bottom)
{
    if (w <= 0 || h <= 0) return;
    quad(x, y, x + w, y, x + w, y + h, x, y + h, top, top, bottom, bottom);
}

void hgradient(float x, float y, float w, float h, u32 left, u32 right)
{
    if (w <= 0 || h <= 0) return;
    quad(x, y, x + w, y, x + w, y + h, x, y + h, left, right, right, left);
}

void background(float w, float h)
{
    const Ui::Palette& p = Ui::pal();
    gradient(0, 0, w, h, p.bgTop, p.bgBottom);
    /* the Wii U Menu's soft diagonal bands (WiiFin's pinstripes' colour) */
    const u32 band = (p.stripe & 0xFFFFFF00u) | ((p.stripe & 0xFF) * 3 / 4);
    if (!(band & 0xFF)) return;
    const float step = 36, bw = 12;
    for (float x = -h; x < w; x += step)
        quad(x, h, x + h, 0, x + h + bw, 0, x + bw, h, band, band, band, band);
}

void image(GRRLIB_texImg* tex, float x, float y, float w, float h, float r, u32 tint)
{
    if (!tex || !tex->w || !tex->h) return;
    Ui::texCover(tex, x, y, w, h, r, w / h, tint);
}

void tileFrame(float x, float y, float w, float h, float r, float focus)
{
    const Ui::Palette& p = Ui::pal();
    Ui::shadow(x + 1, y + 4, w - 2, h - 2, r, 7.0f, p.shadow);
    if (focus > 0.01f) Ui::shadow(x - 3, y - 3, w + 6, h + 6, r + 3, 10.0f, Ui::alpha(p.glow, focus));
    /* the Wii U's white frame around a tile (the card's colour in the dark theme) */
    const u32 frame = Ui::mix(Ui::mix(p.cardTop, 0xFFFFFFFF, 0.55f), p.accent, focus);
    Ui::roundBorder(x - 3, y - 3, w + 6, h + 6, r + 3, 3.0f + focus, frame);
}

std::string ellipsize(const std::string& s, int size, float maxW)
{
    if (maxW <= 0 || Ui::textWidth(s.c_str(), size) <= maxW) return s;
    static const char* ELL = "\xE2\x80\xA6";
    const float ew = Ui::textWidth(ELL, size);
    size_t n = Text::fitBytes(Ui::font(), s.c_str(), size, maxW - ew);
    std::string out = s.substr(0, n);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + ELL;
}

float text(float x, float y, const std::string& s, int size, u32 color, float maxW)
{
    const std::string t = ellipsize(s, size, maxW);
    Ui::text(x, y, t.c_str(), size, color);
    return Ui::textWidth(t.c_str(), size);
}

float textS(float x, float y, const std::string& s, int size, u32 color, float maxW)
{
    const std::string t = ellipsize(s, size, maxW);
    const float o = size > 30 ? 2.0f : 1.0f;
    Ui::text(x + o, y + o, t.c_str(), size, 0x00000090);
    Ui::text(x, y, t.c_str(), size, color);
    return Ui::textWidth(t.c_str(), size);
}

float textW(const std::string& s, int size) { return Ui::textWidth(s.c_str(), size); }

void textC(float cx, float y, const std::string& s, int size, u32 color, float maxW)
{
    const std::string t = ellipsize(s, size, maxW);
    Ui::textCentered(cx, y, t.c_str(), size, color);
}

void textR(float xr, float y, const std::string& s, int size, u32 color)
{
    Ui::textRight(xr, y, s.c_str(), size, color);
}

float paragraph(float x, float y, float maxW, const std::string& s, int size, u32 color,
                int maxLines, float lineH)
{
    if (lineH <= 0) lineH = size * 1.3f;
    if (maxLines <= 0) return 0;
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    bool more = false;
    while (i <= s.size()) {
        size_t e = s.find_first_of(" \n", i);
        if (e == std::string::npos) e = s.size();
        std::string word = s.substr(i, e - i);
        const bool newline = e < s.size() && s[e] == '\n';
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (!cur.empty() && textW(trial, size) > maxW) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = trial;
        }
        if (newline && !cur.empty()) { lines.push_back(cur); cur.clear(); }
        if ((int)lines.size() >= maxLines) { more = true; break; }
        i = e + 1;
    }
    if (!cur.empty() && (int)lines.size() < maxLines) lines.push_back(cur);
    else if (!cur.empty()) more = true;
    if ((int)lines.size() > maxLines) lines.resize(maxLines);
    for (size_t k = 0; k < lines.size(); ++k) {
        std::string l = lines[k];
        if (more && k + 1 == lines.size()) l = ellipsize(l + " \xE2\x80\xA6\xE2\x80\xA6", size, maxW);
        Ui::text(x, y + k * lineH, l.c_str(), size, color);
    }
    return lines.size() * lineH;
}

/* ---- icons ----------------------------------------------------------------- */

static void bar(float x0, float y0, float x1, float y1, float w, u32 c)
{
    const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return;
    const float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    Ui::triangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    Ui::triangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
    Ui::circle(x0, y0, w * 0.5f, c);
    Ui::circle(x1, y1, w * 0.5f, c);
}

void icon(Icon i, float cx, float cy, float s, u32 c)
{
    const float h = s * 0.5f, lw = s * 0.1f < 1.8f ? 1.8f : s * 0.1f;
    switch (i) {
    case Icon::Back:
        bar(cx + h * 0.35f, cy - h * 0.7f, cx - h * 0.35f, cy, lw, c);
        bar(cx - h * 0.35f, cy, cx + h * 0.35f, cy + h * 0.7f, lw, c);
        break;
    case Icon::Search:
        Ui::roundBorder(cx - h * 0.75f, cy - h * 0.75f, h * 1.15f, h * 1.15f, h * 0.575f, lw, c);
        bar(cx + h * 0.25f, cy + h * 0.25f, cx + h * 0.75f, cy + h * 0.75f, lw + 0.6f, c);
        break;
    case Icon::Gear:
        for (int k = 0; k < 4; ++k) {
            const float a = k * (float)M_PI / 4;
            bar(cx - cosf(a) * h * 0.8f, cy - sinf(a) * h * 0.8f, cx + cosf(a) * h * 0.8f, cy + sinf(a) * h * 0.8f, lw * 1.6f, c);
        }
        Ui::circle(cx, cy, h * 0.55f, c);
        Ui::circle(cx, cy, h * 0.25f, Ui::pal().cardTop);
        break;
    case Icon::Play:
        Ui::triangle(cx - h * 0.45f, cy - h * 0.6f, cx + h * 0.6f, cy, cx - h * 0.45f, cy + h * 0.6f, c);
        break;
    case Icon::Pause:
        Ui::roundRect(cx - h * 0.5f, cy - h * 0.6f, h * 0.35f, h * 1.2f, 1, c);
        Ui::roundRect(cx + h * 0.15f, cy - h * 0.6f, h * 0.35f, h * 1.2f, 1, c);
        break;
    case Icon::Prev:
        Ui::roundRect(cx - h * 0.6f, cy - h * 0.55f, h * 0.2f, h * 1.1f, 1, c);
        Ui::triangle(cx + h * 0.5f, cy - h * 0.55f, cx + h * 0.5f, cy + h * 0.55f, cx - h * 0.35f, cy, c);
        break;
    case Icon::Next:
        Ui::roundRect(cx + h * 0.4f, cy - h * 0.55f, h * 0.2f, h * 1.1f, 1, c);
        Ui::triangle(cx - h * 0.5f, cy - h * 0.55f, cx - h * 0.5f, cy + h * 0.55f, cx + h * 0.35f, cy, c);
        break;
    case Icon::Rewind:
        Ui::triangle(cx, cy - h * 0.5f, cx, cy + h * 0.5f, cx - h * 0.7f, cy, c);
        Ui::triangle(cx + h * 0.7f, cy - h * 0.5f, cx + h * 0.7f, cy + h * 0.5f, cx, cy, c);
        break;
    case Icon::Forward:
        Ui::triangle(cx, cy - h * 0.5f, cx, cy + h * 0.5f, cx + h * 0.7f, cy, c);
        Ui::triangle(cx - h * 0.7f, cy - h * 0.5f, cx - h * 0.7f, cy + h * 0.5f, cx, cy, c);
        break;
    case Icon::Heart:
        Ui::circle(cx - h * 0.3f, cy - h * 0.2f, h * 0.36f, c);
        Ui::circle(cx + h * 0.3f, cy - h * 0.2f, h * 0.36f, c);
        Ui::triangle(cx - h * 0.64f, cy - h * 0.1f, cx + h * 0.64f, cy - h * 0.1f, cx, cy + h * 0.65f, c);
        break;
    case Icon::Check:
        bar(cx - h * 0.55f, cy, cx - h * 0.15f, cy + h * 0.45f, lw + 0.4f, c);
        bar(cx - h * 0.15f, cy + h * 0.45f, cx + h * 0.6f, cy - h * 0.5f, lw + 0.4f, c);
        break;
    case Icon::Home:
        Ui::triangle(cx - h * 0.75f, cy - h * 0.05f, cx, cy - h * 0.75f, cx + h * 0.75f, cy - h * 0.05f, c);
        Ui::roundRect(cx - h * 0.5f, cy - h * 0.1f, h, h * 0.75f, 1, c);
        break;
    case Icon::Person:
        Ui::circle(cx, cy - h * 0.3f, h * 0.32f, c);
        Ui::roundRect(cx - h * 0.6f, cy + h * 0.1f, h * 1.2f, h * 0.55f, h * 0.27f, c);
        break;
    case Icon::Audio:
        Ui::roundRect(cx - h * 0.6f, cy - h * 0.2f, h * 0.35f, h * 0.4f, 1, c);
        Ui::triangle(cx - h * 0.3f, cy - h * 0.2f, cx + h * 0.1f, cy - h * 0.6f, cx + h * 0.1f, cy + h * 0.6f, c);
        Ui::triangle(cx - h * 0.3f, cy - h * 0.2f, cx + h * 0.1f, cy + h * 0.6f, cx - h * 0.3f, cy + h * 0.2f, c);
        bar(cx + h * 0.35f, cy - h * 0.3f, cx + h * 0.35f, cy + h * 0.3f, lw, c);
        break;
    case Icon::Subtitles:
        Ui::roundBorder(cx - h * 0.75f, cy - h * 0.5f, h * 1.5f, h, 2, lw, c);
        bar(cx - h * 0.45f, cy + h * 0.15f, cx + h * 0.05f, cy + h * 0.15f, lw, c);
        bar(cx + h * 0.2f, cy + h * 0.15f, cx + h * 0.45f, cy + h * 0.15f, lw, c);
        break;
    case Icon::Shuffle:
        bar(cx - h * 0.6f, cy - h * 0.4f, cx + h * 0.5f, cy + h * 0.4f, lw, c);
        bar(cx - h * 0.6f, cy + h * 0.4f, cx + h * 0.5f, cy - h * 0.4f, lw, c);
        break;
    case Icon::Grid:
        for (int yy = 0; yy < 2; ++yy)
            for (int xx = 0; xx < 2; ++xx)
                Ui::roundRect(cx - h * 0.6f + xx * h * 0.7f, cy - h * 0.6f + yy * h * 0.7f, h * 0.5f, h * 0.5f, 1, c);
        break;
    case Icon::Close:
        bar(cx - h * 0.5f, cy - h * 0.5f, cx + h * 0.5f, cy + h * 0.5f, lw, c);
        bar(cx - h * 0.5f, cy + h * 0.5f, cx + h * 0.5f, cy - h * 0.5f, lw, c);
        break;
    case Icon::Star: {
        /* five points */
        float px[10], py[10];
        for (int k = 0; k < 10; ++k) {
            const float a = -(float)M_PI / 2 + k * (float)M_PI / 5, r = k % 2 ? h * 0.42f : h;
            px[k] = cx + cosf(a) * r; py[k] = cy + sinf(a) * r;
        }
        for (int k = 0; k < 10; ++k)
            Ui::triangle(cx, cy, px[k], py[k], px[(k + 1) % 10], py[(k + 1) % 10], c);
        break;
    }
    case Icon::Music:
        Ui::circle(cx - h * 0.35f, cy + h * 0.45f, h * 0.28f, c);
        Ui::circle(cx + h * 0.45f, cy + h * 0.3f, h * 0.28f, c);
        bar(cx - h * 0.1f, cy + h * 0.45f, cx - h * 0.1f, cy - h * 0.6f, lw, c);
        bar(cx + h * 0.7f, cy + h * 0.3f, cx + h * 0.7f, cy - h * 0.75f, lw, c);
        bar(cx - h * 0.1f, cy - h * 0.6f, cx + h * 0.7f, cy - h * 0.75f, lw * 1.6f, c);
        break;
    }
}

void button(float x, float y, float w, float h, const std::string& label, int size,
            float focus, bool primary, Icon ic, bool withIcon)
{
    const Ui::Palette& p = Ui::pal();
    const float r = h * 0.5f;
    Ui::shadow(x + 1, y + 3, w - 2, h - 2, r, 5, p.shadow);
    if (focus > 0.01f) Ui::shadow(x - 2, y - 2, w + 4, h + 4, r + 2, 8 * focus, Ui::alpha(p.glow, focus));
    if (primary) Ui::roundRect(x, y, w, h, r, p.accent, p.accentDark);
    else         Ui::roundRect(x, y, w, h, r, p.cardTop, p.cardBottom);
    Ui::roundBorder(x, y, w, h, r, focus > 0.5f ? 2.5f : 1.5f,
                    focus > 0.01f ? Ui::mix(p.cardBorder, p.accent, focus) : p.cardBorder);
    const u32 tc = primary ? p.textOnAccent : p.text;
    const float iconS = h * 0.45f;
    const float tw = label.empty() ? 0 : textW(label, size);
    const float total = tw + (withIcon ? iconS + (tw > 0 ? 8 : 0) : 0);
    float cx = x + (w - total) * 0.5f;
    if (cx < x + 10) cx = x + 10;
    if (withIcon) { icon(ic, cx + iconS * 0.5f, y + h * 0.5f, iconS, tc); cx += iconS + 8; }
    if (!label.empty()) text(cx, y + (h - size) * 0.5f - 1, label, size, tc, x + w - 10 - cx);
}

void roundButton(float cx, float cy, float r, Icon ic, float focus, bool pressed)
{
    const Ui::Palette& p = Ui::pal();
    Ui::shadow(cx - r + 1, cy - r + 3, 2 * r - 2, 2 * r - 2, r, 5, p.shadow);
    if (focus > 0.01f) Ui::shadow(cx - r - 2, cy - r - 2, 2 * r + 4, 2 * r + 4, r + 2, 8 * focus, Ui::alpha(p.glow, focus));
    Ui::circle(cx, cy, r, pressed ? p.accentDark : Ui::mix(p.cardTop, 0xFFFFFFFF, 0.25f));
    Ui::circle(cx, cy, r - 2, pressed ? p.accent : p.cardTop);
    icon(ic, cx, cy, r * 1.0f, pressed ? p.textOnAccent : p.text);
}

void progress(float x, float y, float w, float frac, float h)
{
    const Ui::Palette& p = Ui::pal();
    if (frac < 0) return;
    if (frac > 1) frac = 1;
    Ui::roundRect(x, y, w, h, h * 0.5f, 0x00000080);
    if (frac > 0) Ui::roundRect(x, y, w * frac < h ? h : w * frac, h, h * 0.5f, p.accent);
}

void spinner(float cx, float cy, float r)
{
    const Ui::Palette& p = Ui::pal();
    const float t = ticks_to_millisecs(gettime()) / 1000.0f;
    for (int i = 0; i < 8; ++i) {
        const float a = t * 6.0f + i * (float)M_PI / 4;
        Ui::circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.16f + i * r * 0.02f, Ui::alpha(p.accent, (i + 1) / 8.0f));
    }
}

void logo(float x, float cy, float h)
{
    static GRRLIB_texImg* s_logo = nullptr;
    if (!s_logo) s_logo = GRRLIB_LoadTexture(data_logo_wiifin_png);
    if (!s_logo || !s_logo->h) return;
    const float w = h * s_logo->w / s_logo->h;
    Ui::texRound(s_logo, x, cy - h * 0.5f, w, h, 0, 0, 0, 1, 1, 0xFFFFFFFF);
}

void hints(float x, float y, const Hint* hs, int n, int size, u32 color, bool rightAlign)
{
    const Ui::Palette& p = Ui::pal();
    const float d = size + 8;   /* a button's disc */
    float total = 0;
    for (int i = 0; i < n; ++i) total += d + 6 + textW(hs[i].label, size) + (i + 1 < n ? 22 : 0);
    float cx = rightAlign ? x - total : x;
    for (int i = 0; i < n; ++i) {
        const char* b = Ui::buttonName(hs[i].button);
        const bool wide = strlen(b) > 1;
        const float bw = wide ? textW(b, size - 4) + 14 : d;
        Ui::roundRect(cx, y, bw, d, d * 0.5f, Ui::mix(p.cardTop, 0xFFFFFFFF, 0.2f), p.cardBottom);
        Ui::roundBorder(cx, y, bw, d, d * 0.5f, 1.5f, p.cardBorder);
        Ui::textCentered(cx + bw * 0.5f, y + (d - (size - 3)) * 0.5f - 1, b, size - 3, p.text);
        cx += bw + 6;
        cx += text(cx, y + (d - size) * 0.5f - 1, hs[i].label, size, color) + 22;
    }
}

std::string clockText()
{
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char b[16];
    if (Ui::clock12h()) snprintf(b, sizeof(b), "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min,
                                 t.tm_hour < 12 ? "AM" : "PM");
    else snprintf(b, sizeof(b), "%d:%02d", t.tm_hour, t.tm_min);
    return b;
}

/* ---- scrolling ------------------------------------------------------------- */

void Scroller::update(const Touch& t, bool inside)
{
    if (t.pressed && inside) { grabbed = true; vel = 0; target = -1; }
    if (!t.down) grabbed = false;
    if (grabbed && t.dragging) {
        const float d = horizontal ? t.dx : t.dy;
        pos -= d;
        vel = -d;
    } else if (!grabbed) {
        if (target >= 0) {
            pos += (target - pos) * 0.3f;
            if (fabsf(target - pos) < 0.5f) { pos = target; target = -1; }
        } else if (fabsf(vel) > 0.2f) {
            pos += vel;
            vel *= 0.94f;
        } else vel = 0;
    }
    if (pos > maxPos) { pos = maxPos > 0 ? maxPos : 0; vel = 0; }
    if (pos < 0) { pos = 0; vel = 0; }
}

void Scroller::show(float a, float b, float view)
{
    float want = target >= 0 ? target : pos;
    if (a < want) want = a;
    else if (b > want + view) want = b - view;
    if (want > maxPos) want = maxPos > 0 ? maxPos : 0;
    if (want < 0) want = 0;
    if (want != pos) { target = want; vel = 0; }
}

}
