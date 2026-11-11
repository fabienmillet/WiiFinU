/* -----------------------------------------------------------------------
 * PlayerWiiU — what MPlayer does on the Wii (WiiPlayer.h), on the Wii U.
 *
 *   reader thread   HTTP(S) -> a ring buffer (the cache)
 *   player thread   FFmpeg demuxes and decodes from it (on the cores the
 *                   menus leave: the main thread is core 1's); the sound,
 *                   48 kHz, goes to AudioWiiU's stream, the pictures, NV12
 *                   (Y, then U and V side by side), into one of three pairs
 *                   of textures, each shown when the sound reaches its time
 *   main thread     PlaySession / PlayerView as on the Wii; VideoSurface
 *                   draws the newest picture (the GPU makes it RGB)
 *
 * The sound is the clock (AudioWiiU::streamClock).  Without sound, the
 * system clock.
 * ----------------------------------------------------------------------- */
#include "PlayerWiiU.h"
#include "GfxWiiU.h"
#include "AudioWiiU.h"
#include "H264WiiU.h"
#include "../../player/WiiPlayer.h"
#include "../../player/VideoSurface.h"
#include "../../player/stream_wiifin.h"
#include "../../player/vo_wiifin.h"
#include "../../core/Log.h"
#include "../../core/NetConnect.h"
#include "../../ui/Ui.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <condition_variable>
#include <mutex>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string>
#include <deque>
#include <vector>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libavcodec/bsf.h>
}

mbedtls_x509_crt* wiifin_ca_chain();
int wiifin_cert_verify(void*, mbedtls_x509_crt*, int, uint32_t* flags);

/* ---- the Wii's shared state (WiiPlayer.h, stream_wiifin.h) --------------- */

extern "C" {
volatile float g_mplayer_time_pos = 0.0f;
volatile float g_wiifin_audio_pos = 0.0f;   /* the sound heard (the music player's lyrics, its bar) */
volatile float g_mplayer_duration = 0.0f;
volatile int   g_mplayer_paused   = 0;
volatile float g_wiifin_ss_secs = 0.0f;
volatile int   g_wiifin_burned_subs = 0;
volatile float g_wiifin_fps = 0.0f;
float          g_wiifin_test_freeze = 0.0f;
volatile int   g_wiifin_disc_light = 1;
volatile int   g_wiifin_direct = 0;
char           g_wiifin_demuxer[16] = "";
volatile int   g_wiifin_aid = -1;
float          cache_fill_status = 0.0f;
void (*g_stream_opened_cb)(void) = nullptr;
}
volatile int   g_player_stop_reason = PLAYER_STOP_EOF;
volatile float g_wiifin_known_duration = 0.0f;
volatile int   g_wiifin_loading_active = 0;
float          g_wiifin_loading_start_pos = -1.0f;

bool g_wiifin_stream_tls_verify = true;
int  g_wiifin_stream_fail_status = 0;
char g_wiifin_stream_fail_body[160] = "";
volatile unsigned long long g_wiifin_stream_bytes = 0;

extern volatile bool g_app_powerOff;
extern volatile bool g_app_reset;

namespace {

volatile bool s_quit = false;          /* stop everything (request_stop) */

/* ---- HTTP(S) ------------------------------------------------------------- */

const int READ_TIMEOUT_S = 30;

struct Conn {
    s32  sock = -1;
    bool tls  = false;
    bool tlsReady = false;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config       conf;
    mbedtls_ssl_context      ssl;
};

bool waitSock(s32 sock, bool forWrite, int timeoutS)
{
    for (int i = 0; i < timeoutS * 4; ++i) {
        if (s_quit) return false;
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = { 0, 250000 };
        int r = forWrite ? net_select(sock + 1, nullptr, &fds, nullptr, &tv)
                         : net_select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (r > 0) return true;
        if (r < 0) return false;
    }
    return false;
}

int bioSend(void* ctx, const unsigned char* buf, size_t len)
{
    s32 sock = *(s32*)ctx;
    int r = net_write(sock, (void*)buf, (u32)len);
    if (r == -EAGAIN || r == -EWOULDBLOCK) {
        if (!waitSock(sock, true, READ_TIMEOUT_S)) return MBEDTLS_ERR_NET_SEND_FAILED;
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return r < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : r;
}

int bioRecv(void* ctx, unsigned char* buf, size_t len)
{
    s32 sock = *(s32*)ctx;
    if (!waitSock(sock, false, READ_TIMEOUT_S)) return MBEDTLS_ERR_NET_RECV_FAILED;
    int r = net_read(sock, buf, (u32)len);
    if (r == -EAGAIN || r == -EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return r < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : r;
}

void connClose(Conn& c)
{
    if (c.tlsReady) {
        mbedtls_ssl_free(&c.ssl);
        mbedtls_ssl_config_free(&c.conf);
        mbedtls_ctr_drbg_free(&c.drbg);
        mbedtls_entropy_free(&c.entropy);
        c.tlsReady = false;
    }
    if (c.sock >= 0) net_close(c.sock);
    c.sock = -1;
}

bool connOpen(Conn& c, bool tls, const std::string& host, int port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_aton(host.c_str(), &addr.sin_addr) == 0) {
        struct hostent* he = net_gethostbyname(host.c_str());
        if (!he || he->h_length > (int)sizeof(addr.sin_addr)) return false;
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    c.sock = net_socket(AF_INET, SOCK_STREAM, 0);
    if (c.sock < 0) return false;
    static volatile int quitFlag;
    quitFlag = s_quit;
    if (connectWithTimeout(c.sock, &addr, 10, &quitFlag) < 0) { connClose(c); return false; }
    /* a bigger receive window */
    int rcv = 256 * 1024;
    net_setsockopt(c.sock, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    c.tls = tls;
    if (!tls) return true;
    mbedtls_entropy_init(&c.entropy);
    mbedtls_ctr_drbg_init(&c.drbg);
    mbedtls_ssl_config_init(&c.conf);
    mbedtls_ssl_init(&c.ssl);
    c.tlsReady = true;
    const char* pers = "wiifin_stream";
    int ret = mbedtls_ctr_drbg_seed(&c.drbg, mbedtls_entropy_func, &c.entropy,
                                    (const unsigned char*)pers, strlen(pers));
    if (ret == 0)
        ret = mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0) {
        mbedtls_x509_crt* ca = g_wiifin_stream_tls_verify ? wiifin_ca_chain() : nullptr;
        if (ca) {
            mbedtls_ssl_conf_ca_chain(&c.conf, ca, nullptr);
            mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_verify(&c.conf, wiifin_cert_verify, nullptr);
        } else {
            mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
        }
        mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.drbg);
        ret = mbedtls_ssl_setup(&c.ssl, &c.conf);
    }
    if (ret == 0) {
        mbedtls_ssl_set_hostname(&c.ssl, host.c_str());
        mbedtls_ssl_set_bio(&c.ssl, &c.sock, bioSend, bioRecv, nullptr);
        do { ret = mbedtls_ssl_handshake(&c.ssl); }
        while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    if (ret != 0) {
        SYS_Report("[PlayerWiiU] TLS handshake failed -0x%04X\n", (unsigned)-ret);
        connClose(c);
        return false;
    }
    return true;
}

bool connWrite(Conn& c, const char* p, int n)
{
    while (n > 0) {
        int w = c.tls ? mbedtls_ssl_write(&c.ssl, (const unsigned char*)p, (size_t)n)
                      : bioSend(&c.sock, (const unsigned char*)p, (size_t)n);
        if (w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
}

int connRead(Conn& c, char* buf, int n)
{
    for (;;) {
        int r = c.tls ? mbedtls_ssl_read(&c.ssl, (unsigned char*)buf, (size_t)n)
                      : bioRecv(&c.sock, (unsigned char*)buf, (size_t)n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        return r > 0 ? r : 0;
    }
}

enum class Chunk { Size, SizeExt, SizeLF, Data, DataCR, DataLF, Trailer, TrailerLF, Done };

struct Http {
    Conn  conn;
    bool  chunked = false;
    long long remaining = -1;
    bool  done = false;
    char  raw[16384];
    int   rawPos = 0, rawLen = 0;
    Chunk state = Chunk::Size;
    unsigned long chunkLeft = 0;
    bool  sawDigit = false, trailerEmpty = true;
    std::string url;          /* after redirects */
    long long total = -1;     /* a whole file's size (Range works), -1 a live stream */
};

bool parseUrl(const std::string& url, bool& tls, std::string& host, int& port, std::string& path)
{
    size_t p;
    if      (url.compare(0, 8, "https://") == 0) { tls = true;  p = 8; }
    else if (url.compare(0, 7, "http://")  == 0) { tls = false; p = 7; }
    else return false;
    size_t slash = url.find('/', p);
    std::string hostPort = url.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) { host = hostPort.substr(0, colon); port = atoi(hostPort.c_str() + colon + 1); }
    else { host = hostPort; port = tls ? 443 : 80; }
    return !host.empty() && port > 0;
}

std::string headerValue(const std::string& headers, const char* name)
{
    std::string lower = headers;
    for (char& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 32;
    std::string key = std::string("\n") + name + ":";
    size_t p = lower.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    while (p < headers.size() && headers[p] == ' ') ++p;
    size_t e = headers.find("\r\n", p);
    return headers.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

int request(Http* st, const std::string& url, std::string& headers, long long from)
{
    bool tls; std::string host, path; int port;
    if (!parseUrl(url, tls, host, port, path)) return 0;
    Log::addPrivate(host);
    std::string hostHdr = host + (port == (tls ? 443 : 80) ? "" : ":" + std::to_string(port));
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + hostHdr + "\r\n"
                      "User-Agent: WiiFin\r\nAccept: */*\r\n" +
                      (from >= 0 ? "Range: bytes=" + std::to_string(from) + "-\r\n" : std::string()) +
                      "Connection: close\r\n\r\n";
    connClose(st->conn);
    if (!connOpen(st->conn, tls, host, port)) return 0;
    if (!connWrite(st->conn, req.data(), (int)req.size())) return 0;
    st->rawPos = st->rawLen = 0;
    headers.clear();
    for (;;) {
        if (st->rawLen >= (int)sizeof(st->raw)) return 0;
        int n = connRead(st->conn, st->raw + st->rawLen, (int)sizeof(st->raw) - st->rawLen);
        if (n <= 0) return 0;
        st->rawLen += n;
        std::string seen(st->raw, (size_t)st->rawLen);
        size_t end = seen.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        headers = seen.substr(0, end + 2);
        st->rawPos = (int)end + 4;
        break;
    }
    int status = 0;
    if (headers.compare(0, 5, "HTTP/") == 0) {
        size_t sp = headers.find(' ');
        if (sp != std::string::npos) status = atoi(headers.c_str() + sp + 1);
    }
    return status;
}

int decode(Http* st, char* out, int max)
{
    int produced = 0;
    if (!st->chunked) {
        int n = st->rawLen - st->rawPos;
        if (n > max) n = max;
        if (st->remaining >= 0 && n > st->remaining) n = (int)st->remaining;
        memcpy(out, st->raw + st->rawPos, (size_t)n);
        st->rawPos += n;
        if (st->remaining >= 0) { st->remaining -= n; if (st->remaining == 0) st->done = true; }
        return n;
    }
    while (st->rawPos < st->rawLen && produced < max && !st->done) {
        char ch = st->raw[st->rawPos];
        switch (st->state) {
        case Chunk::Size: {
            int d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                  : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
            if (d >= 0) { st->chunkLeft = st->chunkLeft * 16 + d; st->sawDigit = true; }
            else if (ch == '\r') st->state = Chunk::SizeLF;
            else if (ch == ';' || ch == ' ' || ch == '\t') st->state = Chunk::SizeExt;
            ++st->rawPos;
            break;
        }
        case Chunk::SizeExt: if (ch == '\r') st->state = Chunk::SizeLF; ++st->rawPos; break;
        case Chunk::SizeLF:
            ++st->rawPos;
            if (!st->sawDigit) { st->state = Chunk::Size; break; }
            st->sawDigit = false;
            if (st->chunkLeft == 0) { st->state = Chunk::Trailer; st->trailerEmpty = true; }
            else st->state = Chunk::Data;
            break;
        case Chunk::Data: {
            int n = st->rawLen - st->rawPos;
            if ((unsigned long)n > st->chunkLeft) n = (int)st->chunkLeft;
            if (n > max - produced) n = max - produced;
            memcpy(out + produced, st->raw + st->rawPos, (size_t)n);
            produced += n; st->rawPos += n; st->chunkLeft -= (unsigned long)n;
            if (st->chunkLeft == 0) st->state = Chunk::DataCR;
            break;
        }
        case Chunk::DataCR:
            ++st->rawPos;
            st->state = (ch == '\r') ? Chunk::DataLF : Chunk::Size;
            if (ch != '\r' && ch != '\n') --st->rawPos;
            break;
        case Chunk::DataLF: ++st->rawPos; st->state = Chunk::Size; break;
        case Chunk::Trailer:
            ++st->rawPos;
            if (ch == '\r') st->state = Chunk::TrailerLF; else st->trailerEmpty = false;
            break;
        case Chunk::TrailerLF:
            ++st->rawPos;
            if (st->trailerEmpty) { st->state = Chunk::Done; st->done = true; }
            else { st->state = Chunk::Trailer; st->trailerEmpty = true; }
            break;
        case Chunk::Done: st->done = true; break;
        }
    }
    return produced;
}

/* Open url (following redirects) from byte `from` (-1: the start).
 * Returns the HTTP status (2xx: reading can begin), 0 on a network error. */
int httpOpen(Http* st, const std::string& url0, long long from)
{
    std::string url = url0, headers;
    for (int hop = 0; hop < 5; ++hop) {
        st->chunked = false; st->remaining = -1; st->done = false;
        st->state = Chunk::Size; st->chunkLeft = 0; st->sawDigit = false;
        int status = request(st, url, headers, from);
        if (status == 0) return 0;
        if (status >= 300 && status < 400) {
            std::string loc = headerValue(headers, "location");
            connClose(st->conn);
            if (loc.empty()) return status;
            if (loc[0] == '/') {
                size_t p = url.find("://");
                size_t s = url.find('/', p == std::string::npos ? 0 : p + 3);
                loc = url.substr(0, s) + loc;
            }
            url = loc;
            continue;
        }
        st->url = url;
        st->chunked = headerValue(headers, "transfer-encoding").find("chunked") != std::string::npos;
        std::string len = headerValue(headers, "content-length");
        if (!st->chunked && !len.empty()) st->remaining = atoll(len.c_str());
        std::string range = headerValue(headers, "content-range");
        size_t slash = range.find('/');
        if (status == 206 && slash != std::string::npos && range[slash + 1] != '*')
            st->total = atoll(range.c_str() + slash + 1);
        else if (status == 200 && st->remaining >= 0 && headerValue(headers, "accept-ranges") == "bytes")
            st->total = st->remaining;
        if (status >= 400) {
            /* the server's reason, for the error screen */
            char body[160];
            int n = 0;
            while (n < (int)sizeof(body) - 1) {
                if (st->rawPos >= st->rawLen) {
                    int r = connRead(st->conn, st->raw, sizeof(st->raw));
                    if (r <= 0) break;
                    st->rawPos = 0; st->rawLen = r;
                }
                int k = decode(st, body + n, (int)sizeof(body) - 1 - n);
                if (k <= 0) break;
                n += k;
            }
            body[n] = 0;
            g_wiifin_stream_fail_status = status;
            snprintf(g_wiifin_stream_fail_body, sizeof(g_wiifin_stream_fail_body), "%s", body);
            connClose(st->conn);
        }
        return status;
    }
    return 0;
}

/* bytes of the body, 0 at its end or on an error */
int httpRead(Http* st, char* out, int max)
{
    while (!st->done) {
        if (st->rawPos < st->rawLen) {
            int n = decode(st, out, max);
            if (n > 0) return n;
            continue;
        }
        int r = connRead(st->conn, st->raw, sizeof(st->raw));
        if (r <= 0) return 0;
        st->rawPos = 0; st->rawLen = r;
    }
    return 0;
}

/* ---- threads and waiting ----------------------------------------------- */

void sleepMs(int ms) { OSSleepTicks(OSMillisecondsToTicks(ms)); }
void sleepUs(int64_t us) { if (us > 0) OSSleepTicks(OSMicrosecondsToTicks(us)); }
u64 ticksNow() { return (u64)OSGetSystemTime(); }

/* an event: set by one thread, waited for (with a timeout) by another */
struct Event {
    std::mutex m;
    std::condition_variable cv;
    bool set = false;
    void signal() { { std::lock_guard<std::mutex> g(m); set = true; } cv.notify_all(); }
    void wait(int ms)
    {
        std::unique_lock<std::mutex> g(m);
        cv.wait_for(g, std::chrono::milliseconds(ms), [this] { return set; });
        set = false;
    }
};

/* A coreinit thread with a stack of its own (FFmpeg's decoders want a big
 * one), on the given cores */
struct OsThread {
    OSThread thread;
    void*    stack = nullptr;
    void   (*fn)() = nullptr;
};

int osThreadEntry(int, void* argv)
{
    ((OsThread*)argv)->fn();
    return 0;
}

OsThread* startOsThread(void (*fn)(), u32 stackSize, int prio, u32 affinity, const char* name)
{
    OsThread* t = (OsThread*)memalign(16, sizeof(OsThread));
    if (!t) return nullptr;
    memset((void*)t, 0, sizeof(*t));
    t->fn = fn;
    t->stack = memalign(16, stackSize);
    if (!t->stack || !OSCreateThread(&t->thread, osThreadEntry, 0, t, (u8*)t->stack + stackSize,
                                     stackSize, prio, (OSThreadAttributes)affinity)) {
        free(t->stack);
        free(t);
        return nullptr;
    }
    OSSetThreadName(&t->thread, name);
    OSResumeThread(&t->thread);
    return t;
}

void joinOsThread(OsThread*& t)
{
    if (!t) return;
    int r;
    OSJoinThread(&t->thread, &r);
    free(t->stack);
    free(t);
    t = nullptr;
}

/* ---- the cache: a ring buffer between the reader and FFmpeg -------------- */

const size_t RING_SIZE = 24 * 1024 * 1024;
const size_t RING_START = 512 * 1024;     /* playback starts with this much (~1-2 s) */

struct Ring {
    u8*        buf = nullptr;
    volatile unsigned long long rd = 0, wr = 0;   /* bytes read / written ever */
    long long  base = 0;                 /* stream offset of rd == 0 (after a seek) */
    volatile bool eof = false, error = false;
    volatile long long seekTo = -1;      /* asked by FFmpeg: reopen there */
    std::mutex lock;
    Event      changed;
};

Ring  s_ring;
Http* s_http = nullptr;
std::string s_url;
OsThread* s_readerThread = nullptr;

/* (64-bit fields: two accesses on this CPU, read under the lock) */
size_t ringFill() { std::lock_guard<std::mutex> g(s_ring.lock); return (size_t)(s_ring.wr - s_ring.rd); }
long long ringSeekTo() { std::lock_guard<std::mutex> g(s_ring.lock); return s_ring.seekTo; }

/* What PlaySession reads as MPlayer's cache fill (%): its thresholds are
 * for the Wii's 8 MB at 1.5-5 Mb/s (25% about 10 s ahead): the same bytes
 * here.  -1: the whole stream is here. */
const size_t CACHE_REF = 8 * 1024 * 1024;
/* read ahead by the player (its queues): still to play, counted in */
volatile size_t s_heldBytes = 0;
void updateFill()
{
    if (s_ring.eof) { cache_fill_status = -1.0f; return; }
    float f = 100.0f * (ringFill() + s_heldBytes) / CACHE_REF;
    cache_fill_status = f > 100.0f ? 100.0f : f;
}

void readerMain()
{
    Http* st = s_http;
    int status = httpOpen(st, s_url, -1);
    if (status < 200 || status >= 300) {
        SYS_Report("[PlayerWiiU] HTTP %d\n", status);
        s_ring.error = true;
        s_ring.changed.signal();
        return;
    }
    if (g_stream_opened_cb) g_stream_opened_cb();
    static char tmp[65536];
    while (!s_quit) {
        long long seek = ringSeekTo();
        if (seek >= 0) {
            {
                std::lock_guard<std::mutex> g(s_ring.lock);
                s_ring.rd = s_ring.wr = 0;
                s_ring.base = seek;
                s_ring.eof = false;
                s_ring.seekTo = -1;
            }
            status = httpOpen(st, st->url, seek);
            if (status < 200 || status >= 300) { s_ring.error = true; s_ring.changed.signal(); break; }
            s_ring.changed.signal();
        }
        if (s_ring.eof || RING_SIZE - ringFill() < sizeof(tmp)) {
            sleepMs(10);
            continue;
        }
        int n = httpRead(st, tmp, sizeof(tmp));
        if (ringSeekTo() >= 0) continue;      /* a seek came meanwhile: those bytes are not wanted */
        if (n <= 0) {
            /* a whole file cut short: on again from where it stopped */
            if (!st->done && st->total > 0 && !s_quit) {
                long long at;
                { std::lock_guard<std::mutex> g(s_ring.lock); at = s_ring.base + (long long)s_ring.wr; }
                SYS_Report("[PlayerWiiU] connection lost at %lld, resuming\n", at);
                if (httpOpen(st, st->url, at) / 100 == 2) continue;
            }
            s_ring.eof = true;
            updateFill();
            s_ring.changed.signal();
            continue;
        }
        size_t w;
        { std::lock_guard<std::mutex> g(s_ring.lock); w = (size_t)(s_ring.wr % RING_SIZE); }
        size_t first = RING_SIZE - w < (size_t)n ? RING_SIZE - w : (size_t)n;
        memcpy(s_ring.buf + w, tmp, first);
        if ((size_t)n > first) memcpy(s_ring.buf, tmp + first, n - first);
        { std::lock_guard<std::mutex> g(s_ring.lock); s_ring.wr += n; }
        g_wiifin_stream_bytes += n;
        updateFill();
        s_ring.changed.signal();
    }
    connClose(st->conn);
}

int avRead(void*, uint8_t* buf, int size)
{
    for (;;) {
        if (s_quit) return AVERROR_EXIT;
        size_t have = ringFill();
        if (have > 0 && ringSeekTo() < 0) {
            size_t n;
            {
                std::lock_guard<std::mutex> g(s_ring.lock);
                have = (size_t)(s_ring.wr - s_ring.rd);
                n = have < (size_t)size ? have : (size_t)size;
                size_t r = (size_t)(s_ring.rd % RING_SIZE);
                size_t first = RING_SIZE - r < n ? RING_SIZE - r : n;
                memcpy(buf, s_ring.buf + r, first);
                if (n > first) memcpy(buf + first, s_ring.buf, n - first);
                s_ring.rd += n;
            }
            updateFill();
            if (n > 0) return (int)n;
        }
        if (s_ring.error) return AVERROR(EIO);
        if (s_ring.eof && ringFill() == 0) return AVERROR_EOF;
        s_ring.changed.wait(100);
    }
}

int64_t avSeek(void*, int64_t off, int whence)
{
    if (whence & AVSEEK_SIZE) return s_http && s_http->total > 0 ? s_http->total : -1;
    whence &= ~AVSEEK_FORCE;   /* FFmpeg may add it: the same seek */
    if (!s_http || s_http->total <= 0) return -1;
    {
        std::lock_guard<std::mutex> g(s_ring.lock);
        const long long pos = s_ring.base + (long long)s_ring.rd;
        const long long target = whence == SEEK_SET ? off : whence == SEEK_CUR ? pos + off : s_http->total + off;
        /* within what is buffered: no new request */
        if (target >= pos && target <= s_ring.base + (long long)s_ring.wr) {
            s_ring.rd += (unsigned long long)(target - pos);
            return target;
        }
        s_ring.seekTo = target;
        off = target;
    }
    const long long target = off;
    while (ringSeekTo() >= 0 && !s_quit && !s_ring.error) s_ring.changed.wait(50);
    return s_ring.error ? -1 : target;
}

/* ---- sound (AudioWiiU's stream) ------------------------------------------ */

/* pause / resume asked while the sound waits for room (the player's) */
void (*s_pauseHook)() = nullptr;
bool         s_audioOn = false;
volatile int s_volume = 70;
volatile int s_muted = 0;
const double AUDIO_AHEAD = 0.75;       /* seconds of sound queued */

void applyVolume() { AudioWiiU::streamVolume(s_muted ? 0.0f : s_volume / 100.0f); }

bool audioStart()
{
    if (!AudioWiiU::init()) return false;
    AudioWiiU::streamStart(AUDIO_AHEAD);
    applyVolume();
    s_audioOn = true;
    return true;
}

void audioStop()
{
    if (!s_audioOn) return;
    AudioWiiU::streamStop();
    s_audioOn = false;
}

double audioClock() { return s_audioOn ? AudioWiiU::streamClock() : -1; }
int    audioQueuedFrames() { return s_audioOn ? AudioWiiU::streamQueued() : 0; }
bool   audioRoom() { return s_audioOn && audioQueuedFrames() < (int)(AUDIO_AHEAD * AudioWiiU::RATE) - 2048; }

volatile bool s_audioStop = false;
int s_h264Threads = 3;
/* the console's H.264 decoder: refused once, not tried again (FFmpeg then) */
bool s_hwBroken = false;
bool audioPush(const s16* pcm, int frames, double pts)
{
    while (!s_quit) {
        if (s_pauseHook) s_pauseHook();
        if (audioQueuedFrames() + frames <= (int)(AUDIO_AHEAD * AudioWiiU::RATE)) break;
        sleepMs(4);
    }
    if (s_quit) return false;
    s_audioStop = false;
    return AudioWiiU::streamPush(pcm, frames, pts, &s_audioStop);
}

void audioFlushQueue() { if (s_audioOn) AudioWiiU::streamFlush(); }

/* ---- pictures -------------------------------------------------------------- */

const int NTEX = 4;
TexWiiU*     s_texY[NTEX] = {};
TexWiiU*     s_texUV[NTEX] = {};
int          s_texW = 0, s_texH = 0;
float        s_aspect = 16.0f / 9.0f;   /* the picture's display aspect */
bool         s_bt709 = false;
volatile int s_front = -1;              /* the newest picture (the player's own textures) */
unsigned     s_drawnAt[NTEX] = {};      /* the frame each was last drawn in (+1) */
TexWiiU*     s_hwY = nullptr;           /* or the console decoder's frame shown */
TexWiiU*     s_hwUV = nullptr;
std::mutex   s_texLock;
volatile bool s_videoActive = false;

void videoFree()
{
    s_videoActive = false;
    std::lock_guard<std::mutex> g(s_texLock);
    for (int i = 0; i < NTEX; ++i) {
        GfxWiiU::destroy(s_texY[i]); s_texY[i] = nullptr;
        GfxWiiU::destroy(s_texUV[i]); s_texUV[i] = nullptr;
    }
    s_front = -1;
    for (unsigned& d : s_drawnAt) d = 0;
    s_hwY = s_hwUV = nullptr;
}

bool videoAlloc(int w, int h)
{
    videoFree();
    for (int i = 0; i < NTEX; ++i) {
        s_texY[i] = GfxWiiU::make(GfxWiiU::Fmt::R8, w, h, nullptr, 0);
        s_texUV[i] = GfxWiiU::make(GfxWiiU::Fmt::RG8, (w + 1) / 2, (h + 1) / 2, nullptr, 0);
        if (!s_texY[i] || !s_texUV[i]) { videoFree(); return false; }
    }
    s_texW = w; s_texH = h;
    return true;
}

/* the picture into a pair the screen isn't using, then shown */
unsigned s_shown = 0;   /* pictures shown (wiifin_video_frames) */
unsigned s_skipped = 0; /* decoded too late, not shown */
/* CPU time decoding, for the log: how much of the CPU playback takes */
u64 s_vTicks = 0, s_aTicks = 0;

/* Y as it is; U and V (4:2:0, planar or NV12) interleaved */
void toNV12(const AVFrame* f, TexWiiU* ty, TexWiiU* tuv)
{
    const int w = s_texW, h = s_texH, cw = (w + 1) / 2, ch = (h + 1) / 2;
    u8* y = (u8*)GfxWiiU::pixels(ty);
    const int yp = GfxWiiU::pitch(ty);
    for (int r = 0; r < h; ++r) memcpy(y + (size_t)r * yp, f->data[0] + (size_t)r * f->linesize[0], w);
    u8* uv = (u8*)GfxWiiU::pixels(tuv);
    const int uvp = GfxWiiU::pitch(tuv) * 2;
    if (f->format == AV_PIX_FMT_NV12) {
        for (int r = 0; r < ch; ++r) memcpy(uv + (size_t)r * uvp, f->data[1] + (size_t)r * f->linesize[1], cw * 2);
    } else {
        for (int r = 0; r < ch; ++r) {
            const u8* u = f->data[1] + (size_t)r * f->linesize[1];
            const u8* v = f->data[2] + (size_t)r * f->linesize[2];
            u8* o = uv + (size_t)r * uvp;
            for (int x = 0; x < cw; ++x) { o[x * 2] = u[x]; o[x * 2 + 1] = v[x]; }
        }
    }
    GfxWiiU::flush(ty);
    GfxWiiU::flush(tuv);
}

/* a frame of the console's decoder: shown as it is */
void videoShowHw(TexWiiU* y, TexWiiU* uv)
{
    ++s_shown;
    /* written by the decoder: the GPU's cache forgets what it held */
    GfxWiiU::touched(y);
    GfxWiiU::touched(uv);
    H264WiiU::inUse(y, nullptr);
    std::lock_guard<std::mutex> g(s_texLock);
    s_hwY = y;
    s_hwUV = uv;
    s_front = -1;
}

void videoShow(const AVFrame* f)
{
    ++s_shown;
    int target = 0;
    {
        /* not the newest, nor one drawn in the frame being recorded (the
         * GPU draws it at its end) */
        std::lock_guard<std::mutex> g(s_texLock);
        const unsigned now = GfxWiiU::frames() + 1;
        while (target < NTEX && (target == s_front || s_drawnAt[target] >= now)) ++target;
    }
    if (target >= NTEX || !s_texY[target]) { ++s_skipped; return; }
    toNV12(f, s_texY[target], s_texUV[target]);
    std::lock_guard<std::mutex> g(s_texLock);
    s_front = target;
    s_hwY = s_hwUV = nullptr;
}

/* ---- the player thread ----------------------------------------------------- */

enum class Mode { Video, Audio };

Mode         s_mode = Mode::Video;
OsThread*    s_playThread = nullptr;
volatile int s_running = 0;
volatile float s_seekBy = 0.0f;
volatile bool  s_seekAsked = false;
volatile bool  s_pauseToggle = false;
const float TS_START = 1.4f;
float s_streamStart = -1.0f;
bool  s_ssMissed = false;

void logUrl(const char* tag, const std::string& url)
{
    std::string safe(url);
    size_t kp = safe.find("ApiKey=");
    if (kp != std::string::npos) {
        size_t ve = safe.find('&', kp + 7);
        safe.replace(kp + 7, (ve == std::string::npos ? safe.size() : ve) - kp - 7, "***");
    }
    SYS_Report("[PlayerWiiU] %s: %s\n", tag, safe.c_str());
}

u64 s_pausedSince = 0, s_sysStart = 0;
double s_sysPts0 = 0;

volatile bool s_pauseForce = false;   /* the HOME menu: paused, whatever was asked */
void handlePause()
{
    if (s_pauseForce) {
        s_pauseForce = false;
        s_pauseToggle = g_mplayer_paused == 0;   /* a pause, not a toggle */
    }
    if (!s_pauseToggle) return;
    s_pauseToggle = false;
    g_mplayer_paused = !g_mplayer_paused;
    if (s_audioOn) AudioWiiU::streamPause(g_mplayer_paused);
    if (g_mplayer_paused) s_pausedSince = gettime();
    else if (s_pausedSince) { if (s_sysStart) s_sysStart += gettime() - s_pausedSince; s_pausedSince = 0; }
    SYS_Report("[PlayerWiiU] %s at %.1f s\n", g_mplayer_paused ? "paused" : "resumed", (double)g_mplayer_time_pos);
}

/* FFmpeg's own messages (warnings and errors; more while a stream opens) */
void avLog(void* avcl, int level, const char* fmt, va_list vl)
{
    if (level > av_log_get_level()) return;
    char line[256];
    int prefix = 1;
    av_log_format_line(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    SYS_Report("[FFmpeg] %s", line);
}

void playMain()
{
    static bool logSet = false;
    if (!logSet) { av_log_set_callback(avLog); logSet = true; }
    av_log_set_level(AV_LOG_VERBOSE);
    s_pauseHook = handlePause;
    s_ring.buf = (u8*)malloc(RING_SIZE);
    s_http = new Http();
    AVFormatContext* fmt = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext *vdec = nullptr, *adec = nullptr;
    bool hw = false;                 /* the console's H.264 decoder */
    AVBSFContext* bsf = nullptr;     /* AVCC to Annex B, for it */
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    s16* pcm = nullptr;
    int pcmCap = 0;
    int vIdx = -1, aIdx = -1;
    bool started = false, failed = true;
    u64& sysStart = s_sysStart;
    double& sysPts0 = s_sysPts0;
    sysStart = 0; s_pausedSince = 0;

    if (!s_ring.buf || !s_http || !pkt || !frame) goto out;
    s_ring.rd = s_ring.wr = 0; s_ring.base = 0;
    s_ring.eof = s_ring.error = false; s_ring.seekTo = -1;
    /* the reader above the decoder: the cache is never starved */
    s_readerThread = startOsThread(readerMain, 128 * 1024, 16, OS_THREAD_ATTRIB_AFFINITY_ANY, "WiiFin reader");
    if (!s_readerThread) goto out;
    /* some seconds in the cache before playing (MPlayer's -cache-min 8) */
    while (!s_quit && !s_ring.error && !s_ring.eof && ringFill() < RING_START) s_ring.changed.wait(100);
    if (s_quit || s_ring.error) goto out;

    {
        const int IOBUF = 64 * 1024;
        u8* iobuf = (u8*)av_malloc(IOBUF);
        io = avio_alloc_context(iobuf, IOBUF, 0, nullptr, avRead, nullptr, avSeek);
        if (!io) goto out;
        io->seekable = s_http->total > 0 ? AVIO_SEEKABLE_NORMAL : 0;
        fmt = avformat_alloc_context();
        fmt->pb = io;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        AVDictionary* opts = nullptr;
        const AVInputFormat* ifmt = nullptr;
        if (s_mode == Mode::Video && !g_wiifin_direct) {
            ifmt = av_find_input_format("mpegts");
            const bool big = g_wiifin_burned_subs || g_wiifin_ss_secs > 0.0f;
            av_dict_set(&opts, "probesize", big ? "1048576" : "131072", 0);
            av_dict_set(&opts, "analyzeduration", big ? "10000000" : "1500000", 0);
        } else if (g_wiifin_direct && strncmp(g_wiifin_demuxer, "lavf:", 5) == 0) {
            ifmt = av_find_input_format(g_wiifin_demuxer + 5);
        }
        if (avformat_open_input(&fmt, nullptr, ifmt, &opts) < 0) {
            av_dict_free(&opts);
            SYS_Report("[PlayerWiiU] cannot open the stream\n");
            fmt = nullptr;
            goto out;
        }
        av_dict_free(&opts);
        {
            /* its decoders tried on one thread (the stream's own setting comes after) */
            std::vector<AVDictionary*> so(fmt->nb_streams, nullptr);
            for (auto& d : so) av_dict_set(&d, "threads", "1", 0);
            if (avformat_find_stream_info(fmt, so.empty() ? nullptr : so.data()) < 0)
                SYS_Report("[PlayerWiiU] no stream info\n");
            for (auto& d : so) av_dict_free(&d);
        }
    }
    if (s_mode == Mode::Video) vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    aIdx = -1;
    if (g_wiifin_direct && g_wiifin_aid >= 0 && s_mode == Mode::Video) {
        /* a file's chosen track: its rank among the audio ones */
        int n = 0;
        for (unsigned i = 0; i < fmt->nb_streams && aIdx < 0; ++i)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && n++ == g_wiifin_aid) aIdx = (int)i;
    }
    if (aIdx < 0)
        aIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vIdx >= 0 ? vIdx : -1, nullptr, 0);
    if (vIdx < 0 && aIdx < 0) { SYS_Report("[PlayerWiiU] no audio nor video\n"); goto out; }
    if (fmt->duration > 0) g_mplayer_duration = (float)(fmt->duration / (double)AV_TIME_BASE);

    if (vIdx >= 0 && fmt->streams[vIdx]->codecpar->codec_id == AV_CODEC_ID_H264 && !s_hwBroken &&
        !H264WiiU::emulated()) {
        /* H.264: the console's decoder (HD without the CPU); a stream in
         * MKV or MP4 (AVCC) goes to it as Annex B */
        AVStream* st = fmt->streams[vIdx];
        const AVCodecParameters* cp = st->codecpar;
        const int prof = (cp->profile & ~AV_PROFILE_H264_CONSTRAINED) == 66 ? 66 : cp->profile;
        const bool fmtOk = (cp->format == AV_PIX_FMT_YUV420P || cp->format == AV_PIX_FMT_YUVJ420P || cp->format < 0) &&
                           (cp->level <= 42 || cp->level == AV_LEVEL_UNKNOWN);
        hw = fmtOk && H264WiiU::open(cp->width, cp->height, prof);
        if (hw && cp->extradata_size > 0 && cp->extradata[0] == 1) {
            const AVBitStreamFilter* f = av_bsf_get_by_name("h264_mp4toannexb");
            if (!f || av_bsf_alloc(f, &bsf) < 0 || avcodec_parameters_copy(bsf->par_in, cp) < 0) av_bsf_free(&bsf);
            else {
                bsf->time_base_in = st->time_base;
                if (av_bsf_init(bsf) < 0) av_bsf_free(&bsf);
            }
            if (!bsf) { SYS_Report("[PlayerWiiU] no Annex B filter\n"); H264WiiU::close(); hw = false; }
        }
        if (hw) {
            const int w = cp->width, h = cp->height;
            AVRational sar = cp->sample_aspect_ratio;
            s_aspect = h > 0 ? (float)w / h * (sar.num > 0 && sar.den > 0 ? (float)sar.num / sar.den : 1.0f) : 16.0f / 9.0f;
            s_bt709 = cp->color_space == AVCOL_SPC_BT709 || (cp->color_space == AVCOL_SPC_UNSPECIFIED && h > 576);
            SYS_Report("[PlayerWiiU] video h264 %dx%d (profile %d, level %d) aspect %.2f %s: the console's decoder%s\n",
                       w, h, cp->profile, cp->level, (double)s_aspect, s_bt709 ? "BT.709" : "BT.601",
                       bsf ? ", Annex B" : "");
            s_texW = w; s_texH = h;
        } else {
            SYS_Report("[PlayerWiiU] H.264 profile %d %s: decoded by FFmpeg\n", cp->profile,
                       av_get_pix_fmt_name((AVPixelFormat)cp->format) ? av_get_pix_fmt_name((AVPixelFormat)cp->format) : "?");
        }
    }
    if (vIdx >= 0 && !hw) {
        AVStream* st = fmt->streams[vIdx];
        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        vdec = c ? avcodec_alloc_context3(c) : nullptr;
        if (vdec && avcodec_parameters_to_context(vdec, st->codecpar) >= 0) {
            /* the three cores for H.264 (frames in parallel); MPEG-4 and
             * MPEG-2 are light enough for one */
            const bool heavy = c->id == AV_CODEC_ID_H264;
            vdec->thread_count = heavy ? s_h264Threads : 1;
            vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            vdec->flags2 |= AV_CODEC_FLAG2_FAST;
            if (heavy && st->codecpar->width * st->codecpar->height > 854 * 480) vdec->skip_loop_filter = AVDISCARD_NONREF;
            if (avcodec_open2(vdec, c, nullptr) < 0) avcodec_free_context(&vdec);
        } else if (vdec) avcodec_free_context(&vdec);
        if (!vdec) { SYS_Report("[PlayerWiiU] no decoder for video codec %d\n", (int)st->codecpar->codec_id); vIdx = -1; }
        else {
            int w = st->codecpar->width, h = st->codecpar->height;
            AVRational sar = st->codecpar->sample_aspect_ratio;
            s_aspect = h > 0 ? (float)w / h * (sar.num > 0 && sar.den > 0 ? (float)sar.num / sar.den : 1.0f) : 16.0f / 9.0f;
            s_bt709 = st->codecpar->color_space == AVCOL_SPC_BT709 ||
                      (st->codecpar->color_space == AVCOL_SPC_UNSPECIFIED && h > 576);
            SYS_Report("[PlayerWiiU] video %s %dx%d aspect %.2f %s, %d thread(s)\n", c->name, w, h, (double)s_aspect,
                       s_bt709 ? "BT.709" : "BT.601", vdec->thread_count);
            if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || !videoAlloc(w, h)) {
                SYS_Report("[PlayerWiiU] picture %dx%d not possible\n", w, h);
                avcodec_free_context(&vdec);
                vIdx = -1;
            }
        }
    }
    if (aIdx >= 0) {
        AVStream* st = fmt->streams[aIdx];
        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        adec = c ? avcodec_alloc_context3(c) : nullptr;
        if (adec && avcodec_parameters_to_context(adec, st->codecpar) >= 0 && avcodec_open2(adec, c, nullptr) >= 0) {
            const int rate = adec->sample_rate > 0 ? adec->sample_rate : 44100;
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, AudioWiiU::RATE, &adec->ch_layout,
                                    adec->sample_fmt, rate, 0, nullptr) < 0 || swr_init(swr) < 0 ||
                !audioStart()) {
                SYS_Report("[PlayerWiiU] no sound output\n");
                swr_free(&swr);
                avcodec_free_context(&adec);
                aIdx = -1;
            } else {
                SYS_Report("[PlayerWiiU] audio %s %d Hz %d ch\n", c->name, rate, adec->ch_layout.nb_channels);
            }
        } else {
            if (adec) avcodec_free_context(&adec);
            aIdx = -1;
        }
    }
    if (vIdx < 0 && aIdx < 0) goto out;
    av_log_set_level(AV_LOG_WARNING);
    s_videoActive = vIdx >= 0;
    failed = false;
    /* a file played as it is, resumed: to its position first (a transcode
     * starts there itself; MPlayer had -ss on the Wii) */
    if (g_wiifin_direct && g_wiifin_ss_secs > 0.5f && s_mode == Mode::Video) {
        const float ss = g_wiifin_ss_secs;
        if (av_seek_frame(fmt, -1, (int64_t)(ss * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) >= 0) {
            if (vdec) avcodec_flush_buffers(vdec);
            if (hw) { H264WiiU::reset(); if (bsf) av_bsf_flush(bsf); }
            if (adec) avcodec_flush_buffers(adec);
            SYS_Report("[PlayerWiiU] started at %.1f s\n", (double)ss);
        } else {
            SYS_Report("[PlayerWiiU] cannot start at %.1f s: from the start\n", (double)ss);
        }
    }

    {
        /* Packets are read ahead of the pictures: the sound is decoded as
         * soon as it comes (most of a second of it queued), the video's
         * packets wait in a queue, decoded one picture at a time, each
         * shown when the sound gets to it.  While a picture waits, more is
         * read. */
        std::deque<AVPacket*> vq;
        const size_t VQ_MAX = 400;
        bool eof = false, havePic = false;
        double picPts = 0;
        AVFrame* pic = av_frame_alloc();
        const AVRational vtb = vIdx >= 0 ? fmt->streams[vIdx]->time_base : AVRational{ 1, 1 };
        const AVRational atb = aIdx >= 0 ? fmt->streams[aIdx]->time_base : AVRational{ 1, 1 };
        size_t vqBytes = 0;
        const int audioBps = adec && adec->bit_rate > 0 ? (int)(adec->bit_rate / 8) : 16000;
        auto held = [&]() {
            size_t a = s_audioOn ? (size_t)(audioQueuedFrames() / (double)AudioWiiU::RATE * audioBps) : 0;
            s_heldBytes = vqBytes + a;
            updateFill();
        };
        auto clearQueue = [&]() { for (AVPacket* q : vq) av_packet_free(&q); vq.clear(); vqBytes = 0; havePic = false; };
        TexWiiU *hwY = nullptr, *hwUV = nullptr;
        bool hwDrained = false;
        int hwFails = 0, hwFed = 0;
        AVPacket* annexb = av_packet_alloc();
        /* an access unit to the console's decoder (through the filter) */
        auto hwFeed = [&](AVPacket* q) -> bool {
            const double pts = q->pts != AV_NOPTS_VALUE ? q->pts * av_q2d(vtb)
                             : q->dts != AV_NOPTS_VALUE ? q->dts * av_q2d(vtb) : 0;
            if (!bsf) return H264WiiU::decode(q->data, q->size, pts);
            bool ok = true;
            if (av_bsf_send_packet(bsf, q) < 0) return false;
            while (av_bsf_receive_packet(bsf, annexb) == 0) {
                ok = H264WiiU::decode(annexb->data, annexb->size, pts) && ok;
                av_packet_unref(annexb);
            }
            return ok;
        };

        while (!s_quit) {
            if (g_app_powerOff || g_app_reset) { g_player_stop_reason = PLAYER_STOP_EOF; break; }
            handlePause();
            if (g_mplayer_paused) { sleepMs(20); continue; }
            /* a seek (a whole file) */
            if (s_seekAsked) {
                s_seekAsked = false;
                double target = g_mplayer_time_pos + s_seekBy;
                if (target < 0) target = 0;
                if (av_seek_frame(fmt, -1, (int64_t)(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) >= 0) {
                    if (vdec) avcodec_flush_buffers(vdec);
                    if (hw) { H264WiiU::reset(); if (bsf) av_bsf_flush(bsf); hwDrained = false; }
                    if (adec) avcodec_flush_buffers(adec);
                    clearQueue();
                    audioFlushQueue();
                    eof = false;
                    started = false;
                    sysStart = 0;
                    g_wiifin_loading_active = 1;
                }
            }

            /* read: until the sound is well ahead, or enough pictures wait */
            int reads = 0;
            while (!eof && !s_quit && reads < 8 && (aIdx < 0 || audioRoom()) && vq.size() < VQ_MAX) {
                int r = av_read_frame(fmt, pkt);
                if (r < 0) {
                    if (r == AVERROR(EAGAIN)) break;
                    if (r != AVERROR_EOF) {
                        char e[64];
                        av_strerror(r, e, sizeof(e));
                        SYS_Report("[PlayerWiiU] read: %s\n", e);
                    }
                    if (r != AVERROR_EOF && !s_ring.eof) g_player_stop_reason = PLAYER_STOP_ERROR;
                    eof = true;
                    break;
                }
                ++reads;
                if (pkt->stream_index == aIdx && adec) {
                    const u64 t0 = ticksNow();
                    if (avcodec_send_packet(adec, pkt) >= 0) {
                        while (avcodec_receive_frame(adec, frame) >= 0) {
                            double pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                       ? frame->best_effort_timestamp * av_q2d(atb) : 0;
                            int out = swr_get_out_samples(swr, frame->nb_samples);
                            if (out > pcmCap) { free(pcm); pcmCap = out + 256; pcm = (s16*)malloc(pcmCap * 4); }
                            uint8_t* dst = (uint8_t*)pcm;
                            int got = pcm ? swr_convert(swr, &dst, pcmCap, (const uint8_t**)frame->extended_data,
                                                        frame->nb_samples) : 0;
                            if (got > 0) audioPush(pcm, got, pts);
                            if (!started && vIdx < 0) {
                                started = true;
                                g_wiifin_loading_start_pos = (float)pts;
                                g_wiifin_loading_active = 0;
                            }
                        }
                    }
                    s_aTicks += ticksNow() - t0;
                } else if (pkt->stream_index == vIdx && (vdec || hw)) {
                    AVPacket* q = av_packet_clone(pkt);
                    if (q) { vq.push_back(q); vqBytes += q->size; }
                }
                av_packet_unref(pkt);
            }
            held();

            if (vIdx < 0) {
                double t = audioClock();
                if (t >= 0) g_mplayer_time_pos = g_wiifin_audio_pos = (float)t;
                if (eof) {
                    if (audioQueuedFrames() == 0) break;
                    sleepMs(20);
                } else if (!audioRoom()) sleepMs(10);
                continue;
            }

            /* the next picture */
            const u64 vt0 = ticksNow();
            while (hw && !havePic) {
                double t;
                if (H264WiiU::next(hwY, hwUV, t)) {
                    havePic = true;
                    picPts = t;
                    H264WiiU::inUse(s_hwY, hwY);
                    break;
                }
                if (vq.empty()) {
                    if (eof && !hwDrained) { H264WiiU::drain(); hwDrained = true; continue; }
                    break;
                }
                AVPacket* q = vq.front();
                vq.pop_front();
                vqBytes -= q->size;
                const bool ok = hwFeed(q);
                av_packet_free(&q);
                ++hwFed;
                hwFails = ok ? 0 : hwFails + 1;
                if ((hwFails > 8 || hwFed > 120) && H264WiiU::decoded() == 0) {
                    /* the console's decoder refuses this stream: a file goes on
                     * converted by the server (H.264 it can decode); one of the
                     * server's own: FFmpeg's (MPEG-4) from now on */
                    SYS_Report("[PlayerWiiU] the console's decoder gave no picture (%d refused)%s\n", hwFails,
                               g_wiifin_direct ? "" : ": MPEG-4 from now on");
                    if (!g_wiifin_direct) s_hwBroken = true;
                    g_player_stop_reason = PLAYER_STOP_ERROR;
                    s_quit = true;
                    break;
                }
            }
            while (!hw && !havePic && !vq.empty()) {
                AVPacket* q = vq.front();
                vq.pop_front();
                vqBytes -= q->size;
                int sr = avcodec_send_packet(vdec, q);
                av_packet_free(&q);
                if (sr < 0 && sr != AVERROR(EAGAIN)) continue;
                if (avcodec_receive_frame(vdec, pic) >= 0) {
                    havePic = true;
                    picPts = pic->best_effort_timestamp != AV_NOPTS_VALUE ? pic->best_effort_timestamp * av_q2d(vtb) : 0;
                }
            }
            if (!hw && !havePic && eof && vq.empty()) {
                /* the decoder's last pictures (frame threads hold some) */
                avcodec_send_packet(vdec, nullptr);
                if (avcodec_receive_frame(vdec, pic) >= 0) {
                    havePic = true;
                    picPts = pic->best_effort_timestamp != AV_NOPTS_VALUE ? pic->best_effort_timestamp * av_q2d(vtb) : 0;
                }
            }
            s_vTicks += ticksNow() - vt0;
            if (!havePic) {
                if (eof) {
                    /* the end: what sound is queued, heard */
                    if (s_audioOn && audioQueuedFrames() > 0) {
                        double t = audioClock();
                        if (t >= 0) g_mplayer_time_pos = g_wiifin_audio_pos = (float)t;
                        sleepMs(20);
                        continue;
                    }
                    break;
                }
                if (reads == 0) sleepMs(5);   /* waiting for data */
                continue;
            }
            /* its time: the sound's clock, else the system's from the
             * last time heard */
            double now = audioClock();
            if (now >= 0) { sysStart = gettime(); sysPts0 = now; }
            else {
                if (!sysStart) { sysStart = gettime(); sysPts0 = picPts; }
                now = sysPts0 + (gettime() - sysStart) / 1e6;
            }
            const double ahead = picPts - now;
            if (ahead > 0.008 && started) {
                /* early: read meanwhile, or sleep a little */
                if (reads == 0) sleepUs((int64_t)((ahead > 0.01 ? 0.01 : ahead) * 1e6));
                continue;
            }
            havePic = false;
            if (ahead < -0.15 && started) {
                ++s_skipped;
                continue;     /* behind: not shown */
            }
            if (hw) videoShowHw(hwY, hwUV);
            else {
                if (pic->width != s_texW || pic->height != s_texH) {
                    if (!videoAlloc(pic->width, pic->height)) continue;
                    s_videoActive = true;
                }
                videoShow(pic);
            }
            g_mplayer_time_pos = (float)picPts;
            if (!started) {
                started = true;
                g_wiifin_loading_start_pos = (float)picPts;
                g_wiifin_loading_active = 0;
                SYS_Report("[PlayerWiiU] first picture at %.2f s\n", picPts);
            }
            {
                static u64 lastLog = 0;
                const u64 t = gettime();
                if (t - lastLog > 5000000ULL) {
                    static u64 lastTick = 0;
                    const u64 tk = ticksNow();
                    const double span = lastTick ? (double)(tk - lastTick) : 0;
                    SYS_Report("[PlayerWiiU] at %.1f s, audio %.2f s (%d frames queued), cache %.0f KB, "
                               "%u pictures, %u late, %u waiting, CPU video %.0f%% audio %.0f%%\n",
                               picPts, audioClock(), audioQueuedFrames(),
                               ringFill() / 1024.0, s_shown, s_skipped, (unsigned)vq.size(),
                               span > 0 ? 100.0 * s_vTicks / span : 0.0, span > 0 ? 100.0 * s_aTicks / span : 0.0);
                    lastLog = t;
                    lastTick = tk;
                    s_vTicks = s_aTicks = 0;
                }
            }
        }
        clearQueue();
        s_heldBytes = 0;
        av_frame_free(&pic);
        av_packet_free(&annexb);
    }

out:
    if (failed && g_player_stop_reason == PLAYER_STOP_EOF) g_player_stop_reason = PLAYER_STOP_ERROR;
    s_quit = true;
    s_audioStop = true;
    s_ring.changed.signal();
    joinOsThread(s_readerThread);
    audioStop();
    if (fmt) avformat_close_input(&fmt);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    if (vdec) avcodec_free_context(&vdec);
    if (adec) avcodec_free_context(&adec);
    swr_free(&swr);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    free(pcm);
    videoFree();
    av_bsf_free(&bsf);
    if (hw) {
        SYS_Report("[PlayerWiiU] the console's decoder: %u pictures\n", H264WiiU::decoded());
        H264WiiU::close();
    }
    if (s_http) { connClose(s_http->conn); delete s_http; s_http = nullptr; }
    free(s_ring.buf);
    s_ring.buf = nullptr;
    /* nothing played: an error, the caller asks the server again */
    if (g_player_stop_reason == PLAYER_STOP_EOF && g_mplayer_time_pos < 2.0f && g_mplayer_duration == 0.0f &&
        s_mode == Mode::Video)
        g_player_stop_reason = PLAYER_STOP_ERROR;
    SYS_Report("[PlayerWiiU] stopped: reason %d at %.1f s\n", (int)g_player_stop_reason, (double)g_mplayer_time_pos);
    s_running = 0;
}

void resetState()
{
    s_quit = false;
    s_audioStop = false;
    g_player_stop_reason = PLAYER_STOP_EOF;
    g_mplayer_time_pos = 0.0f;
    g_wiifin_audio_pos = 0.0f;
    g_mplayer_duration = 0.0f;
    g_mplayer_paused = 0;
    s_streamStart = -1.0f;
    s_ssMissed = false;
    s_seekAsked = false;
    s_pauseToggle = false;
    g_wiifin_stream_fail_status = 0;
    g_wiifin_stream_fail_body[0] = 0;
    cache_fill_status = 0.0f;
}

bool startThread(const char* url, Mode mode)
{
    if (s_playThread) return false;
    resetState();
    s_url = url;
    s_mode = mode;
    g_wiifin_loading_active = 1;
    g_wiifin_loading_start_pos = -1.0f;
    s_running = 1;
    logUrl(mode == Mode::Video ? "play" : "play_audio", s_url);
    /* cores 0 and 2: the menus (the main thread) have core 1 */
    s_playThread = startOsThread(playMain, 1024 * 1024, 16,
                                 OS_THREAD_ATTRIB_AFFINITY_CPU0 | OS_THREAD_ATTRIB_AFFINITY_CPU2, "WiiFin player");
    if (!s_playThread) { s_running = 0; return false; }
    return true;
}

void joinThread() { joinOsThread(s_playThread); }

void (*s_musicTick)(int paused) = nullptr;

} // namespace

/* ---- WiiPlayer.h ------------------------------------------------------------- */

float wii_player_time_pos()
{
    if (s_streamStart < 0.0f && g_wiifin_loading_start_pos >= 0.0f) {
        float first = g_wiifin_loading_start_pos, ss = g_wiifin_ss_secs;
        if (g_wiifin_direct || g_wiifin_burned_subs) s_streamStart = 0.0f;
        else if (ss <= 0.0f) s_streamStart = first < 2.0f ? first : TS_START;
        else {
            s_ssMissed = first < ss + TS_START - 1.0f;
            s_streamStart = TS_START;
        }
    }
    return g_mplayer_time_pos - (s_streamStart > 0.05f ? s_streamStart : 0.0f);
}

bool wii_player_ss_missed() { return s_ssMissed; }

int wii_player_start(const char* url) { return startThread(url, Mode::Video) ? 1 : 0; }
int wii_player_is_running(void) { return s_running; }

int wii_player_wait(void)
{
    joinThread();
    g_wiifin_loading_active = 0;
    return (int)g_player_stop_reason;
}

void wii_player_request_stop(int reason)
{
    g_player_stop_reason = reason;
    s_quit = true;
    s_audioStop = true;
}

void wii_player_abort_io(void) { s_quit = true; s_audioStop = true; }
void wii_player_keep_socket(int, int) {}

void wii_player_pause_toggle(void) { s_pauseToggle = true; }

void wii_player_seek_by(float delta)
{
    g_wiifin_loading_start_pos = -1.0f;
    s_seekBy = delta;
    s_seekAsked = true;
}
void wii_player_seek_abs(float seconds) { wii_player_seek_by(seconds - wii_player_time_pos()); }
void wii_player_seek_rel(float delta) { wii_player_seek_by(delta); }

void wii_player_vol_up(void)   { s_muted = 0; s_volume = s_volume + 5 > 100 ? 100 : s_volume + 5; applyVolume(); }
void wii_player_vol_down(void) { s_muted = 0; s_volume = s_volume - 5 < 0 ? 0 : s_volume - 5; applyVolume(); }
void wii_player_set_volume(int v) { s_volume = v < 0 ? 0 : v > 100 ? 100 : v; s_muted = 0; applyVolume(); }
void wii_player_set_mute(int on) { s_muted = on ? 1 : 0; applyVolume(); }
int  wii_player_volume(void) { return s_volume; }
int  wii_player_muted(void) { return s_muted; }

int wii_player_play_audio(const char* url)
{
    if (!startThread(url, Mode::Audio)) return PLAYER_STOP_ERROR;
    while (s_running) {
        if (g_app_powerOff || g_app_reset) wii_player_request_stop(PLAYER_STOP_EOF);
        if (s_musicTick) s_musicTick(g_mplayer_paused);
        sleepMs(16);
    }
    joinThread();
    s_musicTick = nullptr;
    g_wiifin_loading_active = 0;
    return (int)g_player_stop_reason;
}

void wii_player_stop(void) { s_quit = true; s_audioStop = true; }
void wii_player_set_music_tick(void (*cb)(int paused)) { s_musicTick = cb; }

/* ---- vo_wiifin.h (MPlayer's video output on the Wii) ------------------------- */

extern "C" {
int g_wiifin_smooth_motion = 1;
unsigned wiifin_video_frames(void) { return s_shown; }
void wiifin_video_clear(void) { std::lock_guard<std::mutex> g(s_texLock); s_front = -1; s_hwY = s_hwUV = nullptr; }
void wiifin_video_report(const char* when) { SYS_Report("[PlayerWiiU] %s: %u pictures shown\n", when, s_shown); }
}

/* ---- VideoSurface (the Wii's video plane) ------------------------------------ */

namespace {
VideoSurface::Zoom s_zoom = VideoSurface::Zoom::Fit;
}

/* the newest picture, fitted into the main frame's 16:9 space (or filling
 * it), drawn by the GPU from NV12 */
bool VideoSurface::draw()
{
    if (!s_videoActive) return false;
    TexWiiU *y = nullptr, *uv = nullptr;
    {
        std::lock_guard<std::mutex> g(s_texLock);
        const int i = s_front;
        if (i >= 0) { y = s_texY[i]; uv = s_texUV[i]; s_drawnAt[i] = GfxWiiU::frames() + 1; }
        else if (s_hwY) { y = s_hwY; uv = s_hwUV; }
    }
    if (!y || !uv) return false;
    const float L = Ui::screenLeft(), R = Ui::screenRight(), SW = R - L, SH = 480.0f;
    const bool wider = s_aspect > SW / SH;
    const bool fill = s_zoom == VideoSurface::Zoom::Fill;
    float w, h;
    if (wider != fill) { w = SW; h = SW / s_aspect; }
    else               { h = SH; w = SH * s_aspect; }
    const float x = L + (SW - w) * 0.5f, y0 = (SH - h) * 0.5f;
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GfxWiiU::drawNV12(y, uv, x, y0, w, h, s_bt709);
    return true;
}
void VideoSurface::endFrame() {}
void VideoSurface::setZoom(Zoom z) { s_zoom = z; }
VideoSurface::Zoom VideoSurface::zoom() { return s_zoom; }

bool PlayerWiiU::videoActive() { return s_videoActive; }
void PlayerWiiU::pause() { s_pauseForce = true; }

bool PlayerWiiU::drawPreview(float x, float y, float w, float h)
{
    if (!s_videoActive || w <= 0 || h <= 0) return false;
    TexWiiU *ty = nullptr, *tuv = nullptr;
    {
        std::lock_guard<std::mutex> g(s_texLock);
        if (s_front >= 0) { ty = s_texY[s_front]; tuv = s_texUV[s_front]; s_drawnAt[s_front] = GfxWiiU::frames() + 1; }
        else if (s_hwY) { ty = s_hwY; tuv = s_hwUV; }
    }
    if (!ty || !tuv) return false;
    /* covering the box, centred */
    float pw = w, ph = w / s_aspect;
    if (ph < h) { ph = h; pw = h * s_aspect; }
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GfxWiiU::drawNV12(ty, tuv, x + (w - pw) * 0.5f, y + (h - ph) * 0.5f, pw, ph, s_bt709);
    return true;
}

namespace { int s_hd = PlayerWiiU::HD_720; }
void PlayerWiiU::setHd(int hd) { s_hd = hd < HD_OFF ? HD_OFF : hd > HD_1080 ? HD_1080 : hd; }
bool PlayerWiiU::h264Now() { return !s_hwBroken; }
int  PlayerWiiU::hd() { return s_hd; }
int  PlayerWiiU::hdNow()
{
    if (s_hwBroken || GfxWiiU::mode() != GfxWiiU::Mode::Remote) return HD_OFF;
    return s_hd;
}
