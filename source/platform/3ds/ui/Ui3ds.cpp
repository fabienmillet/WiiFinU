#include "Ui3ds.h"
#include "../../../ui/Ui.h"
#include "../../../core/Text.h"
#include "../../../input/Input.h"

#include <math.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>

extern "C" int Input3ds_touch(float* x, float* y, int* tap, int* dragged);

namespace Ui3ds {

/* ---- spaces -------------------------------------------------------------- */

static void space(float w, float h, float frameW, float frameH)
{
    Mtx44 m;
    guOrtho(m, 0, h, 0, w, 0, 1000);
    GX_LoadProjectionMtx(m, GX_ORTHOGRAPHIC);
    GX_SetViewport(0, 0, frameW, frameH, 0, 1);
    guMtxIdentity(GXmodelView2D);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    Text::setFrameMapping(0.0f, 1.0f, 1.0f);
    GRRLIB_ClipReset();
}

void bottomSpace() { space(BW, BH, 640, 480); }
void topSpace()    { space(TW, TH, 800, 480); }

void wiiSpace()
{
    Ui::initScreen(false);
    Ui::setSafeArea(0, 0, 0, 0);
    guMtxIdentity(GXmodelView2D);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
}

/* ---- touch ---------------------------------------------------------------- */

void readTouch(const ir_t& ir, Touch& t)
{
    (void)ir;
    float x, y;
    int tap = 0, dragged = 0;
    const bool down = Input3ds_touch(&x, &y, &tap, &dragged) != 0;
    x *= 0.5f; y *= 0.5f;                     /* frame pixels -> screen */
    t.pressed = down && !t.down;
    if (t.pressed) { t.startX = x; t.startY = y; t.dx = t.dy = 0; }
    else if (down) { t.dx = x - t.x; t.dy = y - t.y; }
    else           { t.dx = t.dy = 0; }
    t.down = down;
    t.dragging = down && dragged;
    t.tap = tap != 0;
    if (down || tap) { t.x = x; t.y = y; }
}

/* ---- drawing -------------------------------------------------------------- */

void rect(float x, float y, float w, float h, u32 c) { gradient(x, y, w, h, c, c); }

void gradient(float x, float y, float w, float h, u32 top, u32 bottom)
{
    if (w <= 0 || h <= 0) return;
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

void background(int w, int h)
{
    const Ui::Palette& p = Ui::pal();
    gradient(0, 0, w, h, p.bgTop, p.bgBottom);
}

void image(GRRLIB_texImg* tex, float x, float y, float w, float h, float r, u32 tint)
{
    if (!tex || !tex->w || !tex->h) return;
    Ui::texCover(tex, x, y, w, h, r, w / h, tint);
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
    /* a thick line as a quad */
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
    const float h = s * 0.5f, lw = s > 16 ? 2.2f : 1.8f;
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
            bar(cx - cosf(a) * h * 0.8f, cy - sinf(a) * h * 0.8f, cx + cosf(a) * h * 0.8f, cy + sinf(a) * h * 0.8f, lw + 1.2f, c);
        }
        Ui::circle(cx, cy, h * 0.55f, c);
        Ui::circle(cx, cy, h * 0.25f, Ui::pal().barBottom);
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
    }
}

void button(float x, float y, float w, float h, const std::string& label, int size,
            float focus, bool primary, Icon ic, bool withIcon)
{
    const Ui::Palette& p = Ui::pal();
    const float r = h < 30 ? h * 0.5f : 10;
    if (focus > 0.01f) Ui::shadow(x, y, w, h, r, 4 * focus, Ui::alpha(p.glow, focus));
    if (primary) Ui::roundRect(x, y, w, h, r, p.accent, p.accentDark);
    else         Ui::roundRect(x, y, w, h, r, p.cardTop, p.cardBottom);
    Ui::roundBorder(x, y, w, h, r, focus > 0.5f ? 2.0f : 1.0f,
                    focus > 0.01f ? Ui::mix(p.cardBorder, p.accent, focus) : p.cardBorder);
    const u32 tc = primary ? p.textOnAccent : p.text;
    const float iconS = h * 0.5f;
    float tw = label.empty() ? 0 : textW(label, size);
    float total = tw + (withIcon ? iconS + (tw > 0 ? 6 : 0) : 0);
    float cx = x + (w - total) * 0.5f;
    if (withIcon) { icon(ic, cx + iconS * 0.5f, y + h * 0.5f, iconS, tc); cx += iconS + 6; }
    if (!label.empty()) text(cx, y + (h - size) * 0.5f - 1, label, size, tc, w - 8);
}

int topBar(const Touch& t, const std::string& title, bool canBack, const Icon* icons, int n, int focusIcon)
{
    const Ui::Palette& p = Ui::pal();
    gradient(0, 0, BW, BAR_H, p.barTop, p.barBottom);
    rect(0, BAR_H - 1, BW, 1, p.barBorder);
    int tapped = -2;
    float x = 8;
    if (canBack) {
        icon(Icon::Back, 16, BAR_H * 0.5f, 16, p.text);
        if (hit(t, 0, 0, 40, BAR_H + 6)) tapped = -1;
        x = 32;
    }
    const float right = BW - n * 30 - 4;
    if (title.empty()) logo(x + 34, BAR_H * 0.5f, 14);
    else text(x, (BAR_H - 14) * 0.5f - 1, title, 14, p.text, right - x - 4);
    for (int i = 0; i < n; ++i) {
        const float cx = right + 15 + i * 30;
        if (i == focusIcon) Ui::circle(cx, BAR_H * 0.5f, 12, Ui::alpha(p.accent, 0.5f));
        icon(icons[i], cx, BAR_H * 0.5f, 16, p.text);
        if (hit(t, cx - 15, 0, 30, BAR_H + 6)) tapped = i;
    }
    return tapped;
}

void progress(float x, float y, float w, float frac)
{
    const Ui::Palette& p = Ui::pal();
    if (frac < 0) return;
    if (frac > 1) frac = 1;
    rect(x, y, w, 3, 0x00000099);
    rect(x, y, w * frac, 3, p.accent);
}

void spinner(float cx, float cy, float r)
{
    const Ui::Palette& p = Ui::pal();
    const float t = ticks_to_millisecs(gettime()) / 1000.0f;
    for (int i = 0; i < 8; ++i) {
        const float a = t * 6.0f + i * (float)M_PI / 4;
        Ui::circle(cx + cosf(a) * r, cy + sinf(a) * r, 2.0f + i * 0.25f, Ui::alpha(p.accent, (i + 1) / 8.0f));
    }
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
            vel *= 0.92f;
        } else vel = 0;
    }
    if (pos < 0) { pos = 0; vel = 0; }
    if (pos > maxPos) { pos = maxPos > 0 ? maxPos : 0; vel = 0; }
}

void Scroller::show(float a, float b, float view)
{
    float want = target >= 0 ? target : pos;
    if (a < want) want = a;
    else if (b > want + view) want = b - view;
    if (want < 0) want = 0;
    if (want > maxPos) want = maxPos > 0 ? maxPos : 0;
    if (want != pos) { target = want; vel = 0; }
}

/* ---- the top screen ------------------------------------------------------- */

static const TopInfo* s_top = nullptr;
void setTopInfo(const TopInfo* info) { s_top = info; }
bool hasTopInfo() { return s_top != nullptr; }

void drawTopInfo()
{
    if (!s_top) return;
    const TopInfo& ti = *s_top;
    const Ui::Palette& p = Ui::pal();
    background(TW, TH);
    float textX = 14, textW = TW - 28;
    if (ti.backdrop) {
        image(ti.backdrop, 0, 0, TW, TH);
        gradient(0, 70, TW, 90, 0x00000000, 0x000000C0);
        rect(0, 160, TW, 80, 0x000000D8);
    } else if (ti.poster && ti.square) {
        Ui::shadow(16, 30, 180, 180, 8, 8, p.shadow);
        image(ti.poster, 16, 30, 180, 180, 8);
        textX = 210; textW = TW - textX - 12;
    } else if (ti.poster) {
        Ui::shadow(14, 20, 120, 180, 6, 6, p.shadow);
        image(ti.poster, 14, 20, 120, 180, 6);
        textX = 148; textW = TW - textX - 14;
    } else if (ti.square) {   /* a cover still loading: its place kept */
        Ui::roundRect(16, 30, 180, 180, 8, p.cardTop, p.cardBottom);
        icon(Icon::Audio, 106, 120, 48, p.textDim);
        textX = 210; textW = TW - textX - 12;
    }
    const u32 tc = ti.backdrop ? 0xFFFFFFFF : p.text;
    const u32 dc = ti.backdrop ? 0xD0D8E0FF : p.textDim;
    float y = ti.backdrop ? 122 : ti.square ? 50 : 24;
    if (ti.backdrop && ti.overview.empty()) y = 168;
    text(textX, y, ti.title, 20, tc, textW);
    y += 26;
    if (!ti.line1.empty()) { text(textX, y, ti.line1, 12, dc, textW); y += 16; }
    if (!ti.line2.empty()) { text(textX, y, ti.line2, 12, dc, textW); y += 16; }
    if (ti.progress >= 0) { progress(textX, y + 2, textW < 160 ? textW : 160, ti.progress); y += 10; }
    if (!ti.overview.empty())
        paragraph(textX, y + 2, textW, ti.overview, 12, tc, (int)((TH - 8 - y) / 15.6f));
}

void logo(float cx, float cy, int size)
{
    const Ui::Palette& p = Ui::pal();
    const float tw = textW(APP_NAME, size), s = size * 1.2f;
    const float x = cx - (tw + s + 6) * 0.5f;
    Ui::roundRect(x, cy - s * 0.5f, s, s, s * 0.28f, p.accent, p.accentDark);
    icon(Icon::Play, x + s * 0.52f, cy, s * 0.8f, p.textOnAccent);
    text(x + s + 6, cy - size * 0.5f - 1, APP_NAME, size, p.text);
}

}
