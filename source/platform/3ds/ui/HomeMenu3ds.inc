/* -----------------------------------------------------------------------
 * The START menu on the 3DS (included by HomeMenu.cpp): leave for the
 * Homebrew Launcher, or not.  The console's own HOME button is the
 * system's menu.  Returns true to leave.
 * ----------------------------------------------------------------------- */
#include "../platform/3ds/ui/Ui3ds.h"

bool doShowHomeOverlay(GRRLIB_ttfFont* font, GRRLIB_texImg* btnTex,
                       GRRLIB_texImg* cursorPointerTex, bool musicEnabled)
{
    (void)font; (void)btnTex; (void)cursorPointerTex;
    using namespace Ui3ds;
    SYS_Report("[WiiFin] HOME menu\n");
    MusicBGM::stopMusic();
    SoundFX::play(SoundFX::FX::MenuEnter);
    const u64 openedAt = ticks_to_millisecs(gettime());
    Touch touch;
    int sel = 0;   /* 0 stay, 1 leave */
    ir_t ir;
    for (;;) {
        Input::update();
        Input::readIR(ir);
        if (g_app_powerOff || g_app_reset) return true;
        readTouch(ir, touch);
        const Ui::Palette& p = Ui::pal();
        const bool start = Input::isHomePressed() && ticks_to_millisecs(gettime()) - openedAt > 300;
        if ((Input::isBPressed() && !touch.tap) || start) {
            SoundFX::play(SoundFX::FX::Back);
            if (musicEnabled) MusicBGM::resume();
            return false;
        }
        if (Input::isLeftPressed()) sel = 0;
        if (Input::isRightPressed()) sel = 1;

        bottomSpace();
        background(BW, BH);
        Ui::roundRect(16, 50, BW - 32, 140, 14, p.cardTop, p.cardBottom);
        Ui::roundBorder(16, 50, BW - 32, 140, 14, 1.5f, p.cardBorder);
        textC(BW / 2, 70, tr("Back to the Homebrew Launcher?"), 14, p.text, BW - 48);
        textC(BW / 2, 92, tr("(Anything not saved will be lost.)"), 11, p.textDim, BW - 48);
        button(32, 130, 120, 40, tr("No"), 14, sel == 0 ? 1.0f : 0.0f, false);
        button(BW - 152, 130, 120, 40, tr("Exit"), 14, sel == 1 ? 1.0f : 0.0f, true);
        const bool tapNo = hit(touch, 32, 130, 120, 40), tapYes = hit(touch, BW - 152, 130, 120, 40);
        setTopInfo(nullptr);
        wiiSpace();
        GRRLIB_Render();
        const bool a = Input::isAJustPressed() && !touch.tap;
        if (tapNo || (a && sel == 0)) {
            SoundFX::play(SoundFX::FX::Back);
            if (musicEnabled) MusicBGM::resume();
            return false;
        }
        if (tapYes || (a && sel == 1)) {
            SoundFX::play(SoundFX::FX::MenuExit);
            SoundFX::waitDone(SoundFX::FX::MenuExit);
            return true;
        }
    }
}
