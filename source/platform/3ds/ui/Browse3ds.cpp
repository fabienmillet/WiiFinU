#include "Browse3ds.h"
#include "Images3ds.h"
#include "../Gfx3ds.h"
#include "../../../ui/Ui.h"
#include "../../../input/Input.h"
#include "../../../core/Lang.h"
#include "../../../core/SoundFX.h"

#include <3ds.h>
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ogc/lwp.h>
#include <ogc/system.h>

using namespace Ui3ds;

namespace {

/* jobs of pages left while they ran: finished in the background */
std::vector<std::shared_ptr<Browse3ds::Job>>* s_orphans = nullptr;

std::shared_ptr<Browse3ds::Job> runJob(std::function<void()> work, std::function<void()> apply)
{
    return runJob3ds(std::move(work), std::move(apply));
}

const int POSTER_W = 52, POSTER_H = 78;      /* home rows */
const int GRID_COLS = 5, GRID_W = 56, GRID_H = 84, GRID_GAP = 6;
const int LIB_W = 100, LIB_H = 56;

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

void focusRing(float x, float y, float w, float h, float r)
{
    const Ui::Palette& p = Ui::pal();
    Ui::shadow(x, y, w, h, r, 5, p.glow);
    Ui::roundBorder(x - 1, y - 1, w + 2, h + 2, r + 1, 2.0f, p.accent);
}

/* a poster (or its title on a card while it loads / has none) */
void poster(const JellyfinItem& it, float x, float y, float w, float h, bool focus,
            Images3ds::Kind kind = Images3ds::Kind::Poster)
{
    const Ui::Palette& p = Ui::pal();
    GRRLIB_texImg* tex = Images3ds::get(it, kind);
    if (focus) focusRing(x, y, w, h, 4);
    if (tex) image(tex, x, y, w, h, 4);
    else {
        Ui::roundRect(x, y, w, h, 4, p.cardTop, p.cardBottom);
        paragraph(x + 4, y + 4, w - 8, it.name, 10, p.textDim, (int)(h - 8) / 13);
    }
    if (it.runtimeTicks > 0 && it.playbackPositionTicks > 0)
        progress(x + 3, y + h - 6, w - 6, (float)it.playbackPositionTicks / it.runtimeTicks);
    if (it.played) {
        Ui::circle(x + w - 9, y + 9, 7, p.accent);
        icon(Icon::Check, x + w - 9, y + 9, 9, p.textOnAccent);
    }
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

/* a scroller dragged along its axis, else coasting */
void scroll(Scroller& s, const Touch& t, bool inside, Axis axis)
{
    Touch tt = t;
    if (s_axis != axis || !inside) { tt.dragging = false; tt.pressed = false; }
    if (s_axis == axis && inside && t.dragging && !s.grabbed) { s.grabbed = true; s.target = -1; }
    s.update(tt, false);
    if (!t.down) s.grabbed = false;
}

} // namespace

/* ---- life ------------------------------------------------------------------ */

Browse3ds::Browse3ds(JellyfinClient& c, const JellyfinAuth& a, const std::string& url, const std::string& user)
    : client(c), auth(a), serverUrl(url), userName(user)
{
    static std::vector<std::shared_ptr<Job>> orphans;
    s_orphans = &orphans;
    Images3ds::start(client, auth, serverUrl);
    auto home = std::make_unique<Page>();
    home->type = Type::Home;
    push(std::move(home));
}

Browse3ds::~Browse3ds()
{
    setTopInfo(nullptr);
    stack.clear();
    if (detailJob) detailJob.reset();
    if (s_orphans) s_orphans->clear();
    Images3ds::stop();
}

void Browse3ds::push(std::unique_ptr<Page> p)
{
    stack.push_back(std::move(p));
    load(*stack.back());
}

void Browse3ds::pop()
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

void Browse3ds::notice(const std::string& s)
{
    message = s;
    messageUntil = frames + 150;
}

/* ---- loading ---------------------------------------------------------------- */

void Browse3ds::load(Page& p)
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
            /* the row selected before a reload stays selected */
            const std::string keep = pp->row < (int)pp->rows.size() ? pp->rows[pp->row].title : "";
            std::vector<Row> latest;   /* a reload: the "Latest in" rows kept until new ones come */
            for (Row& r : pp->rows) if (r.latest) latest.push_back(std::move(r));
            pp->rows = std::move(*rows);
            for (Row& r : latest) pp->rows.push_back(std::move(r));
            for (int i = 0; i < (int)pp->rows.size(); ++i)
                if (pp->rows[i].title == keep) pp->row = i;
            if (pp->row >= (int)pp->rows.size()) pp->row = 0;
            for (Row& r : pp->rows) r.hs.horizontal = true;
            pp->loading = false;

            auto more = std::make_shared<std::vector<Row>>();
            if (pp->job2 && pp->job2->running()) {   /* an earlier load's: dropped */
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
                    snprintf(q, sizeof(q), "/Users/%s/Items/Latest?ParentId=%s&Limit=16", au.userId.c_str(), l.id.c_str());
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
            cl->getItems(url, au, lib.id, 0, 60, *items, *total, extra);
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
                       [=]() { pp->items = std::move(*items); pp->loading = false; });
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

void Browse3ds::loadMore(Page& p)
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
        cl->getItems(url, au, lib.id, start, 60, *items, *total, extra);
    }, [=]() {
        pp->items.insert(pp->items.end(), items->begin(), items->end());
        pp->more = !items->empty() && (int)pp->items.size() < *total;
    });
}

/* ---- opening what was chosen ------------------------------------------------- */

void Browse3ds::open(const JellyfinItem& it)
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

/* ---- the top screen --------------------------------------------------------- */

void Browse3ds::setTop(const JellyfinItem* it)
{
    top = TopInfo();
    if (!it) { setTopInfo(nullptr); return; }
    top.title = it->name;
    std::string l1;
    if (it->type == "Episode") {
        top.title = it->seriesName.empty() ? it->name : it->seriesName;
        char b[160];
        snprintf(b, sizeof(b), "S%d E%d \xC2\xB7 %s", it->seasonNumber, it->episodeNumber, it->name.c_str());
        top.line1 = b;
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
    if (top.line1.empty()) top.line1 = l1; else top.line2 = l1;
    if (!it->artist.empty()) top.line2 = it->artist;
    if (it->runtimeTicks > 0 && it->playbackPositionTicks > 0)
        top.progress = (float)it->playbackPositionTicks / it->runtimeTicks;
    /* the backdrop (an episode: its series'), else the poster */
    top.backdrop = it->type == "CollectionFolder" ? nullptr : Images3ds::get(*it, Images3ds::Kind::Backdrop);
    if (!top.backdrop) top.poster = Images3ds::get(*it, it->type == "CollectionFolder" ? Images3ds::Kind::Thumb
                                                                                       : Images3ds::Kind::Poster);
    /* its synopsis: fetched once the selection stays a moment */
    auto d = details.find(it->id);
    if (d != details.end()) top.overview = d->second.overview;
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
    setTopInfo(&top);
}

/* ---- playing ------------------------------------------------------------------ */

void Browse3ds::play(Page& p, bool fromStart)
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
        SYS_Report("[Browse3ds] no playback URL: %s\n", client.lastError().c_str());
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

void Browse3ds::playTracks(Page& p, int from, bool shuffle)
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

void Browse3ds::onPlaybackFinished()
{
    pendingPlayUrl.clear();
    pendingPlayIsMusic = false;
    /* resume points moved: the home's rows and the page shown again */
    for (auto& p : stack)
        if (p->type == Type::Home || p->type == Type::Detail || p->type == Type::Series) {
            if (p->job && p->job->running()) continue;
            if (p->type == Type::Detail) details.erase(p->item.id);
            load(*p);
        }
}

/* ---- search (the 3DS's own keyboard) ------------------------------------------ */

void Browse3ds::search()
{
    char buf[128] = "";
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, 120);
    swkbdSetHintText(&kb, tr("Search"));
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, tr("Cancel"), false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, tr("Search"), true);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    Gfx3ds::endFrameForApplet();
    const SwkbdButton b = swkbdInputText(&kb, buf, sizeof(buf));
    if (b != SWKBD_BUTTON_RIGHT || !buf[0]) return;
    auto p = std::make_unique<Page>();
    p->type = Type::Search;
    p->title = buf;
    push(std::move(p));
}

/* ---- the pages: input --------------------------------------------------------- */

void Browse3ds::updateHome(Page& p, bool a)
{
    if (p.rows.empty()) return;
    const float y0 = BAR_H + 4;
    /* rows: heights, then scrolling */
    std::vector<float> rowY;
    float y = 0;
    for (const Row& r : p.rows) { rowY.push_back(y); y += 18 + (r.libraries ? LIB_H : POSTER_H) + 10; }
    p.vs.maxPos = y - (BH - y0);
    const bool inBody = touch.startY >= y0;
    scroll(p.vs, touch, inBody, Axis::V);
    for (int r = 0; r < (int)p.rows.size(); ++r) {
        Row& row = p.rows[r];
        const float cw = (row.libraries ? LIB_W : POSTER_W) + 6;
        row.hs.maxPos = row.items.size() * cw + 8 - BW;
        const float ry = y0 + rowY[r] - p.vs.pos;
        const float rh = 18 + (row.libraries ? LIB_H : POSTER_H);
        scroll(row.hs, touch, inBody && touch.startY >= ry && touch.startY < ry + rh, Axis::H);
    }
    /* D-pad */
    Row& cur = p.rows[p.row];
    if (Input::isUpPressed() && p.row > 0) { --p.row; SoundFX::play(SoundFX::FX::Move); }
    if (Input::isDownPressed() && p.row + 1 < (int)p.rows.size()) { ++p.row; SoundFX::play(SoundFX::FX::Move); }
    if (Input::isLeftPressed() && cur.sel > 0) { --cur.sel; SoundFX::play(SoundFX::FX::Move); }
    if (Input::isRightPressed() && cur.sel + 1 < (int)cur.items.size()) { ++cur.sel; SoundFX::play(SoundFX::FX::Move); }
    if (Input::isUpPressed() || Input::isDownPressed() || Input::isLeftPressed() || Input::isRightPressed()) {
        Row& r = p.rows[p.row];
        p.vs.show(rowY[p.row], rowY[p.row] + 18 + (r.libraries ? LIB_H : POSTER_H) + 10, BH - y0);
        const float cw = (r.libraries ? LIB_W : POSTER_W) + 6;
        r.hs.show(r.sel * cw, r.sel * cw + cw + 8, BW);
    }
    /* taps */
    for (int r = 0; r < (int)p.rows.size(); ++r) {
        Row& row = p.rows[r];
        const float ry = y0 + rowY[r] - p.vs.pos + 18;
        const float w = row.libraries ? LIB_W : POSTER_W, h = row.libraries ? LIB_H : POSTER_H;
        for (int i = 0; i < (int)row.items.size(); ++i) {
            const float x = 8 + i * (w + 6) - row.hs.pos;
            if (touch.tap && touch.y >= y0 && hit(touch, x, ry, w, h)) {
                p.row = r; row.sel = i;
                open(row.items[i]);
                return;
            }
        }
    }
    if (a && cur.sel < (int)cur.items.size()) open(cur.items[cur.sel]);
}

void Browse3ds::updateGrid(Page& p, bool a)
{
    const float y0 = BAR_H + 6;
    const int n = (int)p.items.size();
    const int rows = (n + GRID_COLS - 1) / GRID_COLS;
    const float rowH = GRID_H + GRID_GAP + 2;
    p.vs.maxPos = rows * rowH - (BH - y0) + 4;
    scroll(p.vs, touch, touch.startY >= y0, Axis::V);
    int sel = p.sel;
    if (Input::isLeftPressed() && sel > 0) --sel;
    if (Input::isRightPressed() && sel + 1 < n) ++sel;
    if (Input::isUpPressed() && sel >= GRID_COLS) sel -= GRID_COLS;
    if (Input::isDownPressed()) sel = std::min(n - 1, sel + GRID_COLS);
    if (Input::isLPressed()) sel = std::max(0, sel - GRID_COLS * 2);
    if (Input::isRPressed()) sel = std::min(n - 1, sel + GRID_COLS * 2);
    if (sel != p.sel && sel >= 0) {
        p.sel = sel;
        SoundFX::play(SoundFX::FX::Move);
        const float ry = (sel / GRID_COLS) * rowH;
        p.vs.show(ry, ry + rowH, BH - y0);
    }
    /* near the end: the next page */
    if (p.more && (p.vs.pos + (BH - y0)) / rowH + 3 >= rows) loadMore(p);
    const float x0 = (BW - (GRID_COLS * GRID_W + (GRID_COLS - 1) * GRID_GAP)) * 0.5f;
    for (int i = 0; i < n; ++i) {
        const float x = x0 + (i % GRID_COLS) * (GRID_W + GRID_GAP), y = y0 + (i / GRID_COLS) * rowH - p.vs.pos;
        if (touch.y >= y0 && hit(touch, x, y, GRID_W, GRID_H)) { p.sel = i; open(p.items[i]); return; }
    }
    if (a && p.sel < n) open(p.items[p.sel]);
}

void Browse3ds::updateSeries(Page& p, bool a)
{
    const float chipsY = BAR_H + 4, listY = BAR_H + 32;
    /* seasons: chips */
    float cx = 8;
    std::vector<float> chipX;
    for (const JellyfinSeason& s : p.seasons) { chipX.push_back(cx); cx += textW(s.name, 12) + 22; }
    p.ss.horizontal = true;
    p.ss.maxPos = cx - BW + 4;
    scroll(p.ss, touch, touch.startY >= chipsY && touch.startY < listY, Axis::H);
    int season = p.season;
    for (int i = 0; i < (int)p.seasons.size(); ++i)
        if (hit(touch, chipX[i] - p.ss.pos, chipsY, textW(p.seasons[i].name, 12) + 16, 24)) season = i;
    if (Input::isLPressed() && season > 0) --season;
    if (Input::isRPressed() && season + 1 < (int)p.seasons.size()) ++season;
    if (season != p.season) {
        p.season = season;
        p.ss.show(chipX[season], chipX[season] + textW(p.seasons[season].name, 12) + 22, BW);
        SoundFX::play(SoundFX::FX::Page);
        load(p);
        return;
    }
    /* episodes */
    const float rowH = 44;
    const int n = (int)p.episodes.size();
    p.vs.maxPos = n * rowH - (BH - listY);
    scroll(p.vs, touch, touch.startY >= listY, Axis::V);
    int sel = p.sel;
    if (Input::isUpPressed() && sel > 0) --sel;
    if (Input::isDownPressed() && sel + 1 < n) ++sel;
    if (sel != p.sel) { p.sel = sel; SoundFX::play(SoundFX::FX::Move); p.vs.show(sel * rowH, sel * rowH + rowH, BH - listY); }
    int open_ = -1;
    for (int i = 0; i < n; ++i) {
        const float y = listY + i * rowH - p.vs.pos;
        if (touch.y >= listY && hit(touch, 0, y, BW, rowH)) open_ = i;
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

void Browse3ds::updateDetail(Page& p, bool a)
{
    if (!p.hasDetail) return;
    JellyfinItemDetail& d = p.detail;
    const bool resume = d.playbackPositionTicks > 0;
    std::vector<int> ctls = { C_PLAY };
    if (resume) ctls.push_back(C_START);
    if (d.audioStreams.size() > 1) ctls.push_back(C_AUDIO);
    if (!d.subtitleStreams.empty()) ctls.push_back(C_SUB);
    ctls.push_back(C_ACTIONS);
    if (p.focus >= (int)ctls.size()) p.focus = 0;
    if (Input::isUpPressed() && p.focus > 0) { --p.focus; SoundFX::play(SoundFX::FX::Move); }
    if (Input::isDownPressed() && p.focus + 1 < (int)ctls.size()) { ++p.focus; SoundFX::play(SoundFX::FX::Move); }
    const int c = ctls[p.focus];
    auto cycleAudio = [&](int dir) { if (!d.audioStreams.empty()) p.audio = (p.audio + dir + (int)d.audioStreams.size()) % (int)d.audioStreams.size(); };
    auto cycleSub = [&](int dir) {
        const int n = (int)d.subtitleStreams.size() + 1;
        p.sub = (p.sub + 1 + dir + n) % n - 1;
    };
    if (c == C_AUDIO && Input::isLeftPressed()) cycleAudio(-1);
    if (c == C_AUDIO && Input::isRightPressed()) cycleAudio(1);
    if (c == C_SUB && Input::isLeftPressed()) cycleSub(-1);
    if (c == C_SUB && Input::isRightPressed()) cycleSub(1);
    static int s_action = 0;   /* the actions row: which one */
    if (c == C_ACTIONS && Input::isLeftPressed() && s_action > 0) --s_action;
    if (c == C_ACTIONS && Input::isRightPressed() && s_action < 2) ++s_action;

    /* the layout (as drawDetail) */
    float y = BAR_H + 10;
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
    for (int ctl : ctls) {
        const float h = ctl == C_PLAY ? 40 : ctl == C_ACTIONS ? 36 : 30;
        if (ctl == C_ACTIONS) {
            const float w = (BW - 32) / 3.0f;
            for (int i = 0; i < 3; ++i)
                if (hit(touch, 12 + i * (w + 4), y, w, h)) { s_action = i; act(i); }
        } else if (hit(touch, 12, y, BW - 24, h)) {
            if (ctl == C_PLAY) { play(p, false); return; }
            if (ctl == C_START) { play(p, true); return; }
            if (ctl == C_AUDIO) cycleAudio(touch.x > BW / 2 ? 1 : -1);
            if (ctl == C_SUB) cycleSub(touch.x > BW / 2 ? 1 : -1);
        }
        y += h + 8;
    }
    if (a) {
        if (c == C_PLAY) play(p, false);
        else if (c == C_START) play(p, true);
        else if (c == C_AUDIO) cycleAudio(1);
        else if (c == C_SUB) cycleSub(1);
        else act(s_action);
    }
}

void Browse3ds::updateTracks(Page& p, bool a)
{
    const float listY = BAR_H + 44, rowH = 30;
    const int n = (int)p.tracks.size();
    p.vs.maxPos = n * rowH - (BH - listY);
    scroll(p.vs, touch, touch.startY >= listY, Axis::V);
    if (hit(touch, 12, BAR_H + 8, 140, 30)) { playTracks(p, 0, false); return; }
    if (hit(touch, 168, BAR_H + 8, 140, 30)) { playTracks(p, 0, true); return; }
    int sel = p.sel;
    if (Input::isUpPressed() && sel > 0) --sel;
    if (Input::isDownPressed() && sel + 1 < n) ++sel;
    if (sel != p.sel) { p.sel = sel; SoundFX::play(SoundFX::FX::Move); p.vs.show(sel * rowH, sel * rowH + rowH, BH - listY); }
    for (int i = 0; i < n; ++i)
        if (touch.y >= listY && hit(touch, 0, listY + i * rowH - p.vs.pos, BW, rowH)) { playTracks(p, i, false); return; }
    if (Input::is2Pressed()) { playTracks(p, 0, true); return; }
    if (a && n) playTracks(p, p.sel, false);
}

/* ---- the pages: drawing --------------------------------------------------------- */

void Browse3ds::drawLoading()
{
    spinner(BW / 2, BH / 2 + 10, 12);
}

void Browse3ds::drawHome(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const float y0 = BAR_H + 4;
    GRRLIB_ClipDrawing(0, (int)y0 * 2, BW * 2, (int)(BH - y0) * 2);
    float y = y0 - p.vs.pos;
    for (int r = 0; r < (int)p.rows.size(); ++r) {
        Row& row = p.rows[r];
        const float h = row.libraries ? LIB_H : POSTER_H, w = row.libraries ? LIB_W : POSTER_W;
        if (y + 18 + h > y0 && y < BH) {
            text(8, y + 1, row.title, 12, r == p.row ? pal.text : pal.textDim);
            for (int i = 0; i < (int)row.items.size(); ++i) {
                const float x = 8 + i * (w + 6) - row.hs.pos;
                if (x + w < 0 || x > BW) continue;
                const bool focus = r == p.row && i == row.sel;
                if (row.libraries) {
                    const JellyfinItem& it = row.items[i];
                    GRRLIB_texImg* tex = Images3ds::get(it, Images3ds::Kind::Thumb);
                    if (focus) focusRing(x, y + 18, w, h, 6);
                    if (tex) {
                        image(tex, x, y + 18, w, h, 6);
                        Ui::roundRect(x, y + 18 + h - 18, w, 18, 0, 0x00000000, 0x000000C0);
                        textC(x + w / 2, y + 18 + h - 16, it.name, 11, 0xFFFFFFFF, w - 6);
                    } else {
                        Ui::roundRect(x, y + 18, w, h, 6, pal.cardTop, pal.cardBottom);
                        textC(x + w / 2, y + 18 + h / 2 - 7, it.name, 12, pal.text, w - 8);
                    }
                } else {
                    poster(row.items[i], x, y + 18, w, h, focus);
                }
            }
        }
        y += 18 + h + 10;
    }
    GRRLIB_ClipReset();
    if (p.rows.empty() && !p.loading) textC(BW / 2, BH / 2, tr("Nothing here yet"), 13, pal.textDim);
}

void Browse3ds::drawGrid(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const float y0 = BAR_H + 6;
    const float rowH = GRID_H + GRID_GAP + 2;
    const float x0 = (BW - (GRID_COLS * GRID_W + (GRID_COLS - 1) * GRID_GAP)) * 0.5f;
    GRRLIB_ClipDrawing(0, BAR_H * 2, BW * 2, (BH - BAR_H) * 2);
    for (int i = 0; i < (int)p.items.size(); ++i) {
        const float x = x0 + (i % GRID_COLS) * (GRID_W + GRID_GAP), y = y0 + (i / GRID_COLS) * rowH - p.vs.pos;
        if (y + GRID_H < BAR_H || y > BH) continue;
        poster(p.items[i], x, y, GRID_W, GRID_H, i == p.sel);
    }
    GRRLIB_ClipReset();
    if (p.items.empty() && !p.loading)
        textC(BW / 2, BH / 2, p.type == Type::Search ? tr("No results") : tr("Nothing here yet"), 13, pal.textDim);
}

void Browse3ds::drawSeries(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const float chipsY = BAR_H + 4, listY = BAR_H + 32;
    float cx = 8 - p.ss.pos;
    for (int i = 0; i < (int)p.seasons.size(); ++i) {
        const float w = textW(p.seasons[i].name, 12) + 16;
        const bool on = i == p.season;
        Ui::roundRect(cx, chipsY, w, 22, 11, on ? pal.accent : pal.cardTop, on ? pal.accentDark : pal.cardBottom);
        text(cx + 8, chipsY + 4, p.seasons[i].name, 12, on ? pal.textOnAccent : pal.text);
        cx += w + 6;
    }
    GRRLIB_ClipDrawing(0, (int)listY * 2, BW * 2, (int)(BH - listY) * 2);
    const float rowH = 44;
    for (int i = 0; i < (int)p.episodes.size(); ++i) {
        const float y = listY + i * rowH - p.vs.pos;
        if (y + rowH < listY || y > BH) continue;
        const JellyfinEpisode& e = p.episodes[i];
        if (i == p.sel) Ui::roundRect(4, y + 1, BW - 8, rowH - 2, 6, Ui::alpha(pal.accent, 0.35f));
        JellyfinItem it; it.id = e.id; it.name = e.name;
        GRRLIB_texImg* tex = Images3ds::get(it, Images3ds::Kind::Thumb);
        if (tex) image(tex, 8, y + 4, 64, 36, 3);
        else Ui::roundRect(8, y + 4, 64, 36, 3, pal.cardTop, pal.cardBottom);
        char b[200];
        snprintf(b, sizeof(b), "%d. %s", e.indexNumber, e.name.c_str());
        text(80, y + 8, b, 12, pal.text, BW - 80 - 22);
        if (e.played) {
            Ui::circle(BW - 14, y + rowH / 2, 7, pal.accent);
            icon(Icon::Check, BW - 14, y + rowH / 2, 9, pal.textOnAccent);
        } else if (e.playbackPositionTicks > 0) {
            text(80, y + 24, fmtTime(e.playbackPositionTicks), 10, pal.textDim);
        }
    }
    GRRLIB_ClipReset();
}

void Browse3ds::drawDetail(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    if (!p.hasDetail) return;
    const JellyfinItemDetail& d = p.detail;
    const bool resume = d.playbackPositionTicks > 0;
    std::vector<int> ctls = { C_PLAY };
    if (resume) ctls.push_back(C_START);
    if (d.audioStreams.size() > 1) ctls.push_back(C_AUDIO);
    if (!d.subtitleStreams.empty()) ctls.push_back(C_SUB);
    ctls.push_back(C_ACTIONS);
    float y = BAR_H + 10;
    static int s_actionDraw = 0;
    (void)s_actionDraw;
    for (int i = 0; i < (int)ctls.size(); ++i) {
        const int c = ctls[i];
        const float f = i == p.focus ? 1.0f : 0.0f;
        const float h = c == C_PLAY ? 40 : c == C_ACTIONS ? 36 : 30;
        if (c == C_PLAY) {
            std::string label = resume ? std::string(tr("Resume")) + "  " + fmtTime(d.playbackPositionTicks) : tr("Play");
            button(12, y, BW - 24, h, label, 15, f, true, Icon::Play, true);
        } else if (c == C_START) {
            button(12, y, BW - 24, h, tr("From the start"), 13, f, false, Icon::Rewind, true);
        } else if (c == C_AUDIO || c == C_SUB) {
            std::string v;
            if (c == C_AUDIO) v = d.audioStreams[p.audio].displayTitle;
            else v = p.sub < 0 ? tr("Off") : d.subtitleStreams[p.sub].displayTitle;
            button(12, y, BW - 24, h, "", 12, f);
            icon(c == C_AUDIO ? Icon::Audio : Icon::Subtitles, 28, y + h / 2, 16, pal.text);
            icon(Icon::Back, 46, y + h / 2, 10, pal.textDim);
            textC(BW / 2 + 4, y + (h - 12) / 2 - 1, v, 12, pal.text, BW - 110);
            Ui::triangle(BW - 34, y + h / 2 - 4, BW - 34, y + h / 2 + 4, BW - 28, y + h / 2, pal.textDim);
        } else {
            const float w = (BW - 32) / 3.0f;
            const char* labels[3] = { tr("Favorite"), d.played ? tr("Watched") : tr("Not watched"),
                                      d.seriesId.empty() ? "" : tr("Episodes") };
            const Icon icons[3] = { Icon::Heart, Icon::Check, Icon::Grid };
            for (int k = 0; k < 3; ++k) {
                if (k == 2 && d.seriesId.empty()) continue;
                const bool on = (k == 0 && d.isFavorite) || (k == 1 && d.played);
                button(12 + k * (w + 4), y, w, h, labels[k], 10, f > 0 ? 0.5f : 0.0f, on, icons[k], true);
            }
        }
        y += h + 8;
    }
}

void Browse3ds::drawTracks(Page& p)
{
    const Ui::Palette& pal = Ui::pal();
    const float listY = BAR_H + 44, rowH = 30;
    button(12, BAR_H + 8, 140, 30, tr("Play"), 13, 0, true, Icon::Play, true);
    button(168, BAR_H + 8, 140, 30, tr("Shuffle"), 13, 0, false, Icon::Shuffle, true);
    GRRLIB_ClipDrawing(0, (int)listY * 2, BW * 2, (int)(BH - listY) * 2);
    for (int i = 0; i < (int)p.tracks.size(); ++i) {
        const float y = listY + i * rowH - p.vs.pos;
        if (y + rowH < listY || y > BH) continue;
        const JellyfinAudioItem& t = p.tracks[i];
        if (i == p.sel) Ui::roundRect(4, y + 1, BW - 8, rowH - 2, 6, Ui::alpha(pal.accent, 0.35f));
        char b[16];
        snprintf(b, sizeof(b), "%d", t.trackNumber > 0 ? t.trackNumber : i + 1);
        textR(28, y + 8, b, 12, pal.textDim);
        text(36, y + 8, t.name, 12, pal.text, BW - 36 - 50);
        textR(BW - 10, y + 8, fmtTime(t.runtimeTicks), 11, pal.textDim);
    }
    GRRLIB_ClipReset();
}

/* ---- one frame -------------------------------------------------------------------- */

bool Browse3ds::frame(ir_t& ir)
{
    ++frames;
    readTouch(ir, touch);
    dragAxis(touch);
    for (auto& p : stack) { if (p->job) p->job->poll(); if (p->job2) p->job2->poll(); }
    if (detailJob) detailJob->poll();
    for (size_t i = 0; i < s_orphans->size();) {
        if ((*s_orphans)[i]->done) { (*s_orphans)[i]->poll(); s_orphans->erase(s_orphans->begin() + i); }
        else ++i;
    }

    Page& p = page();
    const bool a = Input::isAJustPressed() && !touch.tap;

    /* the bar: back, title, search / settings / profiles */
    bottomSpace();
    background(BW, BH);
    static const Icon HOME_ICONS[3] = { Icon::Search, Icon::Gear, Icon::Person };
    static const Icon PAGE_ICONS[2] = { Icon::Search, Icon::Home };
    const bool home = p.type == Type::Home;
    std::string title = home ? "" : p.type == Type::Search ? std::string(tr("Search")) + ": " + p.title
                                  : p.type == Type::Detail && p.hasDetail ? p.detail.name : p.title;

    /* page input (before drawing: the taps act on what was drawn) */
    if (!p.loading) {
        switch (p.type) {
        case Type::Home:   updateHome(p, a); break;
        case Type::Grid:
        case Type::Search: updateGrid(p, a); break;
        case Type::Series: updateSeries(p, a); break;
        case Type::Detail: updateDetail(p, a); break;
        case Type::Tracks: updateTracks(p, a); break;
        }
    }

    /* drawing */
    if (p.loading) drawLoading();
    else switch (p.type) {
        case Type::Home:   drawHome(p); break;
        case Type::Grid:
        case Type::Search: drawGrid(p); break;
        case Type::Series: drawSeries(p); break;
        case Type::Detail: drawDetail(p); break;
        case Type::Tracks: drawTracks(p); break;
    }
    const int tapped = topBar(touch, title, !home, home ? HOME_ICONS : PAGE_ICONS, home ? 3 : 2);
    if (frames < messageUntil) {
        const Ui::Palette& pal = Ui::pal();
        const float w = textW(message, 12) + 24;
        Ui::roundRect((BW - w) / 2, BH - 34, w, 26, 13, pal.cardTop, pal.cardBottom);
        textC(BW / 2, BH - 28, message, 12, pal.text);
    }

    /* the top screen: what is selected */
    const JellyfinItem* sel = nullptr;
    JellyfinItem tmp;
    if (p.type == Type::Home && !p.rows.empty() && p.rows[p.row].sel < (int)p.rows[p.row].items.size())
        sel = &p.rows[p.row].items[p.rows[p.row].sel];
    else if ((p.type == Type::Grid || p.type == Type::Search) && p.sel < (int)p.items.size()) sel = &p.items[p.sel];
    else if (p.type == Type::Series) {
        if (p.sel < (int)p.episodes.size()) {
            const JellyfinEpisode& e = p.episodes[p.sel];
            tmp.id = e.id; tmp.name = e.name; tmp.type = "Episode";
            tmp.seriesName = p.item.type == "Series" ? p.item.name : p.item.seriesName;
            tmp.seriesId = p.item.type == "Series" ? p.item.id : p.item.seriesId;
            tmp.seasonNumber = e.seasonNumber; tmp.episodeNumber = e.indexNumber;
            tmp.playbackPositionTicks = e.playbackPositionTicks;
            sel = &tmp;
        } else sel = &p.item;
    } else if (p.type == Type::Detail || p.type == Type::Tracks) {
        tmp = p.item;
        if (p.hasDetail) {
            tmp.runtimeTicks = p.detail.runtimeTicks;
            tmp.year = p.detail.year;
            tmp.officialRating = p.detail.officialRating;
            tmp.playbackPositionTicks = p.detail.playbackPositionTicks;
        }
        sel = &tmp;
    }
    setTop(sel);

    Images3ds::endFrame();
    wiiSpace();

    /* the bar's taps and the buttons acting on the whole */
    if (tapped == -1 || (Input::isBPressed() && !touch.tap)) {
        if (stack.size() > 1) pop();
    }
    const bool searchKey = Input::is1Pressed();
    if ((home && tapped == 0) || (!home && tapped == 0) || searchKey) { search(); return false; }
    if (home && tapped == 1) { exitTo = Exit::Settings; return true; }
    if (home && tapped == 2) { exitTo = Exit::Profiles; return true; }
    if (!home && tapped == 1) { while (stack.size() > 1) pop(); }
    return pendingPlayIsMusic || !pendingPlayUrl.empty();
}
