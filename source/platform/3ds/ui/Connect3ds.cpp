#include "Connect3ds.h"
#include "../Gfx3ds.h"
#include "../../../ui/Ui.h"
#include "../../../input/Input.h"
#include "../../../core/Lang.h"
#include "../../../core/SoundFX.h"

#include <3ds.h>
#include <ogc/system.h>

using namespace Ui3ds;

Connect3ds::Connect3ds(JellyfinClient& c) : client(c)
{
    url = "http://";
}

Connect3ds::~Connect3ds()
{
    setTopInfo(nullptr);
}

void Connect3ds::setFields(const std::string& u, const std::string& n)
{
    url = u;
    while (url.size() > 1 && url.back() == '/') url.pop_back();
    user = n;
}

std::string Connect3ds::fullUrl() const
{
    std::string u = url;
    while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
    while (!u.empty() && u[0] == ' ') u.erase(0, 1);
    if (u.find("://") == std::string::npos) u = "http://" + u;
    return u;
}

/* the network (the console's Wi-Fi) before the first request */
bool Connect3ds::ensureNetwork()
{
    if (client.isNetworkReady()) return true;
    if (!client.networkBusy()) client.startNetwork();
    return false;
}

void Connect3ds::type(std::string& field, const char* hint, bool password, bool isUrl)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", field.c_str());
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, sizeof(buf) - 1);
    swkbdSetHintText(&kb, hint);
    swkbdSetInitialText(&kb, buf);
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, tr("Cancel"), false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK", true);
    if (password) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    if (isUrl) swkbdSetFeatures(&kb, SWKBD_PREDICTIVE_INPUT);
    Gfx3ds::endFrameForApplet();
    if (swkbdInputText(&kb, buf, sizeof(buf)) == SWKBD_BUTTON_RIGHT) field = buf;
}

void Connect3ds::signIn()
{
    if (!ensureNetwork()) { busy = tr("Connecting to the network"); return; }
    const std::string u = fullUrl(), n = user, pw = pass;
    struct Out { bool ok = false; JellyfinAuth auth; std::string err; };
    auto out = std::make_shared<Out>();
    JellyfinClient* cl = &client;
    busy = tr("Signing in");
    status.clear();
    job = runJob3ds([=]() {
        out->ok = cl->authenticate(u, n, pw, out->auth);
        if (out->ok) { std::string sn; if (cl->getServerName(u, out->auth, sn)) out->auth.serverName = sn; }
        else out->err = cl->lastError();
    }, [=]() {
        busy.clear();
        if (!out->ok) { status = out->err; statusError = true; SoundFX::play(SoundFX::FX::Back); return; }
        auth = out->auth;
        serverUrl = u;
        username = n;
        result = ConnectResult::Success;
    });
}

void Connect3ds::quickStart()
{
    if (!ensureNetwork()) { busy = tr("Connecting to the network"); return; }
    const std::string u = fullUrl();
    auto res = std::make_shared<QuickConnectResult>();
    auto ok = std::make_shared<bool>(false);
    auto err = std::make_shared<std::string>();
    JellyfinClient* cl = &client;
    busy = tr("Asking the server for a code");
    status.clear();
    job = runJob3ds([=]() { *ok = cl->quickConnectInitiate(u, *res); if (!*ok) *err = cl->lastError(); },
                    [=]() {
        busy.clear();
        if (!*ok) { status = *err; statusError = true; return; }
        qc = *res;
        qcWaiting = true;
        qcNextPoll = frames + 120;
    });
}

void Connect3ds::find()
{
    if (!ensureNetwork()) { busy = tr("Connecting to the network"); return; }
    auto out = std::make_shared<std::vector<DiscoveredServer>>();
    JellyfinClient* cl = &client;
    busy = tr("Looking for servers");
    job = runJob3ds([=]() { cl->discoverServers(*out); }, [=]() {
        busy.clear();
        servers = *out;
        searched = true;
    });
}

ConnectResult Connect3ds::update(ir_t& ir)
{
    ++frames;
    readTouch(ir, touch);
    if (job) job->poll();
    const bool working = job && job->running();
    /* the network came up: what was asked goes on */
    if (!working && !busy.empty() && busy == tr("Connecting to the network") && !client.networkBusy()) {
        busy.clear();
        if (!client.takeNetworkResult() && !client.isNetworkReady()) { status = client.lastError(); statusError = true; }
        else if (tab == Tab::SignIn) signIn();
        else if (tab == Tab::Quick) quickStart();
        else find();
    }
    /* Quick Connect: is the code approved yet? */
    if (qcWaiting && !working && frames >= qcNextPoll) {
        const std::string u = fullUrl(), secret = qc.secret;
        auto approved = std::make_shared<bool>(false);
        auto a = std::make_shared<JellyfinAuth>();
        auto err = std::make_shared<std::string>();
        JellyfinClient* cl = &client;
        job = runJob3ds([=]() {
            QuickConnectResult r;
            if (!cl->quickConnectCheck(u, secret, r) || !r.authenticated) return;
            *approved = cl->quickConnectAuthenticate(u, secret, *a);
            if (!*approved) { *err = cl->lastError(); return; }
            std::string sn;
            if (cl->getServerName(u, *a, sn)) a->serverName = sn;
        }, [=]() {
            if (*approved) {
                qcWaiting = false;
                auth = *a;
                serverUrl = u;
                username.clear();   /* Quick Connect: no user name typed */
                result = ConnectResult::Success;
            } else if (!err->empty()) { qcWaiting = false; status = *err; statusError = true; }
            qcNextPoll = frames + 120;
        });
    }

    const Ui::Palette& p = Ui::pal();
    const bool a = Input::isAJustPressed() && !touch.tap;
    bottomSpace();
    background(BW, BH);
    const int tapped = topBar(touch, tr("Add an account"), true, nullptr, 0);

    /* tabs */
    const char* names[3] = { tr("Sign in"), tr("Quick Connect"), tr("Find") };
    const float tw = (BW - 24) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const bool on = (int)tab == i;
        const float x = 12 + i * tw;
        Ui::roundRect(x + 2, BAR_H + 6, tw - 4, 24, 12, on ? p.accent : p.cardTop, on ? p.accentDark : p.cardBottom);
        textC(x + tw / 2, BAR_H + 11, names[i], 11, on ? p.textOnAccent : p.text, tw - 10);
        if (!working && hit(touch, x, BAR_H + 4, tw, 28) && (int)tab != i) {
            tab = (Tab)i; focus = 0; status.clear(); SoundFX::play(SoundFX::FX::Page);
            if (tab == Tab::Find && !searched) find();
        }
    }
    if (!working && Input::isRPressed() && tab != Tab::Find) { tab = (Tab)((int)tab + 1); focus = 0; if (tab == Tab::Find && !searched) find(); }
    if (!working && Input::isLPressed() && tab != Tab::SignIn) { tab = (Tab)((int)tab - 1); focus = 0; }

    const float y0 = BAR_H + 38;
    auto field = [&](int i, float y, const char* label, const std::string& value, bool secret) {
        const bool f = focus == i;
        text(16, y, label, 10, p.textDim);
        Ui::roundRect(12, y + 13, BW - 24, 26, 8, p.field);
        Ui::roundBorder(12, y + 13, BW - 24, 26, 8, f ? 2.0f : 1.0f, f ? p.accent : p.fieldBorder);
        std::string shown = secret ? std::string(value.size(), '*') : value;
        text(20, y + 19, shown, 12, p.text, BW - 40);
        return hit(touch, 12, y + 10, BW - 24, 32);
    };

    if (tab == Tab::SignIn) {
        bool t0 = field(0, y0, tr("Server address"), url, false);
        bool t1 = field(1, y0 + 44, tr("User name"), user, false);
        bool t2 = field(2, y0 + 88, tr("Password"), pass, true);
        button(12, y0 + 136, BW - 24, 34, tr("Sign in"), 14, focus == 3 ? 1.0f : 0.0f, true);
        if (!working) {
            if (Input::isUpPressed() && focus > 0) --focus;
            if (Input::isDownPressed() && focus < 3) ++focus;
            if (t0 || (a && focus == 0)) { focus = 0; type(url, tr("Server address (http://...:8096)"), false, true); }
            else if (t1 || (a && focus == 1)) { focus = 1; type(user, tr("User name"), false, false); }
            else if (t2 || (a && focus == 2)) { focus = 2; type(pass, tr("Password"), true, false); }
            else if (hit(touch, 12, y0 + 136, BW - 24, 34) || (a && focus == 3)) signIn();
        }
    } else if (tab == Tab::Quick) {
        bool t0 = field(0, y0, tr("Server address"), url, false);
        if (qcWaiting) {
            textC(BW / 2, y0 + 50, tr("Enter this code in Quick Connect"), 11, p.textDim);
            textC(BW / 2, y0 + 50 + 16, tr("in another Jellyfin app signed in to this server:"), 11, p.textDim);
            textC(BW / 2, y0 + 92, qc.code, 30, p.accent);
            spinner(BW / 2, y0 + 150, 8);
        } else {
            paragraph(16, y0 + 48, BW - 32, tr("Quick Connect signs in without typing a password: "
                      "a code appears here, to approve in another Jellyfin app."), 11, p.textDim, 3);
            button(12, y0 + 104, BW - 24, 34, tr("Get a code"), 14, focus == 1 ? 1.0f : 0.0f, true);
        }
        if (!working) {
            if (Input::isUpPressed()) focus = 0;
            if (Input::isDownPressed()) focus = 1;
            if (t0 || (a && focus == 0)) { focus = 0; qcWaiting = false; type(url, tr("Server address (http://...:8096)"), false, true); }
            else if (!qcWaiting && (hit(touch, 12, y0 + 104, BW - 24, 34) || (a && focus == 1))) quickStart();
        }
    } else {
        if (servers.empty() && searched && !working)
            paragraph(16, y0 + 10, BW - 32, tr("No server answered on this network. Its address can be typed in Sign in."),
                      12, p.textDim, 3);
        for (int i = 0; i < (int)servers.size() && i < 4; ++i) {
            const float y = y0 + i * 40;
            const bool f = focus == i;
            Ui::roundRect(12, y, BW - 24, 36, 8, f ? Ui::alpha(p.accent, 0.4f) : p.cardTop, f ? Ui::alpha(p.accent, 0.4f) : p.cardBottom);
            text(22, y + 4, servers[i].name, 13, p.text, BW - 44);
            text(22, y + 20, servers[i].address, 10, p.textDim, BW - 44);
            if (!working && (hit(touch, 12, y, BW - 24, 36) || (a && f))) {
                url = servers[i].address;
                tab = Tab::SignIn;
                focus = 1;
                SoundFX::play(SoundFX::FX::Start);
            }
        }
        if (!working) {
            if (Input::isUpPressed() && focus > 0) --focus;
            if (Input::isDownPressed() && focus + 1 < (int)servers.size()) ++focus;
            if (Input::is2Pressed()) find();
        }
    }

    /* status: busy, or the last error */
    if (!busy.empty()) {
        const float w = textW(busy, 12) + 44;
        Ui::roundRect((BW - w) / 2, BH - 30, w, 24, 12, p.cardTop, p.cardBottom);
        spinner((BW - w) / 2 + 16, BH - 18, 6);
        text((BW - w) / 2 + 30, BH - 25, busy, 12, p.text);
    } else if (!status.empty()) {
        paragraph(12, BH - 30, BW - 24, status, 11, statusError ? p.danger : p.textDim, 2, 13);
    }

    /* the top screen: what to do */
    top = TopInfo();
    top.title = tr("Add an account");
    top.line1 = tab == Tab::SignIn ? tr("Your Jellyfin server's address, user name and password")
              : tab == Tab::Quick ? tr("Sign in with a code, no password typed")
              : tr("Jellyfin servers on this network");
    top.overview = tr("Tap a field to type it with the keyboard. L / R: the tabs. B: back.");
    setTopInfo(&top);
    wiiSpace();

    if (result != ConnectResult::None) return result;
    if (!working && (tapped == -1 || (Input::isBPressed() && !touch.tap))) return ConnectResult::Cancelled;
    return ConnectResult::None;
}
