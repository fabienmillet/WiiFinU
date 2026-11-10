#pragma once
/* -----------------------------------------------------------------------
 * Connect3ds — signing in to a server on the 3DS: its address, user name
 * and password typed with the 3DS's keyboard, Quick Connect (a code to
 * enter in another Jellyfin app), or a server found on the network.
 * The same contract as ConnectView (App's runConnect).
 * ----------------------------------------------------------------------- */
#include "../../../ui/ConnectView.h"
#include "Ui3ds.h"
#include "Job3ds.h"

class Connect3ds {
public:
    explicit Connect3ds(JellyfinClient& client);
    ~Connect3ds();
    void setFields(const std::string& url, const std::string& user);
    /* one frame: input and drawing (both screens) */
    ConnectResult update(ir_t& ir);
    void render(ir_t&) {}

    JellyfinAuth auth;
    std::string  serverUrl;
    std::string  username;

private:
    enum class Tab { SignIn, Quick, Find };
    JellyfinClient& client;
    Tab tab = Tab::SignIn;
    std::string url, user, pass;
    std::string status;
    bool statusError = false;
    std::shared_ptr<Job3ds> job;
    std::string busy;                      /* what the job does ("Signing in") */
    ConnectResult result = ConnectResult::None;
    /* Quick Connect */
    QuickConnectResult qc;
    bool qcWaiting = false;
    unsigned qcNextPoll = 0;
    /* found servers */
    std::vector<DiscoveredServer> servers;
    bool searched = false;
    int focus = 0;
    unsigned frames = 0;
    Ui3ds::Touch touch;
    Ui3ds::TopInfo top;

    bool ensureNetwork();
    void type(std::string& field, const char* hint, bool password, bool url);
    void signIn();
    void quickStart();
    void find();
    std::string fullUrl() const;
};
