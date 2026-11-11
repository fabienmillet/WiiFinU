/* -----------------------------------------------------------------------
 * libogc's system calls on the Wii U: threads (LWP) and mutexes on
 * coreinit's, the time base, SYS_Report, the console's settings, the
 * network (the system's connection).
 * ----------------------------------------------------------------------- */
#include <coreinit/debug.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/userconfig.h>
#include <gx2/event.h>
#include <nn/ac.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <ogc/conf.h>
#include <ogc/video.h>
#include <ogc/ios.h>
#include <ogc/if_config.h>
#include <network.h>
#include "GfxWiiU.h"

extern "C" {

/* ---- time ------------------------------------------------------------ */

u64 gettime(void)
{
    return (u64)OSTicksToMicroseconds(OSGetSystemTime());
}

/* ---- threads ------------------------------------------------------------- */

struct Lwp {
    OSThread thread;          /* first: coreinit wants it 8-aligned (memalign) */
    void*  (*entry)(void*);
    void*    arg;
    void*    result;
    void*    stack;
};

static int lwpEntry(int argc, void* argv)
{
    (void)argc;
    Lwp* l = (Lwp*)argv;
    l->result = l->entry(l->arg);
    return 0;
}

/* the Wii's priorities (0..127, higher first; the main thread 64) to
 * coreinit's (0..31, lower first; the main thread 16) */
static int osPrio(u8 prio)
{
    int p = 16 - ((int)prio - 64) / 8;
    if (prio > 64 && p >= 16) p = 15;
    if (prio < 64 && p <= 16) p = 17;
    if (p < 1) p = 1;
    if (p > 30) p = 30;
    return p;
}

s32 LWP_CreateThread(lwp_t* out, void* (*entry)(void*), void* arg, void* stackbase,
                     u32 stack_size, u8 prio)
{
    (void)stackbase;
    Lwp* l = (Lwp*)memalign(16, sizeof(Lwp));
    if (!l) return -1;
    memset(l, 0, sizeof(*l));
    l->entry = entry;
    l->arg = arg;
    u32 stack = stack_size < 128 * 1024 ? 128 * 1024 : stack_size;
    l->stack = memalign(16, stack);
    if (!l->stack) { free(l); return -1; }
    /* any core: the three share the work */
    if (!OSCreateThread(&l->thread, lwpEntry, 0, l, (u8*)l->stack + stack, stack, osPrio(prio),
                        OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        SYS_Report("[OgcWiiU] OSCreateThread failed (stack %u)\n", (unsigned)stack);
        free(l->stack);
        free(l);
        if (out) *out = LWP_THREAD_NULL;
        return -1;
    }
    OSSetThreadName(&l->thread, "WiiFin");
    OSResumeThread(&l->thread);
    if (out) *out = (lwp_t)(uintptr_t)l;
    return 0;
}

s32 LWP_JoinThread(lwp_t t, void** value)
{
    if (t == LWP_THREAD_NULL || t == 0) return -1;
    Lwp* l = (Lwp*)(uintptr_t)t;
    int r = 0;
    OSJoinThread(&l->thread, &r);
    if (value) *value = l->result;
    free(l->stack);
    free(l);
    return 0;
}

lwp_t LWP_GetSelf(void) { return (lwp_t)(uintptr_t)OSGetCurrentThread(); }

void LWP_SetThreadPriority(lwp_t t, u32 prio)
{
    OSThread* th = (t == LWP_THREAD_NULL || t == 0) ? OSGetCurrentThread() : &((Lwp*)(uintptr_t)t)->thread;
    OSSetThreadPriority(th, osPrio((u8)prio));
}

void LWP_YieldThread(void) { OSYieldThread(); }
s32 LWP_SuspendThread(lwp_t t) { (void)t; return -1; }
s32 LWP_ResumeThread(lwp_t t) { (void)t; return -1; }

/* ---- mutexes (coreinit's are recursive) ----------------------------------- */

s32 LWP_MutexInit(mutex_t* m, bool recursive)
{
    (void)recursive;
    OSMutex* x = (OSMutex*)memalign(16, sizeof(OSMutex));
    if (!x) { *m = LWP_MUTEX_NULL; return -1; }
    OSInitMutex(x);
    *m = (mutex_t)(uintptr_t)x;
    return 0;
}

s32 LWP_MutexDestroy(mutex_t m)
{
    if (m == LWP_MUTEX_NULL || m == 0) return -1;
    free((OSMutex*)(uintptr_t)m);
    return 0;
}

s32 LWP_MutexLock(mutex_t m)
{
    if (m == LWP_MUTEX_NULL || m == 0) return -1;
    OSLockMutex((OSMutex*)(uintptr_t)m);
    return 0;
}

s32 LWP_MutexTryLock(mutex_t m)
{
    if (m == LWP_MUTEX_NULL || m == 0) return -1;
    return OSTryLockMutex((OSMutex*)(uintptr_t)m) ? 0 : 1;
}

s32 LWP_MutexUnlock(mutex_t m)
{
    if (m == LWP_MUTEX_NULL || m == 0) return -1;
    OSUnlockMutex((OSMutex*)(uintptr_t)m);
    return 0;
}

/* ---- SYS ---------------------------------------------------------------- */

/* Log.cpp wraps SYS_Report (--wrap): this is the "real" one, the system's
 * debug output (Cemu's log shows it) */
void SYS_Report(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OSReport("%s", buf);
}

void SYS_ResetSystem(s32 reset, u32 type, s32 force)
{
    (void)reset; (void)type; (void)force;
    /* the Wii U's HOME menu and power button are the system's: WiiFin
     * leaves by returning from main (App.cpp, GfxWiiU::exit) */
}

/* (the logs' memory figures) the default heap's free bytes */
u32 SYS_GetArena1Size(void)
{
    struct mallinfo mi = mallinfo();
    return (u32)mi.fordblks;
}
u32 SYS_GetArena2Size(void) { return 0; }

/* ---- settings ------------------------------------------------------------ */

static bool sysConfigU32(const char* name, u32& out)
{
    UCHandle h = UCOpen();
    if (h < 0) return false;
    alignas(64) static u32 value;
    alignas(64) static UCSysConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.name, name, sizeof(cfg.name) - 1);
    cfg.dataType = UC_DATATYPE_UNSIGNED_INT;
    cfg.dataSize = 4;
    cfg.data = &value;
    UCError e = UCReadSysConfig(h, 1, &cfg);
    UCClose(h);
    if (e != UC_ERROR_OK) return false;
    out = value;
    return true;
}

s32 CONF_GetLanguage(void)
{
    u32 lang = 1;
    sysConfigU32("cafe.language", lang);
    switch (lang) {
        case 0:  return CONF_LANG_JAPANESE;
        case 2:  return CONF_LANG_FRENCH;
        case 3:  return CONF_LANG_GERMAN;
        case 4:  return CONF_LANG_ITALIAN;
        case 5:  return CONF_LANG_SPANISH;
        case 6:  return CONF_LANG_SIMP_CHINESE;
        case 7:  return CONF_LANG_KOREAN;
        case 8:  return CONF_LANG_DUTCH;
        case 9:  return CONF_LANG_PORTUGUESE;
        case 10: return CONF_LANG_RUSSIAN;
        case 11: return CONF_LANG_TRAD_CHINESE;
        default: return CONF_LANG_ENGLISH;
    }
}

s32 CONF_GetRegion(void)
{
    MCPRegion r = MCP_REGION_EUROPE;
    int h = MCP_Open();
    if (h >= 0) {
        alignas(64) static MCPSysProdSettings s;
        if (MCP_GetSysProdSettings(h, &s) == 0) r = s.product_area;
        MCP_Close(h);
    }
    if (r & MCP_REGION_JAPAN) return CONF_REGION_JP;
    if (r & MCP_REGION_USA) return CONF_REGION_US;
    if (r & (MCP_REGION_CHINA | MCP_REGION_TAIWAN)) return CONF_REGION_CN;
    if (r & MCP_REGION_KOREA) return CONF_REGION_KR;
    return CONF_REGION_EU;
}

/* one id per console (App.cpp hashes it): its serial number */
s32 ES_GetDeviceID(u32* id)
{
    int h = MCP_Open();
    if (h < 0) return -1;
    alignas(64) static MCPSysProdSettings s;
    s32 r = MCP_GetSysProdSettings(h, &s);
    MCP_Close(h);
    if (r != 0) return -1;
    u32 hash = 2166136261u;
    for (char c : s.code_id)   { if (!c) break; hash = (hash ^ (u8)c) * 16777619u; }
    for (char c : s.serial_id) { if (!c) break; hash = (hash ^ (u8)c) * 16777619u; }
    *id = hash;
    return 0;
}

/* ---- video ---------------------------------------------------------------- */

static GXRModeObj s_mode = { 0, 640, 480, 480, 0, 0, 640, 480 };
GXRModeObj* rmode = &s_mode;

void VIDEO_WaitVSync(void) { if (!GfxWiiU::exiting()) GX2WaitForVsync(); }

/* ---- network ---------------------------------------------------------------- */

/* A name's first IPv4 address, as gethostbyname gives it (a few results
 * kept in turn: two threads resolving at once each keep theirs) */
struct hostent* wiiu_gethostbyname(const char* name)
{
    struct Slot { struct hostent he; struct in_addr addr; char* list[2]; };
    static Slot s_slots[8];
    static unsigned s_next = 0;
    static OSMutex s_lock;
    static bool s_lockInit = false;
    if (!s_lockInit) { OSInitMutex(&s_lock); s_lockInit = true; }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &res) != 0 || !res) return nullptr;
    struct hostent* out = nullptr;
    for (struct addrinfo* a = res; a && !out; a = a->ai_next) {
        if (a->ai_family != AF_INET || !a->ai_addr) continue;
        OSLockMutex(&s_lock);
        Slot& s = s_slots[s_next++ % 8];
        OSUnlockMutex(&s_lock);
        s.addr = ((struct sockaddr_in*)a->ai_addr)->sin_addr;
        s.list[0] = (char*)&s.addr;
        s.list[1] = nullptr;
        memset(&s.he, 0, sizeof(s.he));
        s.he.h_addrtype = AF_INET;
        s.he.h_length = sizeof(s.addr);
        s.he.h_addr_list = s.list;
        out = &s.he;
    }
    freeaddrinfo(res);
    return out;
}

/* wut's sockets start with the program */
s32 net_init(void) { return 0; }
void net_deinit(void) {}

/* The Wii U's connection is the system's: the address it has, asking for
 * it (the connection set in System Settings) for a while */
s32 if_config(char* ip, char* mask, char* gw, bool dhcp, int retries)
{
    (void)dhcp;
    static bool acUp = false;
    if (!acUp) acUp = NNResult_IsSuccess(ACInitialize());
    for (int i = 0; i <= retries * 2; ++i) {
        BOOL connected = FALSE;
        ACIsApplicationConnected(&connected);
        if (!connected) ACConnect();
        u32 addr = 0;
        if (NNResult_IsSuccess(ACGetAssignedAddress(&addr)) && addr != 0) {
            struct in_addr a;
            a.s_addr = htonl(addr);
            if (ip) strcpy(ip, inet_ntoa(a));
            if (mask) strcpy(mask, "255.255.255.0");
            if (gw) strcpy(gw, "0.0.0.0");
            return 0;
        }
        usleep(500 * 1000);
    }
    /* no address told (an emulator): the sockets may still work */
    if (ip) strcpy(ip, "0.0.0.0");
    if (mask) strcpy(mask, "255.255.255.0");
    if (gw) strcpy(gw, "0.0.0.0");
    return 0;
}

} // extern "C"
