/* -----------------------------------------------------------------------
 * SettingsView on the 3DS, included by SettingsView.cpp (its state, its
 * rows, activate()).  Drawn 1:1 on the bottom screen: the sections, then a
 * section's rows (a tap or A changes one); the top screen says what the
 * selected row does.
 * ----------------------------------------------------------------------- */

namespace {
const float S3_Y0 = Ui3ds::BAR_H + 6, S3_ROW_H = 40;
}

bool SettingsView::update(ir_t& ir) {
    using namespace Ui3ds;
    readTouch(ir, touch3ds);
    const bool aPressed = Input::isAJustPressed() && !touch3ds.tap;

    if (pinStep) {   /* a new passcode, typed twice (the Wii's pad) */
        const int r = pin.update(ir);
        if (r < 0) { pinStep = 0; signInWant = profile->signIn; return false; }
        if (r == 0) return false;
        if (pinStep == 1) {
            pinFirst = pin.code();
            pinStep = 2;
            pin.start(N_("Enter it again"));
        } else if (pin.code() != pinFirst) {
            pinStep = 1;
            pin.start(N_("Choose a passcode"), N_("The two didn't match: once more"));
            pin.reject();
        } else {
            profile->passSalt = PinPad::newSalt();
            profile->passHash = PinPad::hash(profile->passSalt, pin.code());
            profile->signIn   = SavedProfile::Passcode;
            pinStep = 0;
            section = -1;
            SoundFX::play(SoundFX::FX::Start);
        }
        return false;
    }

    /* the back arrow of the bar (drawn by render) */
    const bool back = (Input::isBackPressed() && !touch3ds.tap) || hit(touch3ds, 0, 0, 40, BAR_H + 6);

    if (section < 0) {   /* the sections */
        if (back) { SoundFX::play(SoundFX::FX::Back); return true; }
        const int n = sectionCount(), was = menuSel;
        if (Input::isUpPressed()) menuSel = (menuSel + n - 1) % n;
        if (Input::isDownPressed()) menuSel = (menuSel + 1) % n;
        int tapped = -1;
        for (int i = 0; i < n; ++i)
            if (hit(touch3ds, 20, 52 + i * 52, BW - 40, 42)) tapped = i;
        if (tapped >= 0) menuSel = tapped;
        if (menuSel != was) SoundFX::play(SoundFX::FX::Move);
        if (aPressed || tapped >= 0) {
            SoundFX::play(SoundFX::FX::Open);
            openSection(sectionAt(menuSel, profile));
            scroll3ds = Scroller();
        }
        return false;
    }

    if (back) {
        SoundFX::play(SoundFX::FX::Back);
        if (section != 2 || leaveSignIn()) section = -1;
        return false;
    }
    const int n = (int)rows.size();
    scroll3ds.maxPos = n * S3_ROW_H - (BH - S3_Y0) + 4;
    scroll3ds.update(touch3ds, touch3ds.startY >= S3_Y0);
    const int before = selectedIndex;
    if (Input::isUpPressed() && selectedIndex > 0) --selectedIndex;
    if (Input::isDownPressed() && selectedIndex + 1 < n) ++selectedIndex;
    if (selectedIndex != before) {
        SoundFX::play(SoundFX::FX::Move);
        scroll3ds.show(selectedIndex * S3_ROW_H, (selectedIndex + 1) * S3_ROW_H, BH - S3_Y0);
    }
    int tapped = -1;
    for (int i = 0; i < n; ++i)
        if (touch3ds.y >= S3_Y0 && hit(touch3ds, 8, S3_Y0 + i * S3_ROW_H - scroll3ds.pos, BW - 16, S3_ROW_H)) tapped = i;
    if (tapped >= 0) selectedIndex = tapped;
    if ((aPressed || tapped >= 0 || Input::isRightPressed()) && selectedIndex < n) {
        activate(rows[selectedIndex]);
        SoundFX::play(SoundFX::FX::Start);
    }
    return false;
}

void SettingsView::render(ir_t& ir) {
    using namespace Ui3ds;
    if (pinStep) { Ui::background(); pin.render(ir); return; }
    const Ui::Palette& p = Ui::pal();
    bottomSpace();
    background(BW, BH);

    static TopInfo s_top;
    s_top = TopInfo();
    if (section < 0) {
        topBar(touch3ds, tr("Settings"), true, nullptr, 0);
        const int n = sectionCount();
        for (int i = 0; i < n; ++i)
            button(20, 52 + i * 52, BW - 40, 42, tr(SECTIONS[sectionAt(i, profile)]), 14,
                   i == menuSel ? 1.0f : 0.0f, false);
        s_top.title = tr("Settings");
        s_top.line1 = APP_NAME " " WIIFIN_VERSION;
    } else {
        rows3ds.clear();
        describeRows();
        topBar(touch3ds, std::string(tr("Settings")) + " \xC2\xB7 " + tr(SECTIONS[section]), true, nullptr, 0);
        GRRLIB_ClipDrawing(0, (int)S3_Y0 * 2, BW * 2, (int)(BH - S3_Y0) * 2);
        for (int i = 0; i < (int)rows.size(); ++i) {
            const Row3ds* r = nullptr;
            for (const Row3ds& x : rows3ds) if (x.id == rows[i]) r = &x;
            if (!r) continue;
            const float y = S3_Y0 + i * S3_ROW_H - scroll3ds.pos;
            if (y + S3_ROW_H < S3_Y0 || y > BH) continue;
            const bool f = i == selectedIndex;
            Ui::roundRect(8, y + 2, BW - 16, S3_ROW_H - 4, 8, f ? Ui::alpha(p.accent, 0.3f) : p.cardTop,
                          f ? Ui::alpha(p.accent, 0.3f) : p.cardBottom);
            if (f) Ui::roundBorder(8, y + 2, BW - 16, S3_ROW_H - 4, 8, 2, p.accent);
            const float pw = textW(r->value, 11) + 18;
            text(16, y + 6, r->label, 13, p.text, BW - pw - 40);
            text(16, y + 22, r->desc, 9, p.textDim, BW - pw - 40);
            Ui::roundRect(BW - pw - 16, y + (S3_ROW_H - 20) / 2, pw, 20, 10, r->color);
            textC(BW - 16 - pw / 2, y + (S3_ROW_H - 20) / 2 + 4, r->value, 11, p.textOnAccent);
            if (f) {
                s_top.title = r->label;
                s_top.line1 = r->value;
                s_top.overview = r->desc;
            }
        }
        GRRLIB_ClipReset();
    }
    setTopInfo(&s_top);
    wiiSpace();
}
