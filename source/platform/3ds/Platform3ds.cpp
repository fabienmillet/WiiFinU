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
#include "../../core/Text.h"
#include "ui/Ui3ds.h"
#include "../../player/WiiPlayer.h"

#include <3ds.h>
#include <ogc/lwp_watchdog.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

extern "C" u64 g_3ds_lastInputMs;   /* Input3ds */

namespace {

std::string    s_caption;

/* During a video, the bottom screen's light goes off after a while
 * untouched (the battery), back on at any button or touch */
void bottomLight()
{
    const u64 now = osGetTime();
    static u64 s_videoSince = 0;
    const bool video = Player3ds::videoActive() && !g_mplayer_paused;
    if (!video) s_videoSince = 0;
    else if (!s_videoSince) s_videoSince = now;
    const u64 idle = g_3ds_lastInputMs ? ticks_to_millisecs(gettime()) - g_3ds_lastInputMs : ~0ULL;
    Gfx3ds::setBottomOff(video && now - s_videoSince > 8000 && idle > 8000);
}

/* The top screen: the video; else what the browsing screens show (the
 * item selected); else the app's name, the clock and the music playing.
 * Drawn 1:1 in its 400x240 pixels (the text's mapping put back after: the
 * bottom screen may be one of the Wii's screens, at half size) */
void drawTop()
{
    bottomLight();
    float l, sx, sy;
    Text::frameMapping(l, sx, sy);
    if (!Player3ds::drawTop()) {
        Ui3ds::topSpace();
        if (Ui3ds::hasTopInfo()) Ui3ds::drawTopInfo();
        else {
            const Ui::Palette& p = Ui::pal();
            Ui3ds::background(Ui3ds::TW, Ui3ds::TH);
            Ui3ds::logo(Ui3ds::TW / 2, 80, 30);
            time_t now = time(nullptr);
            struct tm lt;
            localtime_r(&now, &lt);
            char clock[16];
            if (Ui::clock12h())
                snprintf(clock, sizeof(clock), "%d:%02d %s", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min,
                         lt.tm_hour < 12 ? "AM" : "PM");
            else
                snprintf(clock, sizeof(clock), "%02d:%02d", lt.tm_hour, lt.tm_min);
            Ui3ds::textC(Ui3ds::TW / 2, 128, clock, 34, p.text);
            const char* playing = Ui::nowPlaying();
            const std::string line = playing && *playing ? playing : s_caption;
            if (!line.empty()) Ui3ds::textC(Ui3ds::TW / 2, 192, line, 14, p.textDim, Ui3ds::TW - 30);
        }
    }
    Text::setFrameMapping(l, sx, sy);
}

aptHookCookie s_hook;

/* The HOME menu or the lid closed during a video: it pauses (as the
 * Wii's HOME menu does) */
void onApt(APT_HookType type, void*)
{
    if ((type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP) && Player3ds::videoActive() && !g_mplayer_paused)
        wii_player_pause_toggle();
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
    aptHook(&s_hook, onApt, nullptr);
}

void setTopCaption(const std::string& s) { s_caption = s; }

bool isNew3DS()
{
    bool n = false;
    APT_CheckNew3DS(&n);
    return n;
}

}
