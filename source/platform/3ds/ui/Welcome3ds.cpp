#include "Welcome3ds.h"
#include "../../../ui/Ui.h"
#include "../../../input/Input.h"
#include "../../../core/Lang.h"
#include "../../../core/SoundFX.h"

using namespace Ui3ds;

Welcome3ds::Result Welcome3ds::update(ir_t& ir)
{
    ++frames;
    readTouch(ir, touch);
    const Ui::Palette& p = Ui::pal();
    const bool a = Input::isAJustPressed() && !touch.tap;
    const int n = (int)profiles.size() + 1;   /* + "Add an account" */
    if (selected >= n) selected = n - 1;

    bottomSpace();
    background(BW, BH);
    static const Icon ICONS[2] = { Icon::Gear, Icon::Close };
    const int tapped = topBar(touch, "", false, ICONS, 2);
    text(12, BAR_H + 8, tr("Who's watching?"), 14, p.text);

    const float y0 = BAR_H + 30, rowH = 44;
    vs.maxPos = n * rowH - (BH - y0) + 4;
    vs.update(touch, touch.startY >= y0);
    Result r = Result::None;
    for (int i = 0; i < n; ++i) {
        const float y = y0 + i * rowH - vs.pos;
        if (y + rowH < y0 || y > BH) continue;
        const bool f = i == selected;
        Ui::roundRect(10, y + 2, BW - 20, rowH - 6, 10, f ? Ui::alpha(p.accent, 0.35f) : p.cardTop,
                      f ? Ui::alpha(p.accent, 0.35f) : p.cardBottom);
        if (f) Ui::roundBorder(10, y + 2, BW - 20, rowH - 6, 10, 2, p.accent);
        if (i < (int)profiles.size()) {
            const SavedProfile& sp = profiles[i];
            const std::string name = sp.username.empty() ? std::string(tr("Quick Connect")) : sp.username;
            Ui::circle(32, y + rowH / 2 - 1, 14, p.accent);
            std::string initial = name.substr(0, 1);
            if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
            textC(32, y + rowH / 2 - 9, initial, 14, p.textOnAccent);
            text(54, y + 7, name, 14, p.text, BW - 54 - 20);
            text(54, y + 24, sp.serverName.empty() ? sp.serverUrl : sp.serverName, 10, p.textDim, BW - 54 - 20);
        } else {
            Ui::circle(32, y + rowH / 2 - 1, 14, p.cardBorder);
            textC(32, y + rowH / 2 - 10, "+", 16, p.text);
            text(54, y + 13, tr("Add an account"), 13, p.text);
        }
        if (touch.y >= y0 && hit(touch, 10, y, BW - 20, rowH)) {
            selected = i;
            r = i < (int)profiles.size() ? Result::Open : Result::Add;
        }
    }
    if (Input::isUpPressed() && selected > 0) { --selected; SoundFX::play(SoundFX::FX::Move); }
    if (Input::isDownPressed() && selected + 1 < n) { ++selected; SoundFX::play(SoundFX::FX::Move); }
    if (Input::isUpPressed() || Input::isDownPressed()) vs.show(selected * rowH, selected * rowH + rowH, BH - y0);
    if (a) r = selected < (int)profiles.size() ? Result::Open : Result::Add;
    /* X: remove the profile (pressed twice) */
    if (Input::is2Pressed() && selected < (int)profiles.size()) {
        if (removeArmed && frames < removeArmed) r = Result::Remove;
        else removeArmed = frames + 180;
    }
    if (removeArmed && frames < removeArmed) {
        const std::string m = tr("X again: remove this profile");
        const float w = textW(m, 12) + 24;
        Ui::roundRect((BW - w) / 2, BH - 30, w, 24, 12, p.cardTop, p.cardBottom);
        textC(BW / 2, BH - 25, m, 12, p.text);
    }
    setTopInfo(nullptr);   /* the top screen: the name and the clock */
    wiiSpace();
    if (tapped == 0) return Result::Settings;
    if (tapped == 1) return Result::Exit;
    if (r != Result::None) SoundFX::play(SoundFX::FX::Start);
    return r;
}
