/* -----------------------------------------------------------------------
 * PlayerView's drawing on the 3DS, included by PlayerView.cpp (its state,
 * its helpers).  The picture is on the top screen; the bottom one is the
 * controls, always there, drawn 1:1 in its pixels: the title, the seek bar
 * (tap to go there), the transport buttons, audio / subtitles / zoom /
 * back, the track picker.  The layout constants are in the Wii frame's
 * units (2 a pixel) as the touch reads them: H = 0.5 to draw.
 * ----------------------------------------------------------------------- */
#include "../platform/3ds/ui/Ui3ds.h"

void PlayerView::render(const ir_t& ir)
{
    (void)ir;
    using namespace Ui3ds;
    const float H = 0.5f;
    const Ui::Palette& p = Ui::pal();

    /* subtitles: on the top screen, over the picture */
    if (subtitles && !subtitles->empty()) Player3ds::setTopOverlay(subtitleOnTop, this);

    bottomSpace();
    background(BW, BH);

    /* title, and which episode */
    text(12, 8, ctx.title, 15, p.text, BW - 24);
    if (!ctx.episodes.empty()) {
        char b[96];
        snprintf(b, sizeof(b), tr("Episode %d of %d"), ctx.episodeIdx + 1, (int)ctx.episodes.size());
        text(12, 28, b, 11, p.textDim, BW - 24);
    }

    /* the seek bar and the times */
    const float dur = duration();
    const float pos = seekPending ? seekPendingTo : displayPosition();
    const float bx = BAR_X * H, by = BAR_Y * H, bw = BAR_W * H;
    /* the track: the theme's field colour is too close to the background */
    Ui::roundRect(bx, by, bw, 6, 3, Ui::alpha(p.text, 0.25f));
    if (dur > 0) {
        const float f = pos / dur < 0 ? 0 : pos / dur > 1 ? 1 : pos / dur;
        Ui::roundRect(bx, by, bw * f, 6, 3, p.accent);
        Ui::circle(bx + bw * f, by + 3, hoverBar ? 8 : 6, p.text);
    }
    text(bx, TIME_Y * H, fmtTime(pos), 12, seekPending ? p.accent : p.text);
    if (dur > 0) textR(bx + bw, TIME_Y * H, "-" + fmtTime(dur - pos), 12, p.textDim);

    /* transport: |< -10 play/pause +10 >| */
    static const Icon ICONS[5] = { Icon::Prev, Icon::Rewind, Icon::Play, Icon::Forward, Icon::Next };
    for (int i = 0; i < BTN_COUNT; ++i) {
        const float x = (BTN_X0 + i * (BTN_W + BTN_GAP)) * H, y = BTN_Y * H;
        const float w = BTN_W * H, h = BTN_H * H;
        const bool main = i == 2;
        button(x, y, w, h, "", 12, hoverButton == i ? 1.0f : 0.0f, main);
        Icon ic = ICONS[i];
        if (main && !g_mplayer_paused) ic = Icon::Pause;
        icon(ic, x + w / 2, y + h / 2 - (i == 1 || i == 3 ? 5 : 0), main ? 24 : 18, main ? p.textOnAccent : p.text);
        if (i == 1 || i == 3) textC(x + w / 2, y + h / 2 + 6, i == 1 ? "-10s" : "+10s", 10, p.textDim);
    }

    /* audio, subtitles, zoom, back */
    {
        const Icon ic[4] = { Icon::Audio, Icon::Subtitles, Icon::Grid, Icon::Back };
        const char* lb[4] = { tr("Audio"), tr("Subtitles"),
                              VideoSurface::zoom() == VideoSurface::Zoom::Fill ? tr("Fill") : tr("Fit"), tr("Back") };
        const int ids[4] = { BTN_AUDIO, BTN_SUBS, BTN_ZOOM, BTN_BACK };
        for (int i = 0; i < 4; ++i) {
            const float x = (ROW2_X0 + i * (ROW2_W + ROW2_GAP)) * H;
            button(x, ROW2_Y * H, ROW2_W * H, ROW2_H * H, lb[i], 10, hoverButton == ids[i] ? 1.0f : 0.0f,
                   false, ic[i], true);
        }
    }

    /* skip the intro, the credits... */
    if (introVisible())
        button(INTRO_X * H, INTRO_Y * H, INTRO_W * H, INTRO_H * H, tr(segmentLabel(activeSegment())), 12,
               hoverIntro ? 1.0f : 0.0f, true, Icon::Forward, true);

    /* loading, buffering, switching streams */
    if (!g_wiifin_loading_active) loadingSince = 0;
    else if (!loadingSince) loadingSince = nowMs();
    const bool stalled = buffering();
    if (!busyMsg.empty() || rebufPercent >= 0 || stalled ||
        (g_wiifin_loading_active && nowMs() - loadingSince > 250)) {
        char msg[96];
        if (!busyMsg.empty())       snprintf(msg, sizeof(msg), "%s", tr(busyMsg.c_str()));
        else if (rebufPercent >= 0) snprintf(msg, sizeof(msg), tr("Buffering... %d%%"), rebufPercent);
        else if (stalled)           snprintf(msg, sizeof(msg), "%s", tr("Buffering..."));
        else                        snprintf(msg, sizeof(msg), "%s", tr("Loading..."));
        const float w = textW(msg, 12) + 44;
        Ui::roundRect((BW - w) / 2, 46, w, 26, 13, p.cardTop, p.cardBottom);
        spinner((BW - w) / 2 + 16, 59, 6);
        text((BW - w) / 2 + 30, 52, msg, 12, p.text);
    }

    /* the track picker, over the rest */
    if (panel != Panel::None) {
        rect(0, 0, BW, BH, Ui::alpha(p.dim, 0.9f));
        Ui::roundRect(8, 8, BW - 16, BH - 16, 10, p.cardTop, p.cardBottom);
        text(20, 16, panel == Panel::Audio ? tr("Audio track") : tr("Subtitles"), 13, p.text);
        const int n = panelCount();
        for (int r = 0; r < LIST_ROWS && panelTop + r < n; ++r) {
            const int row = panelTop + r;
            const float y = (LIST_Y + 44 + r * ROW_H) * H, x = LIST_X * H, w = LIST_W * H, h = ROW_H * H;
            if (row == panelSel) Ui::roundRect(x, y + 1, w, h - 2, 6, Ui::alpha(p.accent, 0.4f));
            text(x + 10, y + (h - 12) / 2 - 1, panelLabel(row), 12, p.text, w - 40);
            if (panelRowIsCurrent(row)) icon(Icon::Check, x + w - 14, y + h / 2, 12, p.accent);
        }
    }

    /* a message (volume, zoom, no next episode...) */
    if (nowMs() < toastUntil && !toastMsg.empty()) {
        const float w = textW(toastMsg, 12) + 24;
        Ui::roundRect((BW - w) / 2, 86, w, 24, 12, p.cardTop, p.cardBottom);
        textC(BW / 2, 91, toastMsg, 12, p.text);
    }
    wiiSpace();
}
