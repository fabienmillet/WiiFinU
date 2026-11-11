/* -----------------------------------------------------------------------
 * BrowseWiiU — see BrowseWiiU.h.  The pages and their loading follow the
 * 3DS's Browse3ds; the drawing is the Wii U's: the GamePad's 854x480
 * pixels for the navigation, the TV's 1280x720 units for the selection.
 * ----------------------------------------------------------------------- */
#include "BrowseWiiU.h"
#include "ImagesWiiU.h"
#include "../GfxWiiU.h"
#include "../InputWiiU.h"
#include "../SwkbdWiiU.h"
#include "../../../ui/Ui.h"
#include "../../../input/Input.h"
#include "../../../core/Lang.h"
#include "../../../core/SoundFX.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ogc/lwp.h>
#include <ogc/system.h>

using namespace UiWiiU;
using ImagesWiiU::Kind;

namespace {

/* jobs of pages left while they ran: finished in the background */
std::vector<std::shared_ptr<BrowseWiiU::Job>>* s_orphans = nullptr;

std::shared_ptr<BrowseWiiU::Job> runJob(std::function<void()> work, std::function<void()> apply)
{
    return runJobWiiU(std::move(work), std::move(apply));
}

/* ---- the GamePad's layout (its pixels) ---- */
const float BAR_H = 64, FOOT_Y = 440;
const float X0 = 24;                                   /* the rows' left edge */
const float P_W = 96, P_H = 144;                       /* home rows: posters */
const float L_W = 208, L_H = 117;                      /* ... libraries */
const float TILE_GAP = 18, ROW_TITLE = 38, ROW_GAP = 36;
const int   G_COLS = 6;
const float G_W = 112, G_H = 168, G_GAP = 18, G_ROW = G_H + 44;   /* grids */
const float CHIP_H = 36, EP_ROW = 84;                  /* series */
const float TR_ROW = 44;                               /* an album's tracks */

/* ---- the TV's (its units) ---- */
const float TV_X = 72;                                 /* the text's left edge */
const float CAR_Y = 452;                               /* the row of tiles: its title */

std::string fmtTime(long long ticks)
{
    const long long s = ticks / 10000000LL;
    char b[24];
    if (s >= 3600) snprintf(b, sizeof(b), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else           snprintf(b, sizeof(b), "%lld:%02lld", s / 60, s % 60);
    return b;
}

std::string runtimeText(long long ticks)
{
    const long long m = ticks / 600000000LL;
    char b[24];
    if (m >= 60) snprintf(b, sizeof(b), "%lldh %02lldmin", m / 60, m % 60);
    else if (m > 0) snprintf(b, sizeof(b), "%lld min", m);
    else b[0] = 0;
    return b;
}

bool isVideo(const std::string& t)
{
    return t == "Movie" || t == "Episode" || t == "Video" || t == "MusicVideo" || t == "Trailer";
}

bool isSquare(const JellyfinItem& it)
{
    return it.type == "MusicAlbum" || it.type == "Audio" || it.type == "MusicArtist" || it.type == "Playlist";
}

/* the picture a tile shows: an episode, its series' poster */
JellyfinItem posterOf(const JellyfinItem& it)
{
    if (it.type == "Episode" && !it.seriesId.empty()) {
        JellyfinItem s;
        s.id = it.seriesId;
        s.type = "Series";
        return s;
    }
    return it;
}

std::string captionOf(const JellyfinItem& it)
{
    if (it.type == "Episode") {
        char b[200];
        snprintf(b, sizeof(b), "S%d E%d \xC2\xB7 %s", it.seasonNumber, it.episodeNumber,
                 (it.seriesName.empty() ? it.name : it.seriesName).c_str());
        return b;
    }
    return it.name;
}

void badges(const JellyfinItem& it, float x, float y, float w, float h)
{
    const Ui::Palette& p = Ui::pal();
    if (it.runtimeTicks > 0 && it.playbackPositionTicks > 0)
        progress(x + 6, y + h - 10, w - 12, (float)it.playbackPositionTicks / it.runtimeTicks);
    if (it.played) {
        const float r = w > 150 ? 13 : 10;
        Ui::circle(x + w - r - 5, y + r + 5, r, p.accent);
        icon(Icon::Check, x + w - r - 5, y + r + 5, r * 1.3f, p.textOnAccent);
    }
}

/* a tile: the item's picture (or its title on a card while it loads),
 * in the Wii U's frame, lit when focused (its size stays: nothing drawn
 * over its neighbours) */
void tile(const JellyfinItem& it, float x, float y, float w, float h, bool focus, Kind kind = Kind::Poster)
{
    const Ui::Palette& p = Ui::pal();
    const float r = w > 150 ? 10 : 8;
    GRRLIB_texImg* tex = ImagesWiiU::get(kind == Kind::Thumb ? it : posterOf(it), kind);
    tileFrame(x, y, w, h, r, focus ? 1.0f : 0.0f);
    if (tex) image(tex, x, y, w, h, r);
    else {
        Ui::roundRect(x, y, w, h, r, p.cardTop);
        const int size = w > 150 ? 18 : 13;
        paragraph(x + 8, y + 8, w - 16, captionOf(it), size, p.textDim, (int)((h - 16) / (size * 1.3f)));
    }
    badges(it, x, y, w, h);
}

/* a line of text centred under a tile, kept inside the screen (width sw) */
void caption(float cx, float y, const std::string& s, int size, u32 color, float maxW, float sw)
{
    const std::string t = ellipsize(s, size, maxW);
    const float w = textW(t, size);
    float x = cx - w / 2;
    if (x + w > sw - 8) x = sw - 8 - w;
    if (x < 8) x = 8;
    text(x, y, t, size, color);
}

/* the axis a drag goes along, decided as it starts */
enum class Axis { None, V, H };
Axis s_axis = Axis::None;
void dragAxis(const Touch& t)
{
    if (!t.down) { s_axis = Axis::None; return; }
    if (s_axis == Axis::None && t.dragging)
        s_axis = fabsf(t.x - t.startX) > fabsf(t.y - t.startY) ? Axis::H : Axis::V;
}

void scroll(Scroller& s, const Touch& t, bool inside, Axis axis)
{
    Touch tt = t;
    if (s_axis != axis || !inside) { tt.dragging = false; tt.pressed = false; }
    if (s_axis == axis && inside && t.dragging && !s.grabbed) { s.grabbed = true; s.target = -1; }
    s.update(tt, false);
    if (!t.down) s.grabbed = false;
}

/* a home row's tiles */
float rowTileW(bool libs) { return libs ? L_W : P_W; }
float rowTileH(bool libs) { return libs ? L_H : P_H; }

/* the bar's buttons: the home's, the other pages' */
const Icon HOME_ICONS[3] = { Icon::Search, Icon::Gear, Icon::Person };
const Icon PAGE_ICONS[2] = { Icon::Search, Icon::Home };
float barButtonX(int i, int n) { return PW - 40 - (n - 1 - i) * 58; }

} // namespace

/* ---- life ------------------------------------------------------------------ */

BrowseWiiU::BrowseWiiU(JellyfinClient& c, const JellyfinAuth& a, const std::string& url, const std::string& user)
    : client(c), auth(a), serverUrl(url), userName(user)
{
    static std::vector<std::shared_ptr<Job>> orphans;
    s_orphans = &orphans;
    ImagesWiiU::start(client, auth, serverUrl);
    auto home = std::make_unique<Page>();
    home->type = Type::Home;
    push(std::move(home));
}

BrowseWiiU::~BrowseWiiU()
{
    stack.clear();
    if (detailJob) detailJob.reset();
    if (s_orphans) s_orphans->clear();
    ImagesWiiU::stop();
}

void BrowseWiiU::push(std::unique_ptr<Page> p)
{
    stack.push_back(std::move(p));
    load(*stack.back());
}

void BrowseWiiU::pop()
{
    if (stack.size() <= 1) return;
    for (std::shared_ptr<Job>* j : { &stack.back()->job, &stack.back()->job2 })
        if (*j && (*j)->running()) {
            (*j)->apply = nullptr;
            s_orphans->push_back(*j);
        }
    stack.pop_back();
    SoundFX::play(SoundFX::FX::Back);
}

void BrowseWiiU::notice(const std::string& s)
{
    message = s;
    messageUntil = frames + 150;
}

/* ---- loading (as the 3DS's) ---------------------------------------------------- */

void BrowseWiiU::load(Page& p)
{
    p.loading = true;
    Page* pp = &p;
    JellyfinClient* cl = &client;
    const JellyfinAuth au = auth;
    const std::string url = serverUrl;
    switch (p.type) {
    case Type::Home: {
        /* the page as soon as its first rows are there; the libraries'
         * latest ones (a request each) come after */
        auto rows = std::make_shared<std::vector<Row>>();
        auto libs = std::make_shared<std::vector<JellyfinLibrary>>();
        const int gen = ++p.gen;
        p.job = runJob([=]() {
            Row cw; cw.title = tr("Continue Watching");
            cl->getContinueWatching(url, au, cw.items);
            if (!cw.items.empty()) rows->push_back(std::move(cw));
            Row nu; nu.title = tr("Next Up");
            cl->getNextUp(url, au, nu.items);
            if (!nu.items.empty()) rows->push_back(std::move(nu));
            cl->getLibraries(url, au, *libs);
            Row media; media.title = tr("My Media"); media.libraries = true;
            for (const JellyfinLibrary& l : *libs) {
                JellyfinItem it;
                it.id = l.id; it.name = l.name; it.type = "CollectionFolder"; it.extraType = l.collectionType;
                media.items.push_back(it);
            }
            if (!media.items.empty()) rows->push_back(std::move(media));
        }, [=]() {
            if (gen != pp->gen) return;
            const std::string keep = pp->row < (int)pp->rows.size() ? pp->rows[pp->row].title : "";
            std::vector<Row> latest;
            for (Row& r : pp->rows) if (r.latest) latest.push_back(std::move(r));
            pp->rows = std::move(*rows);
            for (Row& r : latest) pp->rows.push_back(std::move(r));
            for (int i = 0; i < (int)pp->rows.size(); ++i)
                if (pp->rows[i].title == keep) pp->row = i;
            if (pp->row >= (int)pp->rows.size()) pp->row = 0;
            for (Row& r : pp->rows) r.hs.horizontal = true;
            pp->loading = false;

            auto more = std::make_shared<std::vector<Row>>();
            if (pp->job2 && pp->job2->running()) {
                pp->job2->apply = nullptr;
                s_orphans->push_back(pp->job2);
            }
            pp->job2 = runJob([=]() {
                for (const JellyfinLibrary& l : *libs) {
                    if (l.collectionType != "movies" && l.collectionType != "tvshows" && l.collectionType != "music") continue;
                    Row r;
                    char title[160];
                    /* TRANSLATORS: a row of the home page; %s is the name of a library */
                    snprintf(title, sizeof(title), tr("Latest in %s"), l.name.c_str());
                    r.title = title;
                    r.latest = true;
                    char q[256];
                    snprintf(q, sizeof(q), "/Users/%s/Items/Latest?ParentId=%s&Limit=20", au.userId.c_str(), l.id.c_str());
                    cl->getItemsByQuery(url, au, q, r.items);
                    if (!r.items.empty()) more->push_back(std::move(r));
                }
            }, [=]() {
                if (gen != pp->gen) return;
                const std::string keep = pp->row < (int)pp->rows.size() ? pp->rows[pp->row].title : "";
                std::vector<Row> kept;
                for (Row& r : pp->rows) if (!r.latest) kept.push_back(std::move(r));
                for (Row& r : *more) { r.hs.horizontal = true; kept.push_back(std::move(r)); }
                pp->rows = std::move(kept);
                for (int i = 0; i < (int)pp->rows.size(); ++i)
                    if (pp->rows[i].title == keep) pp->row = i;
                if (pp->row >= (int)pp->rows.size()) pp->row = 0;
            });
        });
        break;
    }
    case Type::Grid: {
        auto items = std::make_shared<std::vector<JellyfinItem>>();
        auto total = std::make_shared<int>(0);
        const JellyfinItem lib = p.item;
        p.job = runJob([=]() {
            std::string extra = "&SortBy=SortName";
            if (lib.extraType == "music") extra = "&IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=AlbumArtist,SortName";
            cl->getItems(url, au, lib.id, 0, 90, *items, *total, extra);
        }, [=]() {
            pp->items = std::move(*items);
            pp->total = *total;
            pp->more = (int)pp->items.size() < pp->total;
            pp->loading = false;
        });
        break;
    }
    case Type::Search: {
        auto items = std::make_shared<std::vector<JellyfinItem>>();
        const std::string term = p.title;
        p.job = runJob([=]() { cl->searchItems(url, au, term, 60, *items); },
                       [=]() { pp->items = std::move(*items); pp->total = (int)pp->items.size(); pp->loading = false; });
        break;
    }
    case Type::Series: {
        auto seasons = std::make_shared<std::vector<JellyfinSeason>>();
        auto eps = std::make_shared<std::vector<JellyfinEpisode>>();
        const std::string sid = p.item.type == "Series" ? p.item.id : p.item.seriesId;
        const int wantSeason = p.season;
        p.job = runJob([=]() {
            cl->getSeasons(url, au, sid, *seasons);
            const int s = wantSeason < (int)seasons->size() ? wantSeason : 0;
            if (!seasons->empty()) cl->getEpisodes(url, au, sid, (*seasons)[s].id, *eps);
        }, [=]() {
            pp->seasons = std::move(*seasons);
            if (pp->season >= (int)pp->seasons.size()) pp->season = 0;
            pp->episodes = std::move(*eps);
            pp->sel = 0;
            for (int i = 0; i < (int)pp->episodes.size(); ++i)
                if (!pp->episodes[i].played) { pp->sel = i; break; }
            pp->vs.pos = 0;
            pp->vs.show(pp->sel * EP_ROW, pp->sel * EP_ROW + EP_ROW, FOOT_Y - (BAR_H + CHIP_H + 18));
            pp->loading = false;
        });
        break;
    }
    case Type::Detail: {
        auto d = std::make_shared<JellyfinItemDetail>();
        const std::string id = p.item.id;
        p.job = runJob([=]() { cl->getItemDetail(url, au, id, *d); }, [=]() {
            pp->detail = *d;
            pp->hasDetail = !d->id.empty() || !d->name.empty();
            pp->audio = 0;
            pp->sub = -1;
            for (int i = 0; i < (int)d->audioStreams.size(); ++i)
                if (d->audioStreams[i].index == d->defaultAudioIndex) pp->audio = i;
            for (int i = 0; i < (int)d->subtitleStreams.size(); ++i)
                if (d->subtitleStreams[i].index == d->defaultSubIndex) pp->sub = i;
            details[id] = *d;
            pp->loading = false;
        });
        break;
    }
    case Type::Tracks: {
        auto tr_ = std::make_shared<std::vector<JellyfinAudioItem>>();
        const std::string id = p.item.id;
        p.job = runJob([=]() { cl->getAlbumTracks(url, au, id, *tr_); },
                       [=]() { pp->tracks = std::move(*tr_); pp->loading = false; });
        break;
    }
    }
}

void BrowseWiiU::loadMore(Page& p)
{
    if (!p.more || (p.job && p.job->running())) return;
    Page* pp = &p;
    JellyfinClient* cl = &client;
    const JellyfinAuth au = auth;
    const std::string url = serverUrl;
    const JellyfinItem lib = p.item;
    const int start = (int)p.items.size();
    auto items = std::make_shared<std::vector<JellyfinItem>>();
    auto total = std::make_shared<int>(0);
    p.job = runJob([=]() {
        std::string extra = "&SortBy=SortName";
        if (lib.extraType == "music") extra = "&IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=AlbumArtist,SortName";
        cl->getItems(url, au, lib.id, start, 90, *items, *total, extra);
    }, [=]() {
        pp->items.insert(pp->items.end(), items->begin(), items->end());
        pp->more = !items->empty() && (int)pp->items.size() < *total;
    });
}

/* ---- opening what was chosen ------------------------------------------------- */

void BrowseWiiU::open(const JellyfinItem& it)
{
    auto p = std::make_unique<Page>();
    p->item = it;
    p->title = it.name;
    const std::string& t = it.type;
    if (t == "Series" || t == "Season") p->type = Type::Series;
    else if (isVideo(t)) p->type = Type::Detail;
    else if (t == "MusicAlbum") p->type = Type::Tracks;
    else if (t == "Audio") {
        MusicTrack m;
        m.id = it.id; m.title = it.name; m.artist = it.artist; m.runtimeTicks = it.runtimeTicks;
        pendingMusicTracks = { m };
        pendingMusicTrackIdx = 0;
        pendingMusicShuffle = false;
        pendingPlayIsMusic = true;
        return;
    } else p->type = Type::Grid;
    SoundFX::play(SoundFX::FX::Open);
    push(std::move(p));
}

/* ---- playing (as the 3DS's) ---------------------------------------------------- */

void BrowseWiiU::play(Page& p, bool fromStart)
{
    if (!p.hasDetail) return;
    const JellyfinItemDetail& d = p.detail;
    const int audioIdx = d.audioStreams.empty() ? 0 : d.audioStreams[p.audio].index;
    const int subIdx = p.sub >= 0 && p.sub < (int)d.subtitleStreams.size() ? d.subtitleStreams[p.sub].index : -1;
    int burnIdx = subIdx;
    for (const MediaStream& s : d.subtitleStreams)
        if (s.index == subIdx && s.isText) burnIdx = -1;
    const long long startTicks = fromStart ? 0 : d.playbackPositionTicks;
    const std::string sourceId = d.versions.empty() ? p.item.id : d.versions[0].id;
    SoundFX::play(SoundFX::FX::Play);
    std::string url, session;
    pendingPlayHow = JellyfinClient::PlaybackChoice();
    if (!client.getPlaybackUrl(serverUrl, auth, p.item.id, sourceId, audioIdx, burnIdx, startTicks,
                               url, session, pendingPlayHow)) {
        SYS_Report("[BrowseWiiU] no playback URL: %s\n", client.lastError().c_str());
        notice(tr("The server didn't answer: try again"));
        return;
    }
    pendingPlayUrl = url;
    pendingPlayTitle = d.name;
    pendingPlayItemId = p.item.id;
    pendingPlayMediaSourceId = sourceId;
    pendingPlaySessionId = session;
    pendingPlayStartTimeTicks = startTicks;
    pendingPlayRuntimeTicks = d.runtimeTicks;
    pendingPlayAudioStreams = d.audioStreams;
    pendingPlaySubStreams = d.subtitleStreams;
    pendingPlayAudioIdx = audioIdx;
    pendingPlaySubIdx = subIdx;
    pendingPlayEpisodes.clear();
    pendingPlayEpisodeIdx = 0;
    pendingPlaySeriesId.clear();
    /* an episode: previous / next through its series */
    if (!d.seriesId.empty()) {
        std::vector<JellyfinEpisode> eps;
        if (client.getSeriesEpisodes(serverUrl, auth, d.seriesId, eps)) {
            int idx = -1;
            for (int i = 0; i < (int)eps.size(); ++i) if (eps[i].id == p.item.id) idx = i;
            if (idx >= 0) {
                const bool special = eps[idx].seasonNumber == 0;
                eps.erase(std::remove_if(eps.begin(), eps.end(),
                          [&](const JellyfinEpisode& e) { return (e.seasonNumber == 0) != special; }), eps.end());
                for (int i = 0; i < (int)eps.size(); ++i) if (eps[i].id == p.item.id) idx = i;
                pendingPlayEpisodes = eps;
                pendingPlayEpisodeIdx = idx;
                pendingPlaySeriesId = d.seriesId;
            }
        }
    }
}

void BrowseWiiU::playTracks(Page& p, int from, bool shuffle)
{
    if (p.tracks.empty()) return;
    pendingMusicTracks.clear();
    for (const JellyfinAudioItem& a : p.tracks) {
        MusicTrack m;
        m.id = a.id; m.title = a.name; m.artist = a.artist; m.album = a.album; m.runtimeTicks = a.runtimeTicks;
        pendingMusicTracks.push_back(m);
    }
    pendingMusicTrackIdx = from;
    pendingMusicShuffle = shuffle;
    pendingPlayIsMusic = true;
}

void BrowseWiiU::onPlaybackFinished()
{
    pendingPlayUrl.clear();
    pendingPlayIsMusic = false;
    exitTo = Exit::None;
    for (auto& p : stack)
        if (p->type == Type::Home || p->type == Type::Detail || p->type == Type::Series) {
            if (p->job && p->job->running()) continue;
            if (p->type == Type::Detail) details.erase(p->item.id);
            load(*p);
        }
}

/* ---- search: the system's keyboard, a frame at a time ------------------------- */

void BrowseWiiU::searchFrame()
{
    switch (SwkbdWiiU::edit(searchText, tr("Search"), false, 120)) {
    case SwkbdWiiU::Result::Busy:
        return;
    case SwkbdWiiU::Result::Ok: {
        searching = false;
        std::string t = searchText;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (t.empty()) return;
        auto p = std::make_unique<Page>();
        p->type = Type::Search;
        p->title = t;
        SoundFX::play(SoundFX::FX::Open);
        push(std::move(p));
        return;
    }
    case SwkbdWiiU::Result::Cancel:
        searching = false;
        return;
    case SwkbdWiiU::Result::Unavailable:
        searching = false;
        return;
    }
}

/* ---- the pages: input (the GamePad's layout) ----------------------------------- */

void BrowseWiiU::updateHome(Page& p, bool a)
{
    if (p.rows.empty()) return;
    const float y0 = BAR_H + 4, view = FOOT_Y - y0;
    std::vector<float> rowY;
    float y = 0;
    for (const Row& r : p.rows) { rowY.push_back(y); y += ROW_TITLE + rowTileH(r.libraries) + ROW_GAP; }
    p.vs.maxPos = y - view;
    const bool inBody = touch.startY >= y0 && touch.startY < FOOT_Y;
    scroll(p.vs, touch, inBody, Axis::V);
    for (int r = 0; r < (int)p.rows.size(); ++r) {
        Row& row = p.rows[r];
        const float cw = rowTileW(row.libraries) + TILE_GAP;
        row.hs.maxPos = X0 * 2 + row.items.size() * cw - TILE_GAP - PW;
        const float ry = y0 + rowY[r] - p.vs.pos;
        const float rh = ROW_TITLE + rowTileH(row.libraries) + ROW_GAP;
        scroll(row.hs, touch, inBody && touch.startY >= ry && touch.startY < ry + rh, Axis::H);
    }
    /* D-pad: the bar above the first row */
    if (p.bar >= 0) {
        if (Input::isLeftPressed() && p.bar > 0) { --p.bar; SoundFX::play(SoundFX::FX::Move); }
        if (Input::isRightPressed() && p.bar < 2) { ++p.bar; SoundFX::play(SoundFX::FX::Move); }
        if (Input::isDownPressed()) { p.bar = -1; SoundFX::play(SoundFX::FX::Move); }
        return;   /* A: frame() acts on the bar */
    }
    Row& cur = p.rows[p.row];
    bool moved = false;
    if (Input::isUpPressed()) {
        if (p.row > 0) { --p.row; moved = true; }
        else { p.bar = 0; SoundFX::play(SoundFX::FX::Move); return; }
    }
    if (Input::isDownPressed() && p.row + 1 < (int)p.rows.size()) { ++p.row; moved = true; }
    if (Input::isLeftPressed() && cur.sel > 0) { --cur.sel; moved = true; }
    if (Input::isRightPressed() && cur.sel + 1 < (int)cur.items.size()) { ++cur.sel; moved = true; }
    if (moved) {
        SoundFX::play(SoundFX::FX::Move);
        Row& r = p.rows[p.row];
        if (r.sel >= (int)r.items.size()) r.sel = (int)r.items.size() - 1;
        if (r.sel < 0) r.sel = 0;
        p.vs.show(rowY[p.row], rowY[p.row] + ROW_TITLE + rowTileH(r.libraries) + ROW_GAP, view);
        const float cw = rowTileW(r.libraries) + TILE_GAP;
        r.hs.show(r.sel * cw, r.sel * cw + cw + X0 * 2, PW);
    }
    /* taps */
    for (int r = 0; r < (int)p.rows.size(); ++r) {
        Row& row = p.rows[r];
        const float ry = y0 + rowY[r] - p.vs.pos + ROW_TITLE;
        const float w = rowTileW(row.libraries), h = rowTileH(row.libraries);
        for (int i = 0; i < (int)row.items.size(); ++i) {
            const float x = X0 + i * (w + TILE_GAP) - row.hs.pos;
            if (touch.tap && touch.y >= y0 && touch.y < FOOT_Y && hit(touch, x, ry, w, h)) {
                p.row = r; row.sel = i;
                open(row.items[i]);
                return;
            }
        }
    }
    if (a && cur.sel < (int)cur.items.size()) open(cur.items[cur.sel]);
}

void BrowseWiiU::updateGrid(Page& p, bool a)
{
    const float y0 = BAR_H + 8, view = FOOT_Y - y0;
    const int n = (int)p.items.size();
    const int rows = (n + G_COLS - 1) / G_COLS;
    p.vs.maxPos = rows * G_ROW - view + 6;
    scroll(p.vs, touch, touch.startY >= y0 && touch.startY < FOOT_Y, Axis::V);
    int sel = p.sel;
    if (Input::isLeftPressed() && sel > 0) --sel;
    if (Input::isRightPressed() && sel + 1 < n) ++sel;
    if (Input::isUpPressed() && sel >= G_COLS) sel -= G_COLS;
    if (Input::isDownPressed() && n > 0) sel = std::min(n - 1, sel + G_COLS);
    if (Input::isLPressed()) sel = std::max(0, sel - G_COLS * 2);
    if (Input::isRPressed() && n > 0) sel = std::min(n - 1, sel + G_COLS * 2);
    if (sel != p.sel && sel >= 0) {
        p.sel = sel;
        SoundFX::play(SoundFX::FX::Move);
        const float ry = (sel / G_COLS) * G_ROW;
        p.vs.show(ry, ry + G_ROW, view);
    }
    if (p.more && (p.vs.pos + view) / G_ROW + 3 >= rows) loadMore(p);
    const float x0 = (PW - (G_COLS * G_W + (G_COLS - 1) * G_GAP)) * 0.5f;
    for (int i = 0; i < n; ++i) {
        const float x = x0 + (i % G_COLS) * (G_W + G_GAP), y = y0 + 6 + (i / G_COLS) * G_ROW - p.vs.pos;
        if (touch.y >= y0 && touch.y < FOOT_Y && hit(touch, x, y, G_W, G_H)) { p.sel = i; open(p.items[i]); return; }
    }
    if (a && p.sel < n) open(p.items[p.sel]);
}

void BrowseWiiU::updateSeries(Page& p, bool a)
{
    const float chipsY = BAR_H + 8, listY = BAR_H + CHIP_H + 18;
    float cx = X0;
    std::vector<float> chipX;
    for (const JellyfinSeason& s : p.seasons) { chipX.push_back(cx); cx += textW(s.name, 16) + 40; }
    p.ss.horizontal = true;
    p.ss.maxPos = cx - PW + X0;
    scroll(p.ss, touch, touch.startY >= chipsY && touch.startY < listY, Axis::H);
    int season = p.season;
    for (int i = 0; i < (int)p.seasons.size(); ++i)
        if (hit(touch, chipX[i] - p.ss.pos, chipsY, textW(p.seasons[i].name, 16) + 32, CHIP_H)) season = i;
    if (Input::isLPressed() && season > 0) --season;
    if (Input::isRPressed() && season + 1 < (int)p.seasons.size()) ++season;
    if (season != p.season) {
        p.season = season;
        p.ss.show(chipX[season], chipX[season] + textW(p.seasons[season].name, 16) + 40, PW - X0);
        SoundFX::play(SoundFX::FX::Page);
        load(p);
        return;
    }
    const int n = (int)p.episodes.size();
    const float view = FOOT_Y - listY;
    p.vs.maxPos = n * EP_ROW - view;
    scroll(p.vs, touch, touch.startY >= listY && touch.startY < FOOT_Y, Axis::V);
    int sel = p.sel;
    if (Input::isUpPressed() && sel > 0) --sel;
    if (Input::isDownPressed() && sel + 1 < n) ++sel;
    if (sel != p.sel) { p.sel = sel; SoundFX::play(SoundFX::FX::Move); p.vs.show(sel * EP_ROW, sel * EP_ROW + EP_ROW, view); }
    int open_ = -1;
    for (int i = 0; i < n; ++i) {
        const float y = listY + i * EP_ROW - p.vs.pos;
        if (touch.y >= listY && touch.y < FOOT_Y && hit(touch, 0, y, PW, EP_ROW)) open_ = i;
    }
    if (a && p.sel < n) open_ = p.sel;
    if (open_ >= 0) {
        const JellyfinEpisode& e = p.episodes[open_];
        p.sel = open_;
        JellyfinItem it;
        it.id = e.id; it.name = e.name; it.type = "Episode";
        it.seasonNumber = e.seasonNumber; it.episodeNumber = e.indexNumber;
        it.seriesName = p.item.type == "Series" ? p.item.name : p.item.seriesName;
        it.seriesId = p.item.type == "Series" ? p.item.id : p.item.seriesId;
        it.playbackPositionTicks = e.playbackPositionTicks;
        it.played = e.played;
        open(it);
    }
}

/* the detail page's controls, top to bottom */
namespace { enum Ctl { C_PLAY, C_START, C_AUDIO, C_SUB, C_ACTIONS }; }

static std::vector<int> detailControls(const JellyfinItemDetail& d)
{
    std::vector<int> ctls = { C_PLAY };
    if (d.playbackPositionTicks > 0) ctls.push_back(C_START);
    if (d.audioStreams.size() > 1) ctls.push_back(C_AUDIO);
    if (!d.subtitleStreams.empty()) ctls.push_back(C_SUB);
    ctls.push_back(C_ACTIONS);
    return ctls;
}

/* the detail page's layout: its column on the right, where the controls start */
static const float D_X = 230, D_W = PW - 230 - 24;
static float detailTop(bool dual) { return dual ? BAR_H + 86 : BAR_H + 150; }
static float ctlH(int c) { return c == C_PLAY ? 46 : c == C_ACTIONS ? 40 : 38; }

void BrowseWiiU::updateDetail(Page& p, bool a)
{
    if (!p.hasDetail) return;
    JellyfinItemDetail& d = p.detail;
    const std::vector<int> ctls = detailControls(d);
    if (p.focus >= (int)ctls.size()) p.focus = 0;
    if (Input::isUpPressed() && p.focus > 0) { --p.focus; SoundFX::play(SoundFX::FX::Move); }
    if (Input::isDownPressed() && p.focus + 1 < (int)ctls.size()) { ++p.focus; SoundFX::play(SoundFX::FX::Move); }
    const int c = ctls[p.focus];
    auto cycleAudio = [&](int dir) { if (!d.audioStreams.empty()) p.audio = (p.audio + dir + (int)d.audioStreams.size()) % (int)d.audioStreams.size(); };
    auto cycleSub = [&](int dir) {
        const int n = (int)d.subtitleStreams.size() + 1;
        p.sub = (p.sub + 1 + dir + n) % n - 1;
    };
    const int nActions = d.seriesId.empty() ? 2 : 3;
    if (c == C_AUDIO && Input::isLeftPressed()) cycleAudio(-1);
    if (c == C_AUDIO && Input::isRightPressed()) cycleAudio(1);
    if (c == C_SUB && Input::isLeftPressed()) cycleSub(-1);
    if (c == C_SUB && Input::isRightPressed()) cycleSub(1);
    if (c == C_ACTIONS && Input::isLeftPressed() && p.action > 0) { --p.action; SoundFX::play(SoundFX::FX::Move); }
    if (c == C_ACTIONS && Input::isRightPressed() && p.action + 1 < nActions) { ++p.action; SoundFX::play(SoundFX::FX::Move); }

    auto act = [&](int which) {
        if (which == 0) {
            d.isFavorite = !d.isFavorite;
            client.setFavorite(serverUrl, auth, p.item.id, d.isFavorite);
            SoundFX::play(SoundFX::FX::Start);
        } else if (which == 1) {
            d.played = !d.played;
            client.setPlayed(serverUrl, auth, p.item.id, d.played);
            if (d.played) d.playbackPositionTicks = 0;
            SoundFX::play(SoundFX::FX::Start);
        } else if (which == 2 && !d.seriesId.empty()) {
            JellyfinItem s;
            s.id = d.seriesId; s.name = d.seriesName; s.type = "Series";
            open(s);
        }
    };
    float y = detailTop(dual);
    for (int i = 0; i < (int)ctls.size(); ++i) {
        const int ctl = ctls[i];
        const float h = ctlH(ctl);
        if (ctl == C_ACTIONS) {
            const float w = (D_W - 10 * (nActions - 1)) / nActions;
            for (int k = 0; k < nActions; ++k)
                if (hit(touch, D_X + k * (w + 10), y, w, h)) { p.focus = i; p.action = k; act(k); return; }
        } else if (hit(touch, D_X, y, D_W, h)) {
            p.focus = i;
            if (ctl == C_PLAY) { play(p, false); return; }
            if (ctl == C_START) { play(p, true); return; }
            if (ctl == C_AUDIO) cycleAudio(touch.x > D_X + D_W / 2 ? 1 : -1);
            if (ctl == C_SUB) cycleSub(touch.x > D_X + D_W / 2 ? 1 : -1);
        }
        y += h + 7;
    }
    if (a) {
        if (c == C_PLAY) play(p, false);
        else if (c == C_START) play(p, true);
        else if (c == C_AUDIO) cycleAudio(1);
        else if (c == C_SUB) cycleSub(1);
        else act(p.action);
    }
}

void BrowseWiiU::updateTracks(Page& p, bool a)
{
    const float listY = BAR_H + 70;
    const float bx = dual ? X0 : X0 + 70;
    const int n = (int)p.tracks.size();
    const float view = FOOT_Y - listY;
    p.vs.maxPos = n * TR_ROW - view;
    scroll(p.vs, touch, touch.startY >= listY && touch.startY < FOOT_Y, Axis::V);
    if (hit(touch, bx, BAR_H + 12, 180, 44)) { playTracks(p, 0, false); return; }
    if (hit(touch, bx + 192, BAR_H + 12, 180, 44)) { playTracks(p, 0, true); return; }
    int sel = p.sel;
    if (Input::isUpPressed() && sel > 0) --sel;
    if (Input::isDownPressed() && sel + 1 < n) ++sel;
    if (sel != p.sel) { p.sel = sel; SoundFX::play(SoundFX::FX::Move); p.vs.show(sel * TR_ROW, sel * TR_ROW + TR_ROW, view); }
    for (int i = 0; i < n; ++i)
        if (touch.y >= listY && touch.y < FOOT_Y && hit(touch, 0, listY + i * TR_ROW - p.vs.pos, PW, TR_ROW)) {
            playTracks(p, i, false);
            return;
        }
    if (Input::is2Pressed()) { playTracks(p, 0, true); return; }
    if (a && n) playTracks(p, p.sel, false);
}

/* ---- the pages: the GamePad's drawing ----------------------------------------------- */

int BrowseWiiU::drawBar(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    /* a light band, its line under it */
    rect(0, 0, PW, BAR_H, Ui::alpha(pal.barTop, 0.95f));
    rect(0, BAR_H - 2, PW, 2, pal.barBorder);
    const bool home = p.type == Type::Home;
    int tapped = -2;
    float x = X0;
    if (!home) {
        roundButton(40, BAR_H / 2, 22, Icon::Back, 0);
        if (hit(touch, 0, 0, 76, BAR_H + 4)) tapped = -1;
        x = 76;
    }
    const int n = home ? 3 : 2;
    const Icon* icons = home ? HOME_ICONS : PAGE_ICONS;
    const float right = barButtonX(0, n) - 30;
    std::string title = home ? "" : p.type == Type::Search ? std::string(tr("Search")) + ": " + p.title
                                  : p.type == Type::Detail && p.hasDetail
                                        ? (p.detail.seriesName.empty() ? p.detail.name : p.detail.seriesName)
                                        : p.title;
    if (home) {
        logo(x, BAR_H / 2, 36);
        if (!userName.empty()) textR(right - 8, (BAR_H - 18) / 2 - 1, ellipsize(userName, 18, 220), 18, pal.textDim);
    } else {
        text(x, (BAR_H - 24) / 2 - 2, title, 24, pal.text, right - x - 8);
    }
    for (int i = 0; i < n; ++i) {
        const float cx = barButtonX(i, n);
        roundButton(cx, BAR_H / 2, 22, icons[i], home && p.bar == i ? 1.0f : 0.0f);
        if (hit(touch, cx - 29, 0, 58, BAR_H + 4)) tapped = i;
    }
    return tapped;
}

void BrowseWiiU::drawFooter(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    rect(0, FOOT_Y, PW, PH - FOOT_Y, Ui::alpha(pal.barTop, 0.95f));
    rect(0, FOOT_Y, PW, 2, pal.barBorder);
    Hint h[4];
    int n = 0;
    switch (p.type) {
    case Type::Home:   h[n++] = { "A", tr("Open") }; h[n++] = { "1", tr("Search") }; break;
    case Type::Grid:
    case Type::Search: h[n++] = { "A", tr("Open") }; h[n++] = { "-/+", tr("Page") }; break;
    case Type::Series: h[n++] = { "A", tr("Open") }; h[n++] = { "-/+", tr("Seasons") }; break;
    case Type::Detail: h[n++] = { "A", tr("Choose") }; break;
    case Type::Tracks: h[n++] = { "A", tr("Play") }; h[n++] = { "2", tr("Shuffle") }; break;
    }
    if (p.type != Type::Home) h[n++] = { "B", tr("Back") };
    hints(X0 - 8, FOOT_Y + 8, h, n, 16, pal.text);
    textR(PW - X0 + 4, FOOT_Y + 10, clockText(), 18, pal.textDim);
}

void BrowseWiiU::drawHome(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const float y0 = BAR_H + 4;
    clip(0, y0, PW, FOOT_Y - y0);
    float y = y0 - p.vs.pos;
    for (int r = 0; r < (int)p.rows.size(); ++r) {
        Row& row = p.rows[r];
        const float h = rowTileH(row.libraries), w = rowTileW(row.libraries);
        const bool focusRow = r == p.row && p.bar < 0;
        if (y + ROW_TITLE + h + ROW_GAP > y0 && y < FOOT_Y) {
            text(X0, y + 6, row.title, 18, focusRow ? pal.text : pal.textDim, PW - 2 * X0);
            for (int i = 0; i < (int)row.items.size(); ++i) {
                const float x = X0 + i * (w + TILE_GAP) - row.hs.pos;
                if (x + w < -20 || x > PW + 20) continue;
                const bool focus = focusRow && i == row.sel;
                const JellyfinItem& it = row.items[i];
                if (row.libraries) {
                    tile(it, x, y + ROW_TITLE, w, h, focus, Kind::Thumb);
                    caption(x + w / 2, y + ROW_TITLE + h + 14, it.name, 14, focus ? pal.text : pal.textDim, w, PW);
                } else if (isSquare(it)) {
                    tile(it, x, y + ROW_TITLE + h - w, w, w, focus);
                } else {
                    tile(it, x, y + ROW_TITLE, w, h, focus);
                }
                if (focus && !row.libraries)
                    caption(x + w / 2, y + ROW_TITLE + h + 14, captionOf(it), 13, pal.text, w + 80, PW);
            }
        }
        y += ROW_TITLE + h + ROW_GAP;
    }
    clipOff();
    if (p.rows.empty() && !p.loading) textC(PW / 2, PH / 2 - 10, tr("Nothing here yet"), 18, pal.textDim);
}

void BrowseWiiU::drawGrid(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const float y0 = BAR_H + 8;
    const float x0 = (PW - (G_COLS * G_W + (G_COLS - 1) * G_GAP)) * 0.5f;
    clip(0, BAR_H, PW, FOOT_Y - BAR_H);
    for (int i = 0; i < (int)p.items.size(); ++i) {
        const float x = x0 + (i % G_COLS) * (G_W + G_GAP), y = y0 + 6 + (i / G_COLS) * G_ROW - p.vs.pos;
        if (y + G_ROW < BAR_H || y > FOOT_Y) continue;
        const JellyfinItem& it = p.items[i];
        const bool sq = isSquare(it);
        const float th = sq ? G_W : G_H, ty = y + (G_H - th);
        tile(it, x, ty, G_W, th, i == p.sel);
        caption(x + G_W / 2, y + G_H + 15, captionOf(it), 13, i == p.sel ? pal.text : pal.textDim, G_W + G_GAP - 4, PW);
    }
    clipOff();
    if (p.items.empty() && !p.loading)
        textC(PW / 2, PH / 2 - 10, p.type == Type::Search ? tr("No results") : tr("Nothing here yet"), 18, pal.textDim);
}

void BrowseWiiU::drawSeries(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const float chipsY = BAR_H + 8, listY = BAR_H + CHIP_H + 18;
    clip(0, chipsY - 4, PW, CHIP_H + 8);
    float cx = X0 - p.ss.pos;
    for (int i = 0; i < (int)p.seasons.size(); ++i) {
        const float w = textW(p.seasons[i].name, 16) + 32;
        const bool on = i == p.season;
        Ui::roundRect(cx, chipsY, w, CHIP_H, CHIP_H / 2, on ? pal.accent : pal.cardTop);
        Ui::roundBorder(cx, chipsY, w, CHIP_H, CHIP_H / 2, 1.5f, on ? pal.accentDark : pal.cardBorder);
        text(cx + 16, chipsY + (CHIP_H - 16) / 2 - 1, p.seasons[i].name, 16, on ? pal.textOnAccent : pal.text);
        cx += w + 8;
    }
    clip(0, listY, PW, FOOT_Y - listY);
    for (int i = 0; i < (int)p.episodes.size(); ++i) {
        const float y = listY + i * EP_ROW - p.vs.pos;
        if (y + EP_ROW < listY || y > FOOT_Y) continue;
        const JellyfinEpisode& e = p.episodes[i];
        if (i == p.sel) {
            Ui::roundRect(10, y + 3, PW - 20, EP_ROW - 6, 12, Ui::alpha(pal.accent, 0.28f));
            Ui::roundBorder(10, y + 3, PW - 20, EP_ROW - 6, 12, 2, Ui::alpha(pal.accent, 0.8f));
        }
        JellyfinItem it; it.id = e.id; it.name = e.name; it.type = "Episode";
        it.playbackPositionTicks = 0;
        GRRLIB_texImg* tex = ImagesWiiU::get(it, Kind::Thumb);
        const float tx = X0, ty = y + 8, tw = 120, th = 68;
        if (tex) image(tex, tx, ty, tw, th, 6);
        else Ui::roundRect(tx, ty, tw, th, 6, pal.cardTop);
        char b[220];
        snprintf(b, sizeof(b), "%d. %s", e.indexNumber, e.name.c_str());
        text(tx + tw + 16, y + 16, b, 18, pal.text, PW - (tx + tw + 16) - 56);
        if (e.played) {
            Ui::circle(PW - 38, y + EP_ROW / 2, 12, pal.accent);
            icon(Icon::Check, PW - 38, y + EP_ROW / 2, 15, pal.textOnAccent);
        } else if (e.playbackPositionTicks > 0) {
            text(tx + tw + 16, y + 46, fmtTime(e.playbackPositionTicks), 14, pal.textDim);
        }
    }
    clipOff();
}

void BrowseWiiU::drawDetail(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    if (!p.hasDetail) return;
    const JellyfinItemDetail& d = p.detail;
    /* the poster on the left */
    {
        const float x = X0 + 4, y = BAR_H + 18, w = 172, h = 258;
        JellyfinItem it = p.item;
        GRRLIB_texImg* tex = ImagesWiiU::get(posterOf(it), Kind::Poster);
        tileFrame(x, y, w, h, 10, 0);
        if (tex) image(tex, x, y, w, h, 10);
        else Ui::roundRect(x, y, w, h, 10, pal.cardTop);
        if (d.runtimeTicks > 0 && d.playbackPositionTicks > 0)
            progress(x + 8, y + h - 12, w - 16, (float)d.playbackPositionTicks / d.runtimeTicks);
    }
    /* its name and what it is (the TV says more) */
    float y = BAR_H + 16;
    std::string title = d.name;
    if (!d.seriesName.empty() && p.item.seasonNumber > 0) {
        char b[220];
        snprintf(b, sizeof(b), "S%d E%d \xC2\xB7 %s", p.item.seasonNumber, p.item.episodeNumber, d.name.c_str());
        title = b;
    }
    text(D_X, y, title, 22, pal.text, D_W);
    y += 32;
    std::string meta;
    if (d.year > 0) meta += std::to_string(d.year);
    const std::string rt = runtimeText(d.runtimeTicks);
    if (!rt.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + rt;
    if (!d.officialRating.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + d.officialRating;
    text(D_X, y, meta, 15, pal.textDim, D_W);
    if (!dual && !d.overview.empty())
        paragraph(D_X, y + 26, D_W, d.overview, 15, pal.text, 3, 19);

    const std::vector<int> ctls = detailControls(d);
    const int nActions = d.seriesId.empty() ? 2 : 3;
    y = detailTop(dual);
    for (int i = 0; i < (int)ctls.size(); ++i) {
        const int c = ctls[i];
        const float f = i == p.focus ? 1.0f : 0.0f;
        const float h = ctlH(c);
        if (c == C_PLAY) {
            const bool resume = d.playbackPositionTicks > 0;
            std::string label = resume ? std::string(tr("Resume")) + "  " + fmtTime(d.playbackPositionTicks) : tr("Play");
            button(D_X, y, D_W, h, label, 19, f, true, Icon::Play, true);
        } else if (c == C_START) {
            button(D_X, y, D_W, h, tr("From the start"), 16, f, false, Icon::Rewind, true);
        } else if (c == C_AUDIO || c == C_SUB) {
            std::string v;
            if (c == C_AUDIO) v = d.audioStreams[p.audio].displayTitle;
            else v = p.sub < 0 ? tr("Off") : d.subtitleStreams[p.sub].displayTitle;
            button(D_X, y, D_W, h, "", 15, f);
            icon(c == C_AUDIO ? Icon::Audio : Icon::Subtitles, D_X + 26, y + h / 2, 20, pal.text);
            icon(Icon::Back, D_X + 52, y + h / 2, 12, pal.textDim);
            textC(D_X + D_W / 2 + 10, y + (h - 15) / 2 - 1, v, 15, pal.text, D_W - 130);
            Ui::triangle(D_X + D_W - 34, y + h / 2 - 6, D_X + D_W - 34, y + h / 2 + 6, D_X + D_W - 26, y + h / 2, pal.textDim);
        } else {
            const float w = (D_W - 10 * (nActions - 1)) / nActions;
            const char* labels[3] = { tr("Favorite"), d.played ? tr("Watched") : tr("Not watched"), tr("Episodes") };
            const Icon icons[3] = { Icon::Heart, Icon::Check, Icon::Grid };
            for (int k = 0; k < nActions; ++k) {
                const bool on = (k == 0 && d.isFavorite) || (k == 1 && d.played);
                button(D_X + k * (w + 10), y, w, h, labels[k], 14, f > 0 && p.action == k ? 1.0f : 0.0f, on, icons[k], true);
            }
        }
        y += h + 7;
    }
}

void BrowseWiiU::drawTracks(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const float listY = BAR_H + 70;
    const float bx = dual ? X0 : X0 + 70;
    if (!dual) {   /* the cover beside the buttons (the TV shows it big) */
        GRRLIB_texImg* tex = ImagesWiiU::get(p.item, Kind::Poster);
        if (tex) image(tex, X0, BAR_H + 8, 56, 56, 6);
        else Ui::roundRect(X0, BAR_H + 8, 56, 56, 6, pal.cardTop);
    }
    button(bx, BAR_H + 12, 180, 44, tr("Play"), 17, 0, true, Icon::Play, true);
    button(bx + 192, BAR_H + 12, 180, 44, tr("Shuffle"), 17, 0, false, Icon::Shuffle, true);
    if (!p.item.artist.empty()) {
        const float room = PW - X0 - (bx + 192 + 180 + 16);
        textR(PW - X0, BAR_H + 26, ellipsize(p.item.artist, 15, room), 15, pal.textDim);
    }
    clip(0, listY, PW, FOOT_Y - listY);
    for (int i = 0; i < (int)p.tracks.size(); ++i) {
        const float y = listY + i * TR_ROW - p.vs.pos;
        if (y + TR_ROW < listY || y > FOOT_Y) continue;
        const JellyfinAudioItem& t = p.tracks[i];
        if (i == p.sel) Ui::roundRect(10, y + 2, PW - 20, TR_ROW - 4, 10, Ui::alpha(pal.accent, 0.3f));
        char b[16];
        snprintf(b, sizeof(b), "%d", t.trackNumber > 0 ? t.trackNumber : i + 1);
        textR(X0 + 30, y + 11, b, 17, pal.textDim);
        text(X0 + 46, y + 11, t.name, 17, pal.text, PW - X0 - 46 - 90);
        textR(PW - X0, y + 12, fmtTime(t.runtimeTicks), 15, pal.textDim);
    }
    clipOff();
}

/* ---- the TV --------------------------------------------------------------------------- */

const JellyfinItem* BrowseWiiU::selection(Page& p, JellyfinItem& tmp)
{
    if (p.type == Type::Home) {
        if (p.rows.empty()) return nullptr;
        const Row& r = p.rows[p.row];
        return r.sel < (int)r.items.size() ? &r.items[r.sel] : nullptr;
    }
    if (p.type == Type::Grid || p.type == Type::Search)
        return p.sel < (int)p.items.size() ? &p.items[p.sel] : nullptr;
    if (p.type == Type::Series) {
        if (p.sel >= (int)p.episodes.size()) return &p.item;
        const JellyfinEpisode& e = p.episodes[p.sel];
        tmp = JellyfinItem();
        tmp.id = e.id; tmp.name = e.name; tmp.type = "Episode";
        tmp.seriesName = p.item.type == "Series" ? p.item.name : p.item.seriesName;
        tmp.seriesId = p.item.type == "Series" ? p.item.id : p.item.seriesId;
        tmp.seasonNumber = e.seasonNumber; tmp.episodeNumber = e.indexNumber;
        tmp.playbackPositionTicks = e.playbackPositionTicks;
        return &tmp;
    }
    tmp = p.item;
    if (p.hasDetail) {
        tmp.runtimeTicks = p.detail.runtimeTicks;
        tmp.year = p.detail.year;
        tmp.officialRating = p.detail.officialRating;
        tmp.playbackPositionTicks = p.detail.playbackPositionTicks;
    }
    return &tmp;
}

void BrowseWiiU::makeInfo(const JellyfinItem* it)
{
    info = Info();
    if (!it) return;
    info.title = it->name;
    std::string l1;
    if (it->type == "Episode") {
        info.title = it->seriesName.empty() ? it->name : it->seriesName;
        char b[220];
        snprintf(b, sizeof(b), "S%d E%d \xC2\xB7 %s", it->seasonNumber, it->episodeNumber, it->name.c_str());
        info.line1 = b;
    }
    if (it->year > 0) l1 += std::to_string(it->year);
    const std::string rt = runtimeText(it->runtimeTicks);
    if (!rt.empty()) l1 += (l1.empty() ? "" : "  \xC2\xB7  ") + rt;
    if (!it->officialRating.empty()) l1 += (l1.empty() ? "" : "  \xC2\xB7  ") + it->officialRating;
    if (it->communityRating > 0) {
        char b[16];
        snprintf(b, sizeof(b), "\xE2\x98\x85 %.1f", (double)it->communityRating);
        l1 += (l1.empty() ? "" : "  \xC2\xB7  ") + std::string(b);
    }
    if (info.line1.empty()) info.line1 = l1; else info.line2 = l1;
    if (!it->artist.empty()) info.line2 = it->artist;
    if (it->runtimeTicks > 0 && it->playbackPositionTicks > 0)
        info.progress = (float)it->playbackPositionTicks / it->runtimeTicks;
    if (it->type == "CollectionFolder") {
        info.poster = *it;
        info.hasPoster = true;
        info.wide = true;
    } else {
        /* music: its cover, big (an album's "backdrop" is its cover stretched) */
        info.square = isSquare(*it);
        if (!info.square) info.picture = *it;
        info.poster = posterOf(*it);
        info.hasPoster = true;
    }
    /* its synopsis: fetched once the selection stays a moment */
    auto d = details.find(it->id);
    if (d != details.end()) info.overview = d->second.overview;
    else if (it->type != "CollectionFolder" && it->type != "Audio") {
        if (detailWanted != it->id) { detailWanted = it->id; focusSince = frames; }
        else if (frames - focusSince > 12 && (!detailJob || !detailJob->running())) {
            auto out = std::make_shared<JellyfinItemDetail>();
            const std::string id = it->id;
            JellyfinClient* cl = &client;
            const JellyfinAuth au = auth;
            const std::string url = serverUrl;
            detailJob = runJob([=]() { cl->getItemDetail(url, au, id, *out); },
                               [=]() { details[id] = *out; });
        }
    }
}

void BrowseWiiU::drawTV(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    background(TW, TH);
    if (p.type == Type::Detail && p.hasDetail) { drawTVDetail(p); return; }
    if (p.type == Type::Tracks && !p.loading) { drawTVTracks(p); return; }

    GRRLIB_texImg* back = info.picture.id.empty() ? nullptr : ImagesWiiU::get(info.picture, Kind::Backdrop);
    if (back) {
        image(back, 0, 0, TW, TH);
        rect(0, 0, TW, TH, 0x000000A8);   /* one veil: the text and the row readable */
    }
    const u32 tc = back ? 0xFFFFFFFF : pal.text;
    const u32 dc = back ? 0xD6DEE8FF : pal.textDim;
    logo(TV_X - 8, 54, 46);
    textR(TW - TV_X, 36, clockText(), 30, tc);

    if (p.loading) {
        spinner(TW / 2, TH / 2, 26);
        return;
    }
    /* the selection's picture beside the text (no backdrop) */
    float textW_ = back ? 900 : TW - 2 * TV_X;
    if (!back && info.hasPoster) {
        const bool wide = info.wide;
        const float w = wide ? 480 : info.square ? 330 : 236, h = wide ? 270 : info.square ? 330 : 354;
        const float x = TW - TV_X - w, y = 96;
        GRRLIB_texImg* tex = ImagesWiiU::get(info.poster, wide ? Kind::Thumb : Kind::Large);
        tileFrame(x, y, w, h, 12, 0);
        if (tex) image(tex, x, y, w, h, 12);
        else Ui::roundRect(x, y, w, h, 12, pal.cardTop);
        textW_ = x - TV_X - 40;
    }
    float y = 116;
    if (!info.title.empty()) {
        textS(TV_X, y, info.title, 50, tc, textW_);
        y += 66;
    }
    if (!info.line1.empty()) { text(TV_X, y, info.line1, 25, dc, textW_); y += 34; }
    if (!info.line2.empty()) { text(TV_X, y, info.line2, 25, dc, textW_); y += 34; }
    if (info.progress >= 0) { progress(TV_X, y + 4, 340, info.progress, 6); y += 20; }
    if (!info.overview.empty())
        paragraph(TV_X, y + 8, textW_ > 860 ? 860 : textW_, info.overview, 23, tc, (int)((CAR_Y - 20 - y) / 30), 30);
    drawTVCarousel(p);
}

/* the row of tiles at the bottom of the TV: where the selection is */
void BrowseWiiU::drawTVCarousel(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const bool back = !info.picture.id.empty() && ImagesWiiU::get(info.picture, Kind::Backdrop);
    const u32 tc = back ? 0xFFFFFFFF : pal.text;
    const u32 dc = back ? 0xD6DEE8FF : pal.textDim;
    const std::vector<JellyfinItem>* items = nullptr;
    std::vector<JellyfinItem> eps;
    int sel = 0;
    bool wide = false, thumbs = false;
    std::string title, key;
    if (p.type == Type::Home && !p.rows.empty()) {
        const Row& r = p.rows[p.row];
        items = &r.items; sel = p.bar >= 0 ? -1 : r.sel; wide = r.libraries; thumbs = r.libraries;
        title = r.title;
        key = "h" + r.title;
    } else if (p.type == Type::Grid || p.type == Type::Search) {
        items = &p.items; sel = p.sel;
        char b[64];
        snprintf(b, sizeof(b), "   %d / %d", p.sel + 1, p.total > 0 ? p.total : (int)p.items.size());
        title = (p.type == Type::Search ? std::string(tr("Search")) + ": " + p.title : p.title) + b;
        key = "g" + p.item.id + p.title;
    } else if (p.type == Type::Series) {
        for (const JellyfinEpisode& e : p.episodes) {
            JellyfinItem it;
            it.id = e.id; it.name = e.name; it.type = "Episode";
            it.seasonNumber = e.seasonNumber; it.episodeNumber = e.indexNumber;
            it.playbackPositionTicks = e.playbackPositionTicks; it.played = e.played;
            eps.push_back(it);
        }
        items = &eps; sel = p.sel; wide = true; thumbs = true;
        title = p.season < (int)p.seasons.size() ? p.seasons[p.season].name : p.title;
        key = "s" + p.item.id + title;
    }
    if (!items || items->empty()) return;
    textS(TV_X, CAR_Y, title, 26, tc, TW - 2 * TV_X);
    const float w = wide ? 272 : 132, h = wide ? 153 : 198, gap = 22, step = w + gap;
    const float y = CAR_Y + 54;
    const float view = TW - 2 * TV_X;
    const float maxScroll = std::max(0.0f, items->size() * step - gap - view);
    float target = (sel < 0 ? 0 : sel) * step - (view - w) * 0.35f;
    target = target < 0 ? 0 : target > maxScroll ? maxScroll : target;
    if (key != tvRowKey) { tvRowKey = key; tvScroll = target; }
    tvScroll += (target - tvScroll) * 0.25f;
    for (int i = 0; i < (int)items->size(); ++i) {
        const float x = TV_X + i * step - tvScroll;
        if (x + w < -40 || x > TW + 40) continue;
        const JellyfinItem& it = (*items)[i];
        const bool sq = !wide && isSquare(it);
        const float th = sq ? w : h;
        tile(it, x, y + (h - th), w, th, i == sel, thumbs ? Kind::Thumb : Kind::Poster);
        if (wide && p.type == Type::Series) {
            char b[16];
            snprintf(b, sizeof(b), "%d", it.episodeNumber);
            Ui::roundRect(x + 8, y + 8, textW(b, 18) + 18, 28, 14, 0x000000A0);
            text(x + 17, y + 12, b, 18, 0xFFFFFFFF);
        } else if (wide) {
            caption(x + w / 2, y + h + 16, it.name, 20, i == sel ? tc : dc, w, TW);
        }
    }
    (void)dc;
}

void BrowseWiiU::drawTVDetail(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const JellyfinItemDetail& d = p.detail;
    GRRLIB_texImg* back = ImagesWiiU::get(p.item, Kind::Backdrop);
    if (back) {
        image(back, 0, 0, TW, TH);
        rect(0, 0, TW, TH, 0x000000B0);
    }
    const u32 tc = back ? 0xFFFFFFFF : pal.text;
    const u32 dc = back ? 0xD6DEE8FF : pal.textDim;
    logo(TV_X - 8, 54, 46);
    textR(TW - TV_X, 36, clockText(), 30, tc);
    /* the poster on the right */
    const float pw = 300, ph = 450, px = TW - TV_X - pw, py = 120;
    {
        GRRLIB_texImg* tex = ImagesWiiU::get(posterOf(p.item), Kind::Large);
        tileFrame(px, py, pw, ph, 14, 0);
        if (tex) image(tex, px, py, pw, ph, 14);
        else Ui::roundRect(px, py, pw, ph, 14, pal.cardTop);
    }
    const float tw = px - TV_X - 60;
    float y = 120;
    textS(TV_X, y, d.seriesName.empty() ? d.name : d.seriesName, 50, tc, tw);
    y += 66;
    if (!d.seriesName.empty()) {
        char b[240];
        snprintf(b, sizeof(b), "S%d E%d \xC2\xB7 %s", p.item.seasonNumber, p.item.episodeNumber, d.name.c_str());
        text(TV_X, y, b, 27, tc, tw);
        y += 38;
    }
    std::string meta;
    if (d.year > 0) meta += std::to_string(d.year);
    const std::string rt = runtimeText(d.runtimeTicks);
    if (!rt.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + rt;
    if (!d.officialRating.empty()) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + d.officialRating;
    if (!meta.empty()) { text(TV_X, y, meta, 25, dc, tw); y += 36; }
    if (!d.genres.empty()) {
        std::string g;
        for (const std::string& s : d.genres) g += (g.empty() ? "" : ", ") + s;
        text(TV_X, y, g, 23, dc, tw);
        y += 34;
    }
    if (d.runtimeTicks > 0 && d.playbackPositionTicks > 0) {
        progress(TV_X, y + 4, 360, (float)d.playbackPositionTicks / d.runtimeTicks, 6);
        text(TV_X + 376, y - 6, fmtTime(d.playbackPositionTicks) + " / " + fmtTime(d.runtimeTicks), 20, dc);
        y += 28;
    }
    y += 8;
    float bottom = TH - 150;   /* the people's lines, the tracks' chips under it */
    if (!d.overview.empty())
        y += paragraph(TV_X, y, tw, d.overview, 23, tc, (int)((bottom - y) / 31), 31) + 10;
    /* the director, then the cast */
    std::string people;
    for (const JellyfinPerson& pe : d.people)
        if (pe.role == "Director") people += (people.empty() ? "" : ", ") + pe.name;
    for (const JellyfinPerson& pe : d.people)
        if (pe.role == "Actor") people += (people.empty() ? "" : ", ") + pe.name;
    if (!people.empty())
        paragraph(TV_X, TH - 136, tw, std::string(tr("Cast & crew")) + ": " + people, 21, dc, 2, 28);
    /* the tracks to play with */
    float cx = TV_X;
    auto chip = [&](Icon ic, const std::string& s0) {
        const std::string s = ellipsize(s0, 20, 340);
        const float w = textW(s, 20) + 64;
        if (cx + w > px - 30) return;
        Ui::roundRect(cx, TH - 70, w, 40, 20, Ui::alpha(back ? 0x000000FF : pal.cardTop, 0.6f));
        Ui::roundBorder(cx, TH - 70, w, 40, 20, 1.5f, Ui::alpha(back ? 0xFFFFFFFF : pal.cardBorder, 0.5f));
        icon(ic, cx + 26, TH - 50, 22, tc);
        text(cx + 46, TH - 61, s, 20, tc);
        cx += w + 14;
    };
    if (!d.audioStreams.empty()) chip(Icon::Audio, d.audioStreams[p.audio].displayTitle);
    if (!d.subtitleStreams.empty()) chip(Icon::Subtitles, p.sub < 0 ? tr("Off") : d.subtitleStreams[p.sub].displayTitle);
    if (d.isFavorite) chip(Icon::Heart, tr("Favorite"));
}

void BrowseWiiU::drawTVTracks(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    logo(TV_X - 8, 54, 46);
    textR(TW - TV_X, 36, clockText(), 30, pal.text);
    const float cs = 420, cx = TV_X, cy = 130;
    GRRLIB_texImg* tex = ImagesWiiU::get(p.item, Kind::Large);
    tileFrame(cx, cy, cs, cs, 16, 0);
    if (tex) image(tex, cx, cy, cs, cs, 16);
    else {
        Ui::roundRect(cx, cy, cs, cs, 16, pal.cardTop);
        icon(Icon::Music, cx + cs / 2, cy + cs / 2, 120, pal.textDim);
    }
    const float x = cx + cs + 60, w = TW - TV_X - x;
    float y = 120;
    textS(x, y, p.item.name, 44, pal.text, w);
    y += 58;
    std::string sub = p.item.artist;
    if (p.item.year > 0) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + std::to_string(p.item.year);
    if (!sub.empty()) { text(x, y, sub, 25, pal.textDim, w); y += 40; }
    y += 10;
    const int n = (int)p.tracks.size();
    const int show = 11;
    int first = p.sel - show / 2;
    if (first > n - show) first = n - show;
    if (first < 0) first = 0;
    for (int i = first; i < n && i < first + show; ++i) {
        const JellyfinAudioItem& t = p.tracks[i];
        const float ry = y + (i - first) * 40;
        if (i == p.sel) {
            Ui::roundRect(x - 14, ry - 4, w + 14, 38, 12, Ui::alpha(pal.accent, 0.3f));
            Ui::roundBorder(x - 14, ry - 4, w + 14, 38, 12, 2, Ui::alpha(pal.accent, 0.8f));
        }
        char b[16];
        snprintf(b, sizeof(b), "%d", t.trackNumber > 0 ? t.trackNumber : i + 1);
        textR(x + 30, ry + 4, b, 22, pal.textDim);
        text(x + 48, ry + 4, t.name, 22, pal.text, w - 48 - 100);
        textR(x + w - 8, ry + 5, fmtTime(t.runtimeTicks), 20, pal.textDim);
    }
}

/* ---- one frame -------------------------------------------------------------------- */

bool BrowseWiiU::frame(ir_t& ir)
{
    (void)ir;
    ++frames;
    dual = GfxWiiU::mode() == GfxWiiU::Mode::Remote;
    readTouch(touch);
    dragAxis(touch);
    for (auto& p : stack) { if (p->job) p->job->poll(); if (p->job2) p->job2->poll(); }
    if (detailJob) detailJob->poll();
    for (size_t i = 0; i < s_orphans->size();) {
        if ((*s_orphans)[i]->done) { (*s_orphans)[i]->poll(); s_orphans->erase(s_orphans->begin() + i); }
        else ++i;
    }

    Page& p = page();
    /* (a tap is the GamePad's A in Mirror mode: not the button's) */
    const bool a = Input::isAJustPressed() && !touch.tap && !searching;
    if (searching) searchFrame();

    /* page input (before drawing: the taps act on what was drawn) */
    if (!p.loading && !searching) {
        switch (p.type) {
        case Type::Home:   updateHome(p, a); break;
        case Type::Grid:
        case Type::Search: updateGrid(p, a); break;
        case Type::Series: updateSeries(p, a); break;
        case Type::Detail: updateDetail(p, a); break;
        case Type::Tracks: updateTracks(p, a); break;
        }
    }
    Page& cur = page();   /* (input may have opened a page) */

    /* what the TV shows: the selection */
    {
        JellyfinItem tmp;
        makeInfo(selection(cur, tmp));
    }

    /* drawing: the TV, then the GamePad (the main frame's when alone) */
    int tapped = -2;
    auto drawPad = [&]() {
        padSpace();
        background(PW, PH);
        if (cur.loading) spinner(PW / 2, PH / 2, 18);
        else switch (cur.type) {
            case Type::Home:   drawHome(cur); break;
            case Type::Grid:
            case Type::Search: drawGrid(cur); break;
            case Type::Series: drawSeries(cur); break;
            case Type::Detail: drawDetail(cur); break;
            case Type::Tracks: drawTracks(cur); break;
        }
        tapped = drawBar(cur);
        drawFooter(cur);
        if (frames < messageUntil) {
            const Ui::Palette& pal = Ui::pal();
            const std::string m = ellipsize(message, 17, PW - 80);
            const float w = textW(m, 17) + 40;
            Ui::shadow((PW - w) / 2, FOOT_Y - 60, w, 44, 22, 6, pal.shadow);
            Ui::roundRect((PW - w) / 2, FOOT_Y - 60, w, 44, 22, pal.cardTop);
            textC(PW / 2, FOOT_Y - 48, m, 17, pal.text);
        }
    };
    if (dual) {
        InputWiiU::setRemotePointer(false, 0, 0);
        tvSpace();
        drawTV(cur);
        if (GfxWiiU::padBegin()) {
            drawPad();
            GfxWiiU::padEnd();
        }
    } else {
        drawPad();
    }
    ImagesWiiU::endFrame();
    wiiSpace();
    if (searching) return false;

    /* the bar's taps (and the D-pad's on the home's bar), the buttons acting on the whole */
    const bool home = cur.type == Type::Home;
    if (home && cur.bar >= 0 && a) tapped = cur.bar;
    if (tapped == -1 || (Input::isBPressed() && !touch.tap)) {
        if (stack.size() > 1) pop();
        return false;
    }
    if (tapped == 0 || Input::is1Pressed()) {
        searching = true;
        searchText.clear();
        SoundFX::play(SoundFX::FX::Open);
        return false;
    }
    if (home && tapped == 1) { exitTo = Exit::Settings; return true; }
    if (home && tapped == 2) { exitTo = Exit::Profiles; return true; }
    if (!home && tapped == 1) { while (stack.size() > 1) pop(); }
    return pendingPlayIsMusic || !pendingPlayUrl.empty();
}
