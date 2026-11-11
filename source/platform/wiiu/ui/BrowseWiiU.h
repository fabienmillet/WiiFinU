#pragma once
/* -----------------------------------------------------------------------
 * BrowseWiiU — browsing a Jellyfin server on the Wii U, in HD and on its
 * two screens, as the Wii U Menu does: home rows, libraries, series
 * (seasons, episodes), a film's or an episode's page, albums, search.
 *
 *   - a TV (the screens' Remote mode): the GamePad is the navigation (its
 *     touch screen, its buttons) and the TV shows what is selected, big:
 *     its backdrop, title, details, synopsis, and the row around it;
 *   - the GamePad alone (Mirror mode): the navigation, with what the TV
 *     would say about the item kept short on it.
 *
 * Play fills the PlayRequest (App then runs the player, as on the Wii).
 * ----------------------------------------------------------------------- */
#include "../../../core/PlayRequest.h"
#include "UiWiiU.h"
#include "JobWiiU.h"
#include <map>
#include <memory>
#include <functional>

class BrowseWiiU : public PlayRequest {
public:
    enum class Exit { None, Profiles, Settings };
    using Job = JobWiiU;

    BrowseWiiU(JellyfinClient& client, const JellyfinAuth& auth, const std::string& serverUrl,
               const std::string& userName);
    ~BrowseWiiU();

    /* One frame: input then drawing (both screens).  true: App has
     * something to do (pendingPlay*, pendingPlayIsMusic, exitTo). */
    bool frame(ir_t& ir);
    Exit exitTo = Exit::None;
    /* back from playing or from the settings: the resume points and rows again */
    void onPlaybackFinished();

private:
    struct Row {
        std::string title;
        std::vector<JellyfinItem> items;
        bool libraries = false;          /* My Media: wide tiles */
        bool latest = false;             /* a library's "Latest in" */
        UiWiiU::Scroller hs;
        int sel = 0;
    };
    enum class Type { Home, Grid, Series, Detail, Tracks, Search };
    struct Page {
        Type type = Type::Home;
        std::string title;
        JellyfinItem item;               /* the library, series, album, film... */
        bool loading = true;
        std::shared_ptr<Job> job;
        std::shared_ptr<Job> job2;       /* Home: its "Latest in" rows, after the others */
        int gen = 0;
        /* Home */
        std::vector<Row> rows;
        int row = 0;
        int bar = -1;                    /* the bar's button the D-pad is on, -1 none */
        /* Grid / Search: items; Series: episodes */
        std::vector<JellyfinItem> items;
        int total = 0;
        bool more = false;
        int sel = 0;
        UiWiiU::Scroller vs;
        /* Series */
        std::vector<JellyfinSeason> seasons;
        int season = 0;
        UiWiiU::Scroller ss;
        std::vector<JellyfinEpisode> episodes;
        /* Detail */
        JellyfinItemDetail detail;
        bool hasDetail = false;
        int audio = 0, sub = -1, focus = 0, action = 0;
        /* Tracks */
        std::vector<JellyfinAudioItem> tracks;
    };
    /* what the TV shows about the selection */
    struct Info {
        std::string title, line1, line2, overview;
        JellyfinItem picture;            /* the backdrop's item */
        JellyfinItem poster;             /* the poster's item (no backdrop) */
        bool  hasPoster = false, square = false, wide = false;
        float progress = -1;
    };

    JellyfinClient& client;
    JellyfinAuth auth;
    std::string serverUrl, userName;
    std::vector<std::unique_ptr<Page>> stack;
    UiWiiU::Touch touch;
    Info info;
    std::map<std::string, JellyfinItemDetail> details;   /* synopses of focused items */
    std::shared_ptr<Job> detailJob;
    std::string detailWanted;
    unsigned focusSince = 0, frames = 0;
    std::string message;
    unsigned messageUntil = 0;
    bool searching = false;
    std::string searchText;
    bool dual = false;                   /* the TV shows the selection, the GamePad the navigation */
    float tvScroll = 0;                  /* the TV's row of tiles: where it is scrolled */
    std::string tvRowKey;                /* ... and which row it shows (another: no animation) */

    Page& page() { return *stack.back(); }
    void push(std::unique_ptr<Page> p);
    void pop();
    void open(const JellyfinItem& it);
    void load(Page& p);
    void loadMore(Page& p);

    void updateHome(Page& p, bool a);
    void updateGrid(Page& p, bool a);
    void updateSeries(Page& p, bool a);
    void updateDetail(Page& p, bool a);
    void updateTracks(Page& p, bool a);
    void drawHome(Page& p);
    void drawGrid(Page& p);
    void drawSeries(Page& p);
    void drawDetail(Page& p);
    void drawTracks(Page& p);
    int  drawBar(Page& p);
    void drawFooter(Page& p);

    /* the TV */
    const JellyfinItem* selection(Page& p, JellyfinItem& tmp);
    void makeInfo(const JellyfinItem* it);
    void drawTV(Page& p);
    void drawTVCarousel(Page& p);
    void drawTVDetail(Page& p);
    void drawTVTracks(Page& p);

    void play(Page& p, bool fromStart);
    void playTracks(Page& p, int from, bool shuffle);
    void searchFrame();
    void notice(const std::string& s);
};
