#pragma once
/* A request to the server on a thread of its own (work), its result
 * applied by the main thread once done (apply, from poll()). */
#include <functional>
#include <memory>
#include <ogc/lwp.h>

struct JobWiiU {
    lwp_t th = LWP_THREAD_NULL;
    volatile bool done = false;
    std::function<void()> work, apply;
    static void* run(void* p) { JobWiiU* j = (JobWiiU*)p; j->work(); j->done = true; return nullptr; }
    void start() { LWP_CreateThread(&th, run, this, nullptr, 128 * 1024, 40); }
    bool running() const { return th != LWP_THREAD_NULL; }
    bool poll()
    {
        if (th == LWP_THREAD_NULL || !done) return false;
        LWP_JoinThread(th, nullptr);
        th = LWP_THREAD_NULL;
        if (apply) apply();
        return true;
    }
    ~JobWiiU() { if (th != LWP_THREAD_NULL) LWP_JoinThread(th, nullptr); }
};

inline std::shared_ptr<JobWiiU> runJobWiiU(std::function<void()> work, std::function<void()> apply)
{
    auto j = std::make_shared<JobWiiU>();
    j->work = std::move(work);
    j->apply = std::move(apply);
    j->start();
    return j;
}
