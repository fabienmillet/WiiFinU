#pragma once
/* -----------------------------------------------------------------------
 * Browse3ds — browsing a Jellyfin server on the 3DS: home rows, libraries,
 * series (seasons, episodes), a film's or an episode's page, albums,
 * search.  The bottom (touch) screen is the navigation, the top one shows
 * what is selected (its backdrop or poster, title, details, synopsis).
 * Play fills the PlayRequest (App then runs the player, as on the Wii).
 * ----------------------------------------------------------------------- */
#include "../../../core/PlayRequest.h"
#include "Ui3ds.h"
#include <map>
#include <memory>
#include <functional>

class Browse3ds : public PlayRequest {
public:
    enum class Exit { None, Profiles, Settings };
    struct Job;   /* a request to the server on a thread (Browse3ds.cpp) */

    Browse3ds(JellyfinClient& client, const JellyfinAuth& auth, const std::string& serverUrl,
              const std::string& userName);
    ~Browse3ds();

    /* One frame: input then drawing (both screens).  true: App has
     * something to do (pendingPlay*, pendingPlayIsMusic, exitTo). */
    bool frame(ir_t& ir);
    Exit exitTo = Exit::None;
    /* back from playing: the resume points and rows again */
    void onPlaybackFinished();

private:
    struct Row {
        std::string title;
        std::vector<JellyfinItem> items;
        bool libraries = false;          /* My Media: wide tiles */
        Ui3ds::Scroller hs;
        int sel = 0;
    };
    enum class Type { Home, Grid, Series, Detail, Tracks, Search };
    struct Page {
        Type type = Type::Home;
        std::string title;
        JellyfinItem item;               /* the library, series, album, film... */
        bool loading = true;
        std::shared_ptr<Job> job;
        /* Home */
        std::vector<Row> rows;
        int row = 0;
        /* Grid / Search: items; Series: episodes */
        std::vector<JellyfinItem> items;
        int total = 0;
        bool more = false;               /* Grid: more pages to fetch */
        int sel = 0;
        Ui3ds::Scroller vs;
        /* Series */
        std::vector<JellyfinSeason> seasons;
        int season = 0;
        Ui3ds::Scroller ss;              /* the seasons' chips */
        std::vector<JellyfinEpisode> episodes;
        /* Detail */
        JellyfinItemDetail detail;
        bool hasDetail = false;
        int audio = 0, sub = -1, focus = 0;
        /* Tracks */
        std::vector<JellyfinAudioItem> tracks;
    };

    JellyfinClient& client;
    JellyfinAuth auth;
    std::string serverUrl, userName;
    std::vector<std::unique_ptr<Page>> stack;
    Ui3ds::Touch touch;
    Ui3ds::TopInfo top;
    std::map<std::string, JellyfinItemDetail> details;   /* synopses of focused items */
    std::shared_ptr<Job> detailJob;
    std::string detailWanted;
    unsigned focusSince = 0, frames = 0;
    std::string message;                 /* a short notice ("Couldn't play") */
    unsigned messageUntil = 0;

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
    void drawLoading();

    void setTop(const JellyfinItem* it);
    void play(Page& p, bool fromStart);
    void playEpisode(Page& p, int idx);
    void playTracks(Page& p, int from, bool shuffle);
    void search();
    void notice(const std::string& s);
};
