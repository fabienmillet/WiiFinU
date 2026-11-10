/* -----------------------------------------------------------------------
 * Player3ds — what MPlayer does on the Wii (WiiPlayer.h), on the 3DS.
 *
 *   reader thread   HTTP(S) -> a ring buffer (the cache)
 *   player thread   FFmpeg demuxes and decodes from it; the sound goes to
 *                   the DSP (ndsp channel 17), the pictures, converted to
 *                   RGB565, into one of three textures, each shown when the
 *                   sound reaches its time
 *   main thread     PlaySession / PlayerView as on the Wii; the top
 *                   screen's drawer shows the newest picture (drawTop)
 *
 * The sound is the clock: its position is the time of the buffer the DSP
 * plays plus the samples played of it.  Without sound, the system clock.
 * ----------------------------------------------------------------------- */
#include "Player3ds.h"
#include "Gfx3ds.h"
#include "../../player/WiiPlayer.h"
#include "../../player/VideoSurface.h"
#include "../../player/stream_wiifin.h"
#include "../../player/vo_wiifin.h"
#include "../../core/Log.h"
#include "../../core/NetConnect.h"

#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string>
#include <deque>
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
#include <libswresample/swresample.h>
}

mbedtls_x509_crt* wiifin_ca_chain();
int wiifin_cert_verify(void*, mbedtls_x509_crt*, int, uint32_t* flags);

/* ---- the Wii's shared state (WiiPlayer.h, stream_wiifin.h) --------------- */

extern "C" {
volatile float g_mplayer_time_pos = 0.0f;
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
    /* a bigger receive window: the 3DS's default is small */
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
        SYS_Report("[Player3ds] TLS handshake failed -0x%04X\n", (unsigned)-ret);
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

/* ---- the cache: a ring buffer between the reader and FFmpeg -------------- */

const size_t RING_SIZE = 6 * 1024 * 1024;
const size_t RING_START = 160 * 1024;     /* playback starts with this much (~2 s) */

struct Ring {
    u8*        buf = nullptr;
    volatile unsigned long long rd = 0, wr = 0;   /* bytes read / written ever */
    long long  base = 0;                 /* stream offset of rd == 0 (after a seek) */
    volatile bool eof = false, error = false;
    volatile long long seekTo = -1;      /* asked by FFmpeg: reopen there */
    LightLock  lock;
    LightEvent changed;
};

Ring  s_ring;
Http* s_http = nullptr;
std::string s_url;
Thread s_readerThread = nullptr;

size_t ringFill() { return (size_t)(s_ring.wr - s_ring.rd); }

/* What PlaySession reads as MPlayer's cache fill (%): its thresholds are
 * for the Wii's 8 MB at 1.5-5 Mb/s (25% about 10 s ahead); the 3DS's
 * streams are a quarter of that, so the same seconds are a quarter of the
 * bytes.  -1: the whole stream is here. */
const size_t CACHE_REF = 2 * 1024 * 1024;
/* read ahead by the player (its queues): still to play, counted in */
volatile size_t s_heldBytes = 0;
void updateFill()
{
    if (s_ring.eof) { cache_fill_status = -1.0f; return; }
    float f = 100.0f * (ringFill() + s_heldBytes) / CACHE_REF;
    cache_fill_status = f > 100.0f ? 100.0f : f;
}

void readerMain(void*)
{
    Http* st = s_http;
    int status = httpOpen(st, s_url, -1);
    if (status < 200 || status >= 300) {
        SYS_Report("[Player3ds] HTTP %d\n", status);
        s_ring.error = true;
        LightEvent_Signal(&s_ring.changed);
        return;
    }
    if (g_stream_opened_cb) g_stream_opened_cb();
    static char tmp[16384];
    while (!s_quit) {
        long long seek = s_ring.seekTo;
        if (seek >= 0) {
            LightLock_Lock(&s_ring.lock);
            s_ring.rd = s_ring.wr = 0;
            s_ring.base = seek;
            s_ring.eof = false;
            s_ring.seekTo = -1;
            LightLock_Unlock(&s_ring.lock);
            status = httpOpen(st, st->url, seek);
            if (status < 200 || status >= 300) { s_ring.error = true; LightEvent_Signal(&s_ring.changed); break; }
            LightEvent_Signal(&s_ring.changed);
        }
        if (s_ring.eof || RING_SIZE - ringFill() < sizeof(tmp)) {
            svcSleepThread(10 * 1000 * 1000);
            continue;
        }
        int n = httpRead(st, tmp, sizeof(tmp));
        if (s_ring.seekTo >= 0) continue;     /* a seek came meanwhile: those bytes are not wanted */
        if (n <= 0) {
            /* a whole file cut short: on again from where it stopped */
            if (!st->done && st->total > 0 && !s_quit) {
                long long at = s_ring.base + (long long)s_ring.wr;
                SYS_Report("[Player3ds] connection lost at %lld, resuming\n", at);
                if (httpOpen(st, st->url, at) / 100 == 2) continue;
            }
            s_ring.eof = true;
            updateFill();
            LightEvent_Signal(&s_ring.changed);
            continue;
        }
        size_t w = (size_t)(s_ring.wr % RING_SIZE);
        size_t first = RING_SIZE - w < (size_t)n ? RING_SIZE - w : (size_t)n;
        memcpy(s_ring.buf + w, tmp, first);
        if ((size_t)n > first) memcpy(s_ring.buf, tmp + first, n - first);
        s_ring.wr += n;
        g_wiifin_stream_bytes += n;
        updateFill();
        LightEvent_Signal(&s_ring.changed);
    }
    connClose(st->conn);
}

int avRead(void*, uint8_t* buf, int size)
{
    for (;;) {
        if (s_quit) return AVERROR_EXIT;
        size_t have = ringFill();
        if (have > 0 && s_ring.seekTo < 0) {
            LightLock_Lock(&s_ring.lock);
            have = ringFill();
            size_t n = have < (size_t)size ? have : (size_t)size;
            size_t r = (size_t)(s_ring.rd % RING_SIZE);
            size_t first = RING_SIZE - r < n ? RING_SIZE - r : n;
            memcpy(buf, s_ring.buf + r, first);
            if (n > first) memcpy(buf + first, s_ring.buf, n - first);
            s_ring.rd += n;
            LightLock_Unlock(&s_ring.lock);
            updateFill();
            if (n > 0) return (int)n;
        }
        if (s_ring.error) return AVERROR(EIO);
        if (s_ring.eof && ringFill() == 0) return AVERROR_EOF;
        LightEvent_WaitTimeout(&s_ring.changed, 100 * 1000 * 1000LL);
    }
}

int64_t avSeek(void*, int64_t off, int whence)
{
    if (whence == AVSEEK_SIZE) return s_http && s_http->total > 0 ? s_http->total : -1;
    if (!s_http || s_http->total <= 0) return -1;
    long long target = whence == SEEK_SET ? off : whence == SEEK_CUR ? s_ring.base + (long long)s_ring.rd + off
                     : s_http->total + off;
    /* within what is buffered: no new request */
    long long pos = s_ring.base + (long long)s_ring.rd;
    if (target >= pos && target <= s_ring.base + (long long)s_ring.wr) {
        LightLock_Lock(&s_ring.lock);
        s_ring.rd += (unsigned long long)(target - pos);
        LightLock_Unlock(&s_ring.lock);
        return target;
    }
    s_ring.seekTo = target;
    while (s_ring.seekTo >= 0 && !s_quit && !s_ring.error)
        LightEvent_WaitTimeout(&s_ring.changed, 50 * 1000 * 1000LL);
    return s_ring.error ? -1 : target;
}

/* ---- sound --------------------------------------------------------------- */

const int AUDIO_CH = 17;
const int AUDIO_BUFS = 24;
const int AUDIO_FRAMES = 2048;          /* stereo frames per buffer */

int    s_stageN = 0;          /* frames gathered in s_wbNext's buffer */
double s_stagePts = 0;
/* pause / resume asked while the sound waits for room (the player's) */
void (*s_pauseHook)() = nullptr;
ndspWaveBuf s_wb[AUDIO_BUFS];
double      s_wbPts[AUDIO_BUFS];
s16*        s_pcm = nullptr;
int         s_wbNext = 0;
int         s_rate = 0;
bool        s_audioOn = false;
volatile int s_volume = 70;
volatile int s_muted = 0;

void applyVolume()
{
    float mix[12] = {};
    mix[0] = mix[1] = s_muted ? 0.0f : s_volume / 100.0f;
    ndspChnSetMix(AUDIO_CH, mix);
}

bool audioStart(int rate)
{
    if (R_FAILED(ndspInit())) return false;     /* (a count: Audio3ds has it too) */
    if (!s_pcm) s_pcm = (s16*)linearAlloc(AUDIO_BUFS * AUDIO_FRAMES * 4);
    if (!s_pcm) { ndspExit(); return false; }
    memset(s_wb, 0, sizeof(s_wb));
    ndspChnReset(AUDIO_CH);
    ndspChnSetInterp(AUDIO_CH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CH, (float)rate);
    ndspChnSetFormat(AUDIO_CH, NDSP_FORMAT_STEREO_PCM16);
    applyVolume();
    s_wbNext = 0;
    s_stageN = 0;
    s_rate = rate;
    s_audioOn = true;
    return true;
}

void audioStop()
{
    if (!s_audioOn) return;
    ndspChnWaveBufClear(AUDIO_CH);
    ndspChnReset(AUDIO_CH);
    ndspExit();
    s_audioOn = false;
}

int audioQueued()
{
    int n = 0;
    for (auto& b : s_wb) n += b.status == NDSP_WBUF_QUEUED || b.status == NDSP_WBUF_PLAYING;
    return n;
}

/* the time heard now; < 0: nothing playing.  The buffer playing is the
 * earliest one still queued (its sequence number from the DSP is not
 * always there: an emulator), the samples played of it on top. */
double audioClock()
{
    if (!s_audioOn) return -1;
    int cur = -1;
    for (int i = 0; i < AUDIO_BUFS; ++i) {
        const u8 st = s_wb[i].status;
        if ((st == NDSP_WBUF_PLAYING || st == NDSP_WBUF_QUEUED) && (cur < 0 || s_wbPts[i] < s_wbPts[cur]))
            cur = i;
    }
    if (cur < 0) return -1;
    u32 pos = ndspChnGetSamplePos(AUDIO_CH);
    if (s_wb[cur].status != NDSP_WBUF_PLAYING || pos > s_wb[cur].nsamples) pos = 0;
    return s_wbPts[cur] + pos / (double)s_rate;
}

/* Samples are gathered into whole DSP buffers (AUDIO_FRAMES); the
 * buffers queued are the sound ahead (24 x 43 ms: about a second). */

bool audioRoom() { return s_audioOn && audioQueued() < AUDIO_BUFS - 1; }

void audioSubmit()
{
    if (s_stageN <= 0) return;
    ndspWaveBuf& wb = s_wb[s_wbNext];
    s16* dst = s_pcm + s_wbNext * AUDIO_FRAMES * 2;
    DSP_FlushDataCache(dst, s_stageN * 4);
    memset(&wb, 0, sizeof(wb));
    wb.data_vaddr = dst;
    wb.nsamples = s_stageN;
    s_wbPts[s_wbNext] = s_stagePts;
    ndspChnWaveBufAdd(AUDIO_CH, &wb);
    s_wbNext = (s_wbNext + 1) % AUDIO_BUFS;
    s_stageN = 0;
}

/* frames samples (interleaved stereo s16) from pts.  Waits only when the
 * whole second ahead is queued (handling a pause meanwhile).  false:
 * stopping. */
bool audioPush(const s16* pcm, int frames, double pts)
{
    while (frames > 0) {
        if (s_stageN == 0) {
            ndspWaveBuf& wb = s_wb[s_wbNext];
            while (wb.status == NDSP_WBUF_QUEUED || wb.status == NDSP_WBUF_PLAYING) {
                if (s_quit) return false;
                if (s_pauseHook) s_pauseHook();
                svcSleepThread(4 * 1000 * 1000);
            }
            s_stagePts = pts;
        }
        int n = AUDIO_FRAMES - s_stageN;
        if (n > frames) n = frames;
        memcpy(s_pcm + (s_wbNext * AUDIO_FRAMES + s_stageN) * 2, pcm, n * 4);
        s_stageN += n;
        pcm += n * 2;
        frames -= n;
        pts += n / (double)s_rate;
        if (s_stageN == AUDIO_FRAMES) audioSubmit();
    }
    return true;
}

void audioFlushQueue()
{
    if (!s_audioOn) return;
    ndspChnWaveBufClear(AUDIO_CH);
    memset(s_wb, 0, sizeof(s_wb));
    s_wbNext = 0;
    s_stageN = 0;
}

/* ---- pictures -------------------------------------------------------------- */

const int NTEX = 3;
Tex3ds*      s_tex[NTEX] = {};
int          s_texW = 0, s_texH = 0;
float        s_aspect = 16.0f / 9.0f;   /* the picture's display aspect */
volatile int s_front = -1;              /* the newest picture */
volatile int s_drawn = -1;              /* the one the GPU may be drawing */
LightLock    s_texLock;
u16*         s_rgb = nullptr;           /* conversion rows */
volatile bool s_videoActive = false;

void videoFree()
{
    s_videoActive = false;
    LightLock_Lock(&s_texLock);
    for (auto& t : s_tex) { Gfx3ds::destroy(t); t = nullptr; }
    s_front = s_drawn = -1;
    LightLock_Unlock(&s_texLock);
    free(s_rgb);
    s_rgb = nullptr;
}

bool videoAlloc(int w, int h)
{
    videoFree();
    s_rgb = (u16*)malloc((size_t)w * h * 2);
    if (!s_rgb) return false;
    for (auto& t : s_tex) {
        t = Gfx3ds::make(Gfx3ds::Fmt::RGB565, w, h, nullptr, 0, false);
        if (!t) { videoFree(); return false; }
    }
    s_texW = w; s_texH = h;
    return true;
}

/* YUV 4:2:0 to RGB565 rows (BT.601, video range) */
void yuvToRgb565(const AVFrame* f, u16* out, int w, int h)
{
    for (int y = 0; y < h; y += 2) {
        const u8* y0 = f->data[0] + y * f->linesize[0];
        const u8* y1 = y0 + (y + 1 < h ? f->linesize[0] : 0);
        const u8* u  = f->data[1] + (y / 2) * f->linesize[1];
        const u8* v  = f->data[2] + (y / 2) * f->linesize[2];
        u16* o0 = out + y * w;
        u16* o1 = y + 1 < h ? o0 + w : o0;
        for (int x = 0; x < w; x += 2) {
            const int cu = u[x / 2] - 128, cv = v[x / 2] - 128;
            const int rr = 409 * cv + 128, gg = -100 * cu - 208 * cv + 128, bb = 516 * cu + 128;
            #define PIX(Y, O) do { const int c = 298 * ((Y) - 16); \
                int r = (c + rr) >> 8, g = (c + gg) >> 8, b = (c + bb) >> 8; \
                r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b; \
                (O) = (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); } while (0)
            PIX(y0[x], o0[x]);
            if (x + 1 < w) PIX(y0[x + 1], o0[x + 1]);
            if (o1 != o0) { PIX(y1[x], o1[x]); if (x + 1 < w) PIX(y1[x + 1], o1[x + 1]); }
            #undef PIX
        }
    }
}

/* the picture into a texture the screen isn't using, then shown */
unsigned s_shown = 0;   /* pictures shown (wiifin_video_frames) */
unsigned s_skipped = 0; /* decoded too late, not shown */
/* CPU time (system ticks) decoding, for the log: how much of the CPU
 * playback takes */
u64 s_vTicks = 0, s_aTicks = 0;

/* The 3DS's YUV-to-RGB hardware (Y2R) writes the picture straight into
 * the texture, in the GPU's 8x8 tiles: no work for the CPU, which the
 * old 3DS needs for decoding.  Widths of 8n only (Jellyfin's 400, 320...),
 * whole rows of tiles (a picture 8n+k lines tall loses its last k). */
int    s_y2r = 0;          /* 0 not tried, 1 up, -1 none */
Handle s_y2rEnd = 0;

bool y2rConvert(const AVFrame* f, Tex3ds* t, int w, int h)
{
    if (s_y2r == 0) {
        s_y2r = R_SUCCEEDED(y2rInit()) && R_SUCCEEDED(Y2RU_GetTransferEndEvent(&s_y2rEnd)) ? 1 : -1;
        SYS_Report("[Player3ds] colour conversion: %s\n", s_y2r > 0 ? "Y2R hardware" : "software");
    }
    const int lines = h & ~7;
    if (s_y2r < 0 || (w & 7) || lines <= 0 || f->format != AV_PIX_FMT_YUV420P) return false;
    Y2RU_ConversionParams p;
    memset(&p, 0, sizeof(p));
    p.input_format = INPUT_YUV420_INDIV_8;
    p.output_format = OUTPUT_RGB_16_565;
    p.rotation = ROTATION_NONE;
    p.block_alignment = BLOCK_8_BY_8;
    p.input_line_width = (s16)w;
    p.input_lines = (s16)lines;
    p.standard_coefficient = COEFFICIENT_ITU_R_BT_601_SCALING;
    p.alpha = 0xFF;
    if (R_FAILED(Y2RU_SetConversionParams(&p))) return false;
    for (int i = 0; i < 3; ++i)
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)f->data[i], f->linesize[i] * (i ? lines / 2 : lines));
    Y2RU_SetSendingY(f->data[0], w * lines, w, f->linesize[0] - w);
    Y2RU_SetSendingU(f->data[1], w / 2 * lines / 2, w / 2, f->linesize[1] - w / 2);
    Y2RU_SetSendingV(f->data[2], w / 2 * lines / 2, w / 2, f->linesize[2] - w / 2);
    int pw, ph;
    void* dst = Gfx3ds::pixels(t, pw, ph);
    /* a row of tiles at a time, the texture's wider rows skipped */
    Y2RU_SetReceiving(dst, w * lines * 2, w * 8 * 2, (pw - w) * 8 * 2);
    if (R_FAILED(Y2RU_StartConversion())) return false;
    if (svcWaitSynchronization(s_y2rEnd, 200 * 1000 * 1000LL) != 0) {
        Y2RU_StopConversion();
        SYS_Report("[Player3ds] Y2R timed out: software from now on\n");
        s_y2r = -1;
        return false;
    }
    return true;
}

void videoShow(const AVFrame* f)
{
    ++s_shown;
    if (!s_rgb) return;
    LightLock_Lock(&s_texLock);
    int target = 0;
    while (target == s_front || target == s_drawn) ++target;
    LightLock_Unlock(&s_texLock);
    if (!y2rConvert(f, s_tex[target], s_texW, s_texH)) {
        yuvToRgb565(f, s_rgb, s_texW, s_texH);
        Gfx3ds::update(s_tex[target], s_rgb, s_texW * 2);
    }
    LightLock_Lock(&s_texLock);
    s_front = target;
    LightLock_Unlock(&s_texLock);
}

/* ---- the player thread ----------------------------------------------------- */

enum class Mode { Video, Audio };

Mode         s_mode = Mode::Video;
Thread       s_playThread = nullptr;
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
    SYS_Report("[Player3ds] %s: %s\n", tag, safe.c_str());
}

u64 s_pausedSince = 0, s_sysStart = 0;
double s_sysPts0 = 0;

void handlePause()
{
    if (!s_pauseToggle) return;
    s_pauseToggle = false;
    g_mplayer_paused = !g_mplayer_paused;
    if (s_audioOn) ndspChnSetPaused(AUDIO_CH, g_mplayer_paused);
    if (g_mplayer_paused) s_pausedSince = gettime();
    else if (s_pausedSince) { if (s_sysStart) s_sysStart += gettime() - s_pausedSince; s_pausedSince = 0; }
    SYS_Report("[Player3ds] %s at %.1f s\n", g_mplayer_paused ? "paused" : "resumed", (double)g_mplayer_time_pos);
}

void playMain(void*)
{
    s_pauseHook = handlePause;
    s_ring.buf = (u8*)malloc(RING_SIZE);
    s_http = new Http();
    AVFormatContext* fmt = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext *vdec = nullptr, *adec = nullptr;
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
    LightLock_Init(&s_ring.lock);
    LightEvent_Init(&s_ring.changed, RESET_ONESHOT);
    {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        /* the reader above the decoder: the cache is never starved */
        s_readerThread = threadCreate(readerMain, nullptr, 64 * 1024, prio > 0x18 ? prio - 1 : prio, -2, false);
        if (!s_readerThread) goto out;
    }
    /* some seconds in the cache before playing (MPlayer's -cache-min 8) */
    while (!s_quit && !s_ring.error && !s_ring.eof && ringFill() < RING_START)
        LightEvent_WaitTimeout(&s_ring.changed, 100 * 1000 * 1000LL);
    if (s_quit || s_ring.error) goto out;

    {
        const int IOBUF = 32 * 1024;
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
            av_dict_set(&opts, "probesize", big ? "1048576" : "65536", 0);
            av_dict_set(&opts, "analyzeduration", big ? "10000000" : "1500000", 0);
        } else if (g_wiifin_direct && strncmp(g_wiifin_demuxer, "lavf:", 5) == 0) {
            ifmt = av_find_input_format(g_wiifin_demuxer + 5);
        }
        if (avformat_open_input(&fmt, nullptr, ifmt, &opts) < 0) {
            av_dict_free(&opts);
            SYS_Report("[Player3ds] cannot open the stream\n");
            fmt = nullptr;
            goto out;
        }
        av_dict_free(&opts);
        if (avformat_find_stream_info(fmt, nullptr) < 0) SYS_Report("[Player3ds] no stream info\n");
    }
    if (s_mode == Mode::Video) vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    aIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, g_wiifin_aid >= 0 && s_mode == Mode::Video ? -1 : -1,
                               vIdx >= 0 ? vIdx : -1, nullptr, 0);
    if (vIdx < 0 && aIdx < 0) { SYS_Report("[Player3ds] no audio nor video\n"); goto out; }
    if (fmt->duration > 0) g_mplayer_duration = (float)(fmt->duration / (double)AV_TIME_BASE);

    if (vIdx >= 0) {
        AVStream* st = fmt->streams[vIdx];
        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        vdec = c ? avcodec_alloc_context3(c) : nullptr;
        if (vdec && avcodec_parameters_to_context(vdec, st->codecpar) >= 0) {
            vdec->thread_count = 1;
            vdec->flags2 |= AV_CODEC_FLAG2_FAST;
            vdec->skip_loop_filter = AVDISCARD_ALL;
            if (avcodec_open2(vdec, c, nullptr) < 0) avcodec_free_context(&vdec);
        } else if (vdec) avcodec_free_context(&vdec);
        if (!vdec) { SYS_Report("[Player3ds] no decoder for video codec %d\n", (int)st->codecpar->codec_id); vIdx = -1; }
        else {
            int w = st->codecpar->width, h = st->codecpar->height;
            AVRational sar = st->codecpar->sample_aspect_ratio;
            s_aspect = h > 0 ? (float)w / h * (sar.num > 0 && sar.den > 0 ? (float)sar.num / sar.den : 1.0f) : 16.0f / 9.0f;
            SYS_Report("[Player3ds] video %s %dx%d aspect %.2f\n", c->name, w, h, (double)s_aspect);
            if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || !videoAlloc(w, h)) {
                SYS_Report("[Player3ds] picture %dx%d not possible\n", w, h);
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
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, rate, &adec->ch_layout,
                                    adec->sample_fmt, rate, 0, nullptr) < 0 || swr_init(swr) < 0 ||
                !audioStart(rate)) {
                SYS_Report("[Player3ds] no sound output\n");
                swr_free(&swr);
                avcodec_free_context(&adec);
                aIdx = -1;
            } else {
                SYS_Report("[Player3ds] audio %s %d Hz %d ch\n", c->name, rate, adec->ch_layout.nb_channels);
            }
        } else {
            if (adec) avcodec_free_context(&adec);
            aIdx = -1;
        }
    }
    if (vIdx < 0 && aIdx < 0) goto out;
    s_videoActive = vIdx >= 0;
    failed = false;

    {
        /* Packets are read ahead of the pictures: the sound is decoded as
         * soon as it comes (a second of it queued), the video's packets
         * wait in a queue, decoded one picture at a time, each shown when
         * the sound gets to it.  While a picture waits, more is read. */
        std::deque<AVPacket*> vq;
        const size_t VQ_MAX = 240;
        bool eof = false, havePic = false;
        double picPts = 0;
        AVFrame* pic = av_frame_alloc();
        const AVRational vtb = vIdx >= 0 ? fmt->streams[vIdx]->time_base : AVRational{ 1, 1 };
        const AVRational atb = aIdx >= 0 ? fmt->streams[aIdx]->time_base : AVRational{ 1, 1 };
        size_t vqBytes = 0;
        const int audioBps = adec && adec->bit_rate > 0 ? (int)(adec->bit_rate / 8) : 12000;
        auto held = [&]() {
            size_t a = s_audioOn ? (size_t)(audioQueued() * (double)AUDIO_FRAMES / s_rate * audioBps) : 0;
            s_heldBytes = vqBytes + a;
            updateFill();
        };
        auto clearQueue = [&]() { for (AVPacket* q : vq) av_packet_free(&q); vq.clear(); vqBytes = 0; havePic = false; };

        while (!s_quit) {
            if (g_app_powerOff || g_app_reset) { g_player_stop_reason = PLAYER_STOP_EOF; break; }
            handlePause();
            if (g_mplayer_paused) { svcSleepThread(20 * 1000 * 1000); continue; }
            /* a seek (a whole file) */
            if (s_seekAsked) {
                s_seekAsked = false;
                double target = g_mplayer_time_pos + s_seekBy;
                if (target < 0) target = 0;
                if (av_seek_frame(fmt, -1, (int64_t)(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) >= 0) {
                    if (vdec) avcodec_flush_buffers(vdec);
                    if (adec) avcodec_flush_buffers(adec);
                    clearQueue();
                    audioFlushQueue();
                    eof = false;
                    started = false;
                    sysStart = 0;
                    g_wiifin_loading_active = 1;
                }
            }

            /* read: until the sound is a second ahead, or enough pictures wait */
            int reads = 0;
            while (!eof && !s_quit && reads < 8 && (aIdx < 0 || audioRoom()) && vq.size() < VQ_MAX) {
                int r = av_read_frame(fmt, pkt);
                if (r < 0) {
                    if (r == AVERROR(EAGAIN)) break;
                    if (r != AVERROR_EOF && !s_ring.eof) g_player_stop_reason = PLAYER_STOP_ERROR;
                    eof = true;
                    break;
                }
                ++reads;
                if (pkt->stream_index == aIdx && adec) {
                    const u64 t0 = svcGetSystemTick();
                    struct AT { u64 t0; ~AT() { s_aTicks += svcGetSystemTick() - t0; } } at{ t0 };
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
                } else if (pkt->stream_index == vIdx && vdec) {
                    AVPacket* q = av_packet_clone(pkt);
                    if (q) { vq.push_back(q); vqBytes += q->size; }
                }
                av_packet_unref(pkt);
            }
            held();
            /* the last bits of sound gathered: heard too */
            if ((eof || (vIdx >= 0 && vq.empty() && !havePic)) && s_stageN > 0) audioSubmit();

            if (vIdx < 0) {
                double t = audioClock();
                if (t >= 0) g_mplayer_time_pos = (float)t;
                if (eof) {
                    if (audioQueued() == 0) break;
                    svcSleepThread(20 * 1000 * 1000);
                } else if (!audioRoom()) svcSleepThread(10 * 1000 * 1000);
                continue;
            }

            /* the next picture */
            const u64 vt0 = svcGetSystemTick();
            while (!havePic && !vq.empty()) {
                AVPacket* q = vq.front();
                vq.pop_front();
                vqBytes -= q->size;
                int sr = avcodec_send_packet(vdec, q);
                av_packet_free(&q);
                if (sr < 0) continue;
                if (avcodec_receive_frame(vdec, pic) >= 0) {
                    havePic = true;
                    picPts = pic->best_effort_timestamp != AV_NOPTS_VALUE ? pic->best_effort_timestamp * av_q2d(vtb) : 0;
                }
            }
            s_vTicks += svcGetSystemTick() - vt0;
            if (!havePic) {
                if (eof) {
                    /* the end: what sound is queued, heard */
                    if (s_audioOn && audioQueued() > 0) {
                        double t = audioClock();
                        if (t >= 0) g_mplayer_time_pos = (float)t;
                        svcSleepThread(20 * 1000 * 1000);
                        continue;
                    }
                    break;
                }
                if (reads == 0) svcSleepThread(5 * 1000 * 1000);   /* waiting for data */
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
                if (reads == 0) svcSleepThread((s64)((ahead > 0.01 ? 0.01 : ahead) * 1e9));
                continue;
            }
            havePic = false;
            if (ahead < -0.15 && started) {
                ++s_skipped;
                continue;     /* behind: not shown */
            }
            if (pic->width != s_texW || pic->height != s_texH) {
                if (!videoAlloc(pic->width, pic->height)) continue;
            }
            videoShow(pic);
            g_mplayer_time_pos = (float)picPts;
            if (!started) {
                started = true;
                g_wiifin_loading_start_pos = (float)picPts;
                g_wiifin_loading_active = 0;
                SYS_Report("[Player3ds] first picture at %.2f s\n", picPts);
            }
            {
                static u64 lastLog = 0;
                const u64 t = gettime();
                if (t - lastLog > 5000000ULL) {
                    static u64 lastTick = 0;
                    const u64 now = svcGetSystemTick();
                    const double span = lastTick ? (double)(now - lastTick) : 0;
                    SYS_Report("[Player3ds] at %.1f s, audio %.2f s (%d queued), cache %.0f KB, "
                               "%u pictures, %u late, %u waiting, CPU video %.0f%% audio %.0f%%\n",
                               picPts, audioClock(), s_audioOn ? audioQueued() : 0,
                               ringFill() / 1024.0, s_shown, s_skipped, (unsigned)vq.size(),
                               span > 0 ? 100.0 * s_vTicks / span : 0.0, span > 0 ? 100.0 * s_aTicks / span : 0.0);
                    lastLog = t;
                    lastTick = now;
                    s_vTicks = s_aTicks = 0;
                }
            }
        }
        clearQueue();
        s_heldBytes = 0;
        av_frame_free(&pic);
    }

out:
    if (failed && g_player_stop_reason == PLAYER_STOP_EOF) g_player_stop_reason = PLAYER_STOP_ERROR;
    s_quit = true;
    LightEvent_Signal(&s_ring.changed);
    if (s_readerThread) { threadJoin(s_readerThread, U64_MAX); threadFree(s_readerThread); s_readerThread = nullptr; }
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
    if (s_http) { connClose(s_http->conn); delete s_http; s_http = nullptr; }
    free(s_ring.buf);
    s_ring.buf = nullptr;
    /* nothing played: an error, the caller asks the server again */
    if (g_player_stop_reason == PLAYER_STOP_EOF && g_mplayer_time_pos < 2.0f && g_mplayer_duration == 0.0f &&
        s_mode == Mode::Video)
        g_player_stop_reason = PLAYER_STOP_ERROR;
    SYS_Report("[Player3ds] stopped: reason %d at %.1f s\n", (int)g_player_stop_reason, (double)g_mplayer_time_pos);
    s_running = 0;
}

void resetState()
{
    s_quit = false;
    g_player_stop_reason = PLAYER_STOP_EOF;
    g_mplayer_time_pos = 0.0f;
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
    static bool lockInit = false;
    if (!lockInit) { LightLock_Init(&s_texLock); lockInit = true; }
    resetState();
    s_url = url;
    s_mode = mode;
    g_wiifin_loading_active = 1;
    g_wiifin_loading_start_pos = -1.0f;
    s_running = 1;
    logUrl(mode == Mode::Video ? "play" : "play_audio", s_url);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* below the main thread (the menus stay smooth); on the New 3DS, its
     * third core */
    int core = -2;
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (n3ds) core = 2;
    s_playThread = threadCreate(playMain, nullptr, 256 * 1024, prio < 0x3F ? prio + 1 : prio, core, false);
    if (!s_playThread && core != -2) {
        core = -2;
        s_playThread = threadCreate(playMain, nullptr, 256 * 1024, prio < 0x3F ? prio + 1 : prio, -2, false);
    }
    SYS_Report("[Player3ds] %s, decoding on %s\n", n3ds ? "New 3DS" : "3DS",
               core == 2 ? "core 2" : "the application's core");
    if (!s_playThread) { s_running = 0; return false; }
    return true;
}

void joinThread()
{
    if (!s_playThread) return;
    threadJoin(s_playThread, U64_MAX);
    threadFree(s_playThread);
    s_playThread = nullptr;
}

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
}

void wii_player_abort_io(void) { s_quit = true; }
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

void wii_player_vol_up(void)   { s_muted = 0; s_volume = s_volume + 5 > 100 ? 100 : s_volume + 5; if (s_audioOn) applyVolume(); }
void wii_player_vol_down(void) { s_muted = 0; s_volume = s_volume - 5 < 0 ? 0 : s_volume - 5; if (s_audioOn) applyVolume(); }
void wii_player_set_volume(int v) { s_volume = v < 0 ? 0 : v > 100 ? 100 : v; s_muted = 0; if (s_audioOn) applyVolume(); }
void wii_player_set_mute(int on) { s_muted = on ? 1 : 0; if (s_audioOn) applyVolume(); }
int  wii_player_volume(void) { return s_volume; }
int  wii_player_muted(void) { return s_muted; }

int wii_player_play_audio(const char* url)
{
    if (!startThread(url, Mode::Audio)) return PLAYER_STOP_ERROR;
    while (s_running) {
        if (g_app_powerOff || g_app_reset) wii_player_request_stop(PLAYER_STOP_EOF);
        if (s_musicTick) s_musicTick(g_mplayer_paused);
        svcSleepThread(16 * 1000 * 1000);
    }
    joinThread();
    s_musicTick = nullptr;
    g_wiifin_loading_active = 0;
    return (int)g_player_stop_reason;
}

void wii_player_stop(void) { s_quit = true; }
void wii_player_set_music_tick(void (*cb)(int paused)) { s_musicTick = cb; }

/* ---- vo_wiifin.h (MPlayer's video output on the Wii) ------------------------- */

extern "C" {
int g_wiifin_smooth_motion = 1;
unsigned wiifin_video_frames(void) { return s_shown; }
void wiifin_video_clear(void) { LightLock_Lock(&s_texLock); s_front = -1; LightLock_Unlock(&s_texLock); }
void wiifin_video_report(const char* when) { SYS_Report("[Player3ds] %s: %u pictures shown\n", when, s_shown); }
}

/* ---- VideoSurface (the Wii's video plane) ------------------------------------ */

namespace {
VideoSurface::Zoom s_zoom = VideoSurface::Zoom::Fit;
}

/* PlayerView asks whether there is a picture (it draws its controls over
 * it); the picture itself is on the top screen (drawTop) */
bool VideoSurface::draw() { return s_videoActive && s_front >= 0; }
void VideoSurface::endFrame() {}
void VideoSurface::setZoom(Zoom z) { s_zoom = z; }
VideoSurface::Zoom VideoSurface::zoom() { return s_zoom; }

bool Player3ds::videoActive() { return s_videoActive; }

namespace {
void (*s_overlay)(void*) = nullptr;
void* s_overlayCtx = nullptr;
unsigned s_overlayFrame = ~0u;
}

void Player3ds::setTopOverlay(void (*draw)(void*), void* ctx)
{
    s_overlay = draw;
    s_overlayCtx = ctx;
    s_overlayFrame = Gfx3ds::frames();
}

bool Player3ds::drawTop()
{
    if (!s_videoActive) return false;
    LightLock_Lock(&s_texLock);
    const int i = s_front;
    s_drawn = i;
    Tex3ds* t = i >= 0 ? s_tex[i] : nullptr;
    LightLock_Unlock(&s_texLock);
    if (!t) return false;
    /* the top screen's space is 800x480 (5:3, square pixels): fitted
     * whole, or filling it (the edges cut) */
    const bool wider = s_aspect > 800.0f / 480.0f;
    const bool fill = s_zoom == VideoSurface::Zoom::Fill;
    float w, h;
    if (wider != fill) { w = 800; h = 800 / s_aspect; }
    else               { h = 480; w = 480 * s_aspect; }
    const float x = (800 - w) * 0.5f, y = (480 - h) * 0.5f;
    GXTexObj obj;
    GX_InitTexObj(&obj, t, Gfx3ds::width(t), Gfx3ds::height(t), GX_TF_3DS, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_LoadTexObj(&obj, GX_TEXMAP0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, 0);         GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(0, 0);
    GX_Position3f32(x + w, y, 0);     GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(1, 0);
    GX_Position3f32(x + w, y + h, 0); GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(1, 1);
    GX_Position3f32(x, y + h, 0);     GX_Color1u32(0xFFFFFFFF); GX_TexCoord2f32(0, 1);
    GX_End();
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
    /* asked this frame (the view that asked still there) */
    if (s_overlay && s_overlayFrame == Gfx3ds::frames()) s_overlay(s_overlayCtx);
    return true;
}
