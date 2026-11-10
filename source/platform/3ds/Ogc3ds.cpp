/* -----------------------------------------------------------------------
 * libogc's system calls on the 3DS: threads (LWP), mutexes, the time base,
 * SYS_Report, the console's settings, the network's start.
 * ----------------------------------------------------------------------- */
#include <3ds.h>
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

extern "C" {

/* ---- time ------------------------------------------------------------ */

u64 gettime(void)
{
    return (u64)((double)svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000.0));
}

/* ---- threads ------------------------------------------------------------- */

struct Lwp {
    Thread   thread;
    void*  (*entry)(void*);
    void*    arg;
    void*    result;
};

static s32 s_mainPrio = -1;

static void lwpEntry(void* p)
{
    Lwp* l = (Lwp*)p;
    l->result = l->entry(l->arg);
}

/* the Wii's priorities (0..127, higher first; the main thread 64) to the
 * 3DS's (0x18..0x3F, lower first; the main thread's, 0x30 usually) */
static s32 ctruPrio(u8 prio)
{
    if (s_mainPrio < 0) svcGetThreadPriority(&s_mainPrio, CUR_THREAD_HANDLE);
    s32 p = s_mainPrio + (64 - (int)prio) / 12;
    if (prio > 64 && p >= s_mainPrio) p = s_mainPrio - 1;
    if (prio < 64 && p <= s_mainPrio) p = s_mainPrio + 1;
    if (p < 0x18) p = 0x18;
    if (p > 0x3F) p = 0x3F;
    return p;
}

s32 LWP_CreateThread(lwp_t* out, void* (*entry)(void*), void* arg, void* stackbase,
                     u32 stack_size, u8 prio)
{
    (void)stackbase;
    Lwp* l = (Lwp*)calloc(1, sizeof(Lwp));
    if (!l) return -1;
    l->entry = entry;
    l->arg = arg;
    u32 stack = stack_size < 64 * 1024 ? 64 * 1024 : stack_size;
    l->thread = threadCreate(lwpEntry, l, stack, ctruPrio(prio), -2, false);
    if (!l->thread) {
        SYS_Report("[Ogc3ds] threadCreate failed (stack %u)\n", (unsigned)stack);
        free(l);
        if (out) *out = LWP_THREAD_NULL;
        return -1;
    }
    if (out) *out = (lwp_t)(uintptr_t)l;
    return 0;
}

s32 LWP_JoinThread(lwp_t t, void** value)
{
    if (t == LWP_THREAD_NULL || t == 0) return -1;
    Lwp* l = (Lwp*)(uintptr_t)t;
    threadJoin(l->thread, U64_MAX);
    threadFree(l->thread);
    if (value) *value = l->result;
    free(l);
    return 0;
}

lwp_t LWP_GetSelf(void)
{
    Thread t = threadGetCurrent();
    return t ? (lwp_t)(uintptr_t)t : 1;
}

void LWP_SetThreadPriority(lwp_t t, u32 prio)
{
    if (t == LWP_THREAD_NULL || t == 0) {
        svcSetThreadPriority(CUR_THREAD_HANDLE, ctruPrio((u8)prio));
        return;
    }
    Lwp* l = (Lwp*)(uintptr_t)t;
    svcSetThreadPriority(threadGetHandle(l->thread), ctruPrio((u8)prio));
}

void LWP_YieldThread(void) { svcSleepThread(0); }
s32 LWP_SuspendThread(lwp_t t) { (void)t; return -1; }
s32 LWP_ResumeThread(lwp_t t) { (void)t; return -1; }

/* ---- mutexes ------------------------------------------------------------- */

s32 LWP_MutexInit(mutex_t* m, bool recursive)
{
    (void)recursive;
    RecursiveLock* l = (RecursiveLock*)malloc(sizeof(RecursiveLock));
    if (!l) { *m = LWP_MUTEX_NULL; return -1; }
    RecursiveLock_Init(l);
    *m = (mutex_t)(uintptr_t)l;
    return 0;
}

s32 LWP_MutexDestroy(mutex_t m)
{
    if (m == LWP_MUTEX_NULL || m == 0) return -1;
    free((RecursiveLock*)(uintptr_t)m);
    return 0;
}

s32 LWP_MutexLock(mutex_t m)
{
    if (m == LWP_MUTEX_NULL || m == 0) return -1;
    RecursiveLock_Lock((RecursiveLock*)(uintptr_t)m);
    return 0;
}

s32 LWP_MutexTryLock(mutex_t m)
{
    if (m == LWP_MUTEX_NULL || m == 0) return -1;
    return RecursiveLock_TryLock((RecursiveLock*)(uintptr_t)m) == 0 ? 0 : 1;
}

s32 LWP_MutexUnlock(mutex_t m)
{
    if (m == LWP_MUTEX_NULL || m == 0) return -1;
    RecursiveLock_Unlock((RecursiveLock*)(uintptr_t)m);
    return 0;
}

/* ---- SYS ---------------------------------------------------------------- */

/* Log.cpp wraps SYS_Report (--wrap): this is the "real" one, the debug
 * output (Citra / Azahar show it; on a console, nothing) */
void SYS_Report(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    svcOutputDebugString(buf, n);
}

volatile bool g_3ds_exitRequested = false;

void SYS_ResetSystem(s32 reset, u32 type, s32 force)
{
    (void)type; (void)force;
    /* the 3DS's HOME menu and power button are the system's: leaving is
     * going back to where WiiFin was started from */
    if (reset == SYS_POWEROFF || reset == SYS_SHUTDOWN) {
        if (reset == SYS_POWEROFF) {
            /* turn the console off (ptm:sysm needs a CIA's rights; else just leave) */
            if (R_SUCCEEDED(ptmSysmInit())) { PTMSYSM_ShutdownAsync(0); ptmSysmExit(); }
        }
    }
    g_3ds_exitRequested = true;
    exit(0);
}

u32 SYS_GetArena1Size(void) { return (u32)osGetMemRegionFree(MEMREGION_APPLICATION); }
u32 SYS_GetArena2Size(void) { return (u32)linearSpaceFree(); }

/* ---- settings ------------------------------------------------------------ */

static bool s_cfgu = false;
static void cfgu() { if (!s_cfgu) s_cfgu = R_SUCCEEDED(cfguInit()); }

s32 CONF_GetLanguage(void)
{
    cfgu();
    u8 lang = CFG_LANGUAGE_EN;
    CFGU_GetSystemLanguage(&lang);
    switch (lang) {
        case CFG_LANGUAGE_JP: return CONF_LANG_JAPANESE;
        case CFG_LANGUAGE_FR: return CONF_LANG_FRENCH;
        case CFG_LANGUAGE_DE: return CONF_LANG_GERMAN;
        case CFG_LANGUAGE_IT: return CONF_LANG_ITALIAN;
        case CFG_LANGUAGE_ES: return CONF_LANG_SPANISH;
        case CFG_LANGUAGE_ZH: return CONF_LANG_SIMP_CHINESE;
        case CFG_LANGUAGE_KO: return CONF_LANG_KOREAN;
        case CFG_LANGUAGE_NL: return CONF_LANG_DUTCH;
        case CFG_LANGUAGE_PT: return CONF_LANG_PORTUGUESE;
        case CFG_LANGUAGE_RU: return CONF_LANG_RUSSIAN;
        case CFG_LANGUAGE_TW: return CONF_LANG_TRAD_CHINESE;
        default:              return CONF_LANG_ENGLISH;
    }
}

s32 CONF_GetRegion(void)
{
    cfgu();
    u8 r = CFG_REGION_EUR;
    CFGU_SecureInfoGetRegion(&r);
    switch (r) {
        case CFG_REGION_JPN: return CONF_REGION_JP;
        case CFG_REGION_USA: return CONF_REGION_US;
        case CFG_REGION_KOR: return CONF_REGION_KR;
        case CFG_REGION_CHN: case CFG_REGION_TWN: return CONF_REGION_CN;
        default: return CONF_REGION_EU;
    }
}

/* one id per console (App.cpp hashes it) */
s32 ES_GetDeviceID(u32* id)
{
    cfgu();
    u64 hash = 0;
    if (R_FAILED(CFGU_GenHashConsoleUnique(0x5749464E, &hash)) || !hash) return -1;
    *id = (u32)(hash ^ (hash >> 32));
    return 0;
}

/* ---- video ---------------------------------------------------------------- */

static GXRModeObj s_mode = { 0, 640, 480, 480, 0, 0, 640, 480 };
GXRModeObj* rmode = &s_mode;

void VIDEO_WaitVSync(void) { gspWaitForVBlank(); }

/* ---- network ---------------------------------------------------------------- */

static u32* s_socBuf = nullptr;
static bool s_socUp = false;
static const u32 SOC_BUFSIZE = 0x100000;

s32 net_init(void)
{
    if (s_socUp) return 0;
    if (!s_socBuf) s_socBuf = (u32*)memalign(0x1000, SOC_BUFSIZE);
    if (!s_socBuf) return -ENOMEM;
    Result r = socInit(s_socBuf, SOC_BUFSIZE);
    if (R_FAILED(r)) { SYS_Report("[Net] socInit failed: %08lx\n", (unsigned long)r); return -EIO; }
    s_socUp = true;
    return 0;
}

void net_deinit(void)
{
    if (!s_socUp) return;
    socExit();
    s_socUp = false;
}

/* The 3DS's Wi-Fi is the system's: the address it has, waiting for the
 * connection for a while (max_retries x 0.5 s) */
s32 if_config(char* ip, char* mask, char* gw, bool dhcp, int retries)
{
    (void)dhcp;
    s32 r = net_init();
    if (r < 0) return r;
    for (int i = 0; i <= retries * 2; ++i) {
        long host = gethostid();
        if (host != 0 && host != INADDR_ANY) {
            struct in_addr a;
            a.s_addr = (in_addr_t)host;
            if (ip) strcpy(ip, inet_ntoa(a));
            struct in_addr ipa, nm, bc;
            if (SOCU_GetIPInfo(&ipa, &nm, &bc) == 0) {
                if (mask) strcpy(mask, inet_ntoa(nm));
                if (gw) strcpy(gw, inet_ntoa(bc));
            } else if (mask) strcpy(mask, "255.255.255.0");
            return 0;
        }
        usleep(500 * 1000);
    }
    if (ip) strcpy(ip, "0.0.0.0");
    return -ENETDOWN;
}

} // extern "C"
