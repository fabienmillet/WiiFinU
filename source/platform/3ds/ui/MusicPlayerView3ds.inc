/* -----------------------------------------------------------------------
 * The music player on the 3DS (included by MusicPlayerView.cpp, in its
 * namespace: the session S, drawControl, drawSeekBar...).  The bottom
 * screen, in the Wii frame's units (the 640x480 space at half size: sizes
 * here are twice the pixels): the controls, the seek bar, what comes
 * next, Up Next.  The top screen: the cover, the title, the artist.
 * ----------------------------------------------------------------------- */
} // namespace (the 3DS's helpers, then back in)
#include "../platform/3ds/ui/Ui3ds.h"
namespace {

std::string fmtSecs(float t)
{
    if (t < 0) t = 0;
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", (int)t / 60, (int)t % 60);
    return b;
}

void drawPanel3ds()
{
    Session& s = *S;
    const Ui::Palette& p = Ui::pal();
    Ui::roundRect(panelX(), PANEL_Y, PANEL_W, PANEL_H, 20, p.cardTop, p.cardBottom);
    Ui::roundBorder(panelX(), PANEL_Y, PANEL_W, PANEL_H, 20, 2.0f, Ui::alpha(p.cardBorder, 0.8f));
    Ui::text(panelX() + 24, PANEL_Y + 14, tr("Up Next"), 28, p.text);
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d / %d", s.pos + 1, (int)s.order.size());
    Ui::textRight(panelX() + PANEL_W - 24, PANEL_Y + 20, cnt, 22, p.textDim);
    const int n = (int)s.order.size();
    for (int r = 0; r < PANEL_ROWS && s.panelTop + r < n; ++r) {
        const int k = s.panelTop + r;
        const MusicTrack& t = s.tracks[s.order[k]];
        const float y = ROWS_Y + r * ROW_H;
        const bool sel = k == s.panelSel, now = k == s.pos;
        if (sel) Ui::roundRect(panelX() + 10, y, PANEL_W - 20, ROW_H - 4, (ROW_H - 4) / 2, p.accent, p.accentDark);
        const u32 c = sel ? p.textOnAccent : (now ? p.accent : p.text);
        std::string title, artist;
        splitName(t, "", title, artist);
        std::string line = artist.empty() ? title : title + "  \xc2\xb7  " + artist;
        Ui::text(panelX() + 28, y + 8, LibDraw::fitText(Ui::font(), line, 24, (int)PANEL_W - 60).c_str(), 24, c);
    }
    /* scroll arrows under the rows */
    const float acx = panelX() + PANEL_W / 2;
    if (s.panelTop > 0) Ui::triangle(acx - 40, ARROW_Y + 8, acx - 20, ARROW_Y + 8, acx - 30, ARROW_Y - 6, p.text);
    if (s.panelTop + PANEL_ROWS < n) Ui::triangle(acx + 20, ARROW_Y - 6, acx + 40, ARROW_Y - 6, acx + 30, ARROW_Y + 8, p.text);
}

void drawScreen3ds(bool live, bool loading)
{
    Session& s = *S;
    const Ui::Palette& p = Ui::pal();
    const float pos = live ? position() : s.seekBase, dur = duration();
    const bool playing = live && !g_mplayer_paused;

    Ui::background(false);
    /* back, and where the track is in the queue */
    Ui3ds::icon(Ui3ds::Icon::Back, 30, 30, 30, p.text);
    Ui::text(64, 16, tr("NOW PLAYING"), 22, p.textDim);
    if (s.order.size() > 1) {
        char b[96];
        snprintf(b, sizeof(b), s.shuffle ? tr("%d of %d  \xc2\xb7  Shuffle") : tr("%d of %d"), s.pos + 1, (int)s.order.size());
        Ui::textRight(608, 16, b, 22, p.textDim);
    }
    /* the title and the artist (the top screen shows them big too) */
    Ui::text(COL_X, 72, LibDraw::fitText(Ui::font(), s.title, 34, (int)COL_W).c_str(), 34, p.text);
    if (!s.artist.empty())
        Ui::text(COL_X, 120, LibDraw::fitText(Ui::font(), s.artist, 24, (int)COL_W).c_str(), 24, p.accent);

    drawSeekBar(pos, dur, live && !s.pointer && s.focusBar && !s.panel);
    Ui::text(BAR_X, BAR_Y + 16, fmtSecs(pos).c_str(), 22, p.text);
    if (dur > 0) Ui::textRight(BAR_X + BAR_W, BAR_Y + 16, ("-" + fmtSecs(dur - pos)).c_str(), 22, p.textDim);
    for (int i = 0; i < C_COUNT; ++i) {
        const bool f = live && !s.panel && (s.pointer ? s.hover == i : (!s.focusBar && s.focus == i));
        drawControl(i, f, live ? playing : true);
    }
    if (loading) Ui::textCentered(320, 168, tr("Loading..."), 24, p.textDim);
    /* what comes next */
    const int nextPos = s.repeat == 2 ? s.pos : (s.pos + 1 < (int)s.order.size() ? s.pos + 1 : (s.repeat == 1 ? 0 : -1));
    if (nextPos >= 0) {
        std::string t, ar;
        splitName(s.tracks[s.order[nextPos]], "", t, ar);
        char line[512];
        if (ar.empty()) snprintf(line, sizeof(line), tr("Next:  %s"), t.c_str());
        else            snprintf(line, sizeof(line), tr("Next:  %s  \xe2\x80\x94  %s"), t.c_str(), ar.c_str());
        Ui::textCentered(320, 416, LibDraw::fitText(Ui::font(), line, 22, 580).c_str(), 22, p.textDim);
    }
    if (s.panelK > 0.01f && s.panel) drawPanel3ds();
    if (!s.toastMsg.empty() && nowMs() < s.toastUntil) {
        const char* msg = tr(s.toastMsg.c_str());
        const int w = Ui::textWidth(msg, 24) + 40;
        Ui::roundRect(320 - w / 2, 180, w, 44, 22, p.accent, p.accentDark);
        Ui::textCentered(320, 189, msg, 24, p.textOnAccent);
    }

    /* the top screen: the cover, the title, the artist, the album */
    static Ui3ds::TopInfo s_top;
    s_top = Ui3ds::TopInfo();
    s_top.square = true;
    s_top.poster = s.art;
    s_top.title = s.title;
    s_top.line1 = s.artist;
    s_top.line2 = s.detail;
    if (dur > 0) s_top.progress = pos / dur;
    Ui3ds::setTopInfo(&s_top);
}
