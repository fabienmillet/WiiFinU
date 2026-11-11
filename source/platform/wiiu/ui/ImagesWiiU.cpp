#include "ImagesWiiU.h"
#include "../../../ui/JpegTexture.h"
#include "../GfxWiiU.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>
#include <unistd.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/system.h>

namespace ImagesWiiU {

namespace {

enum class St { Queued, Loading, Ready, Failed };

struct Entry {
    JellyfinItem item;
    Kind kind;
    St st = St::Queued;
    GRRLIB_texImg* tex = nullptr;
    unsigned used = 0;        /* frame last asked */
    unsigned asked = 0;       /* order asked (the newest loads first) */
};

JellyfinClient* s_client = nullptr;
JellyfinAuth    s_auth;
std::string     s_server;
std::map<std::string, Entry> s_map;
mutex_t  s_lock = LWP_MUTEX_NULL;
/* two loaders, each with its own connection to the server (beside the one
 * the pages' lists use) */
const int LOADERS = 2;
lwp_t    s_thread[LOADERS] = { LWP_THREAD_NULL, LWP_THREAD_NULL };
volatile bool s_quit = false;
unsigned s_frame = 0, s_ask = 0;

const size_t BUDGET_POSTERS = 160, BUDGET_BACKDROPS = 5, BUDGET_LARGE = 6;

std::string key(const std::string& id, Kind k)
{
    return id + (k == Kind::Poster ? "/p" : k == Kind::Thumb ? "/t" : k == Kind::Large ? "/l" : "/b");
}

void* worker(void* slot)
{
    JellyfinClient::setThreadConnection((int)(intptr_t)slot);
    while (!s_quit) {
        /* the newest asked, still wanted (asked lately) */
        std::string k;
        JellyfinItem item;
        Kind kind = Kind::Poster;
        LWP_MutexLock(s_lock);
        unsigned best = 0;
        for (auto& e : s_map)
            if (e.second.st == St::Queued && s_frame - e.second.used < 30 && e.second.asked >= best) {
                best = e.second.asked;
                k = e.first;
            }
        if (!k.empty()) {
            Entry& e = s_map[k];
            e.st = St::Loading;
            item = e.item;
            kind = e.kind;
        }
        LWP_MutexUnlock(s_lock);
        if (k.empty()) { usleep(30 * 1000); continue; }

        std::string bytes;
        bool ok;
        if (kind == Kind::Backdrop) {
            /* the TV's own size: 1280x720 or 1920x1080 */
            int w = 1280, h = 720;
            GfxWiiU::tvSize(w, h);
            ok = s_client->getItemBackdropBytes(s_server, s_auth, item, w, h, bytes);
        } else if (kind == Kind::Thumb)
            ok = s_client->getItemImageBytes(s_server, s_auth, item.id, 480, 270, bytes);
        else if (kind == Kind::Large)
            ok = s_client->getItemImageBytes(s_server, s_auth, item.id, 600, 900, bytes);
        else
            ok = s_client->getItemImageBytes(s_server, s_auth, item.id, 240, 360, bytes);
        GRRLIB_texImg* tex = ok && !bytes.empty() ? loadJPEGTexture((const u8*)bytes.data(), (u32)bytes.size()) : nullptr;

        LWP_MutexLock(s_lock);
        auto it = s_map.find(k);
        if (it != s_map.end() && it->second.st == St::Loading) {
            it->second.tex = tex;
            it->second.st = tex ? St::Ready : St::Failed;
        } else if (tex) {
            GRRLIB_FreeTexture(tex);   /* dropped meanwhile */
        }
        LWP_MutexUnlock(s_lock);
    }
    return nullptr;
}

} // namespace

void start(JellyfinClient& client, const JellyfinAuth& auth, const std::string& serverUrl)
{
    stop();
    s_client = &client;
    s_auth = auth;
    s_server = serverUrl;
    if (s_lock == LWP_MUTEX_NULL) LWP_MutexInit(&s_lock, false);
    s_quit = false;
    JellyfinClient::setThreadConnection(0);   /* (makes the locks, before the loaders) */
    for (int i = 0; i < LOADERS; ++i)
        LWP_CreateThread(&s_thread[i], worker, (void*)(intptr_t)(i + 1), nullptr, 64 * 1024, 40);
}

void stop()
{
    s_quit = true;
    for (int i = 0; i < LOADERS; ++i)
        if (s_thread[i] != LWP_THREAD_NULL) {
            LWP_JoinThread(s_thread[i], nullptr);
            s_thread[i] = LWP_THREAD_NULL;
        }
    if (s_lock == LWP_MUTEX_NULL) return;
    LWP_MutexLock(s_lock);
    for (auto& e : s_map) if (e.second.tex) GRRLIB_FreeTexture(e.second.tex);
    s_map.clear();
    LWP_MutexUnlock(s_lock);
}

GRRLIB_texImg* get(const JellyfinItem& item, Kind kind)
{
    if (item.id.empty() || s_lock == LWP_MUTEX_NULL) return nullptr;
    const std::string k = key(item.id, kind);
    LWP_MutexLock(s_lock);
    auto it = s_map.find(k);
    if (it == s_map.end()) {
        Entry e;
        e.item = item;
        e.kind = kind;
        it = s_map.emplace(k, e).first;
    }
    Entry& e = it->second;
    if (e.used != s_frame) e.asked = ++s_ask;
    e.used = s_frame;
    GRRLIB_texImg* tex = e.st == St::Ready ? e.tex : nullptr;
    LWP_MutexUnlock(s_lock);
    return tex;
}

void endFrame()
{
    if (s_lock == LWP_MUTEX_NULL) return;
    ++s_frame;
    if ((s_frame & 15) != 0) return;
    LWP_MutexLock(s_lock);
    /* over budget: the least lately used, not used for a second */
    for (int pass = 0; pass < 3; ++pass) {
        auto cls = [](Kind k) { return k == Kind::Backdrop ? 1 : k == Kind::Large ? 2 : 0; };
        std::vector<std::pair<unsigned, std::string>> ready;
        for (auto& e : s_map)
            if (cls(e.second.kind) == pass && e.second.st == St::Ready)
                ready.push_back({ e.second.used, e.first });
        const size_t budget = pass == 1 ? BUDGET_BACKDROPS : pass == 2 ? BUDGET_LARGE : BUDGET_POSTERS;
        if (ready.size() <= budget) continue;
        std::sort(ready.begin(), ready.end());
        for (size_t i = 0; i + budget < ready.size(); ++i) {
            Entry& e = s_map[ready[i].second];
            if (s_frame - e.used < 60) break;
            GRRLIB_FreeTexture(e.tex);
            s_map.erase(ready[i].second);
        }
    }
    /* failed and queued ones not wanted any more: forgotten */
    for (auto it = s_map.begin(); it != s_map.end();) {
        if ((it->second.st == St::Queued || it->second.st == St::Failed) && s_frame - it->second.used > 120)
            it = s_map.erase(it);
        else ++it;
    }
    LWP_MutexUnlock(s_lock);
}

}
