#include "core/App.h"
#include "core/Log.h"
#include <ogc/lwp.h>
#include <ogc/system.h>
#ifdef __3DS__
#include "platform/3ds/Platform3ds.h"
#endif

int main(int argc, char* argv[]) {
#ifdef __3DS__
    Platform3ds::init();
#endif
    Log::init();
    SYS_Report("[DBG] main thread self=%p\n", (void*)(uintptr_t)LWP_GetSelf());
    App app;
    app.init(argc > 0 ? argv[0] : nullptr);
    app.run();
    return 0;
}
