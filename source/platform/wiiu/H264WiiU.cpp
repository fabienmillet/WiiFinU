/* -----------------------------------------------------------------------
 * H264WiiU — the console's H.264 decoder (h264.rpl), see H264WiiU.h.
 *
 * As Moonlight's Wii U port uses it: the decoder's memory and each frame
 * buffer may not cross one of its memory segments (checked, else taken
 * elsewhere), frames are NV12 with rows of 256-byte multiples and a height
 * of 16-line multiples, the UV plane after the Y one.  The output callback
 * (called during Execute and Flush) gives the pictures in display order.
 * ----------------------------------------------------------------------- */
#include "H264WiiU.h"
#include "GfxWiiU.h"

#include <h264/decode.h>
#include <malloc.h>
#include <string.h>
#include <deque>
#include <mutex>
#include <vector>
#include <ogc/system.h>

namespace {

const int POOL = 20;              /* frames: 16 references at most, those to show */
const uint32_t ALIGN = 0x400;

struct Frame { void* buf; TexWiiU* y; TexWiiU* uv; };

void*    s_mem = nullptr;
std::vector<Frame> s_pool;
int      s_next = 0;
int      s_w = 0, s_h = 0, s_pitch = 0, s_alignedH = 0;
uint8_t* s_bits = nullptr;
uint32_t s_bitsCap = 0;
unsigned s_decoded = 0;
bool     s_begun = false;

struct Out { int frame; double pts; };
std::deque<Out> s_out;
std::mutex      s_outLock;

/* memory the decoder takes: not across one of its segments */
void* allocFor(uint32_t size)
{
    std::vector<void*> refused;
    void* p = nullptr;
    for (int tries = 0; tries < 6 && !p; ++tries) {
        void* q = memalign(ALIGN, size);
        if (!q) break;
        if (H264DECCheckMemSegmentation(q, size) == H264_ERROR_OK) p = q;
        else refused.push_back(q);   /* kept meanwhile: the next one lands elsewhere */
    }
    for (void* q : refused) free(q);
    return p;
}

void onOutput(H264DecodeOutput* o)
{
    if (!o) return;
    std::lock_guard<std::mutex> g(s_outLock);
    for (int i = 0; i < o->frameCount; ++i) {
        H264DecodeResult* r = o->decodeResults[i];
        if (!r || !r->framebuffer) continue;
        for (int k = 0; k < (int)s_pool.size(); ++k)
            if (s_pool[k].buf == r->framebuffer) { s_out.push_back({ k, r->timestamp }); ++s_decoded; break; }
    }
}

bool begin()
{
    if (H264DECSetParam_FPTR_OUTPUT(s_mem, onOutput) != H264_ERROR_OK ||
        H264DECSetParam_OUTPUT_PER_FRAME(s_mem, 1) != H264_ERROR_OK ||
        H264DECOpen(s_mem) != H264_ERROR_OK) return false;
    if (H264DECBegin(s_mem) != H264_ERROR_OK) { H264DECClose(s_mem); return false; }
    s_begun = true;
    return true;
}

} // namespace

namespace H264WiiU {

bool open(int width, int height, int profile)
{
    close();
    if (profile != 66 && profile != 77 && profile != 100) {
        SYS_Report("[H264WiiU] profile %d: not the decoder's (66, 77, 100)\n", profile);
        return false;
    }
    if (width <= 0 || height <= 0 || width > 1920 || height > 1088) return false;
    uint32_t need = 0;
    if (H264DECMemoryRequirement(100, 42, 1920, 1088, &need) != H264_ERROR_OK || !need) {
        SYS_Report("[H264WiiU] no memory requirement\n");
        return false;
    }
    s_mem = allocFor(need);
    if (!s_mem) { SYS_Report("[H264WiiU] no memory (%u bytes)\n", (unsigned)need); return false; }
    if (H264DECInitParam((int32_t)need, s_mem) != H264_ERROR_OK) {
        SYS_Report("[H264WiiU] InitParam failed\n");
        free(s_mem); s_mem = nullptr;
        return false;
    }
    if (!begin()) {
        SYS_Report("[H264WiiU] the decoder did not open\n");
        free(s_mem); s_mem = nullptr;
        return false;
    }
    s_w = width; s_h = height;
    s_pitch = (width + 255) & ~255;
    s_alignedH = (height + 15) & ~15;
    const uint32_t frameSize = (uint32_t)s_pitch * s_alignedH * 3 / 2;
    for (int i = 0; i < POOL; ++i) {
        void* buf = allocFor(frameSize);
        if (!buf) break;
        memset(buf, 0, (size_t)s_pitch * s_alignedH);
        memset((uint8_t*)buf + (size_t)s_pitch * s_alignedH, 128, (size_t)s_pitch * s_alignedH / 2);
        Frame f;
        f.buf = buf;
        /* the Y texture owns the frame (freed when the GPU is done with it) */
        f.y = GfxWiiU::wrapMemory(GfxWiiU::Fmt::R8, width, height, buf, s_pitch, true);
        f.uv = GfxWiiU::wrapMemory(GfxWiiU::Fmt::RG8, (width + 1) / 2, (height + 1) / 2,
                                   (uint8_t*)buf + (size_t)s_pitch * s_alignedH, s_pitch, false);
        if (!f.y || !f.uv) { GfxWiiU::destroy(f.uv); if (f.y) GfxWiiU::destroy(f.y); else free(buf); break; }
        GfxWiiU::flush(f.y);
        s_pool.push_back(f);
    }
    if ((int)s_pool.size() < 8) {
        SYS_Report("[H264WiiU] only %d frames of %u bytes\n", (int)s_pool.size(), (unsigned)frameSize);
        close();
        return false;
    }
    s_next = 0;
    s_decoded = 0;
    SYS_Report("[H264WiiU] %dx%d profile %d: %u bytes for the decoder, %d frames of %u bytes\n", width, height,
               profile, (unsigned)need, (int)s_pool.size(), (unsigned)frameSize);
    return true;
}

void close()
{
    if (s_mem) {
        if (s_begun) {
            H264DECFlush(s_mem);
            H264DECEnd(s_mem);
            H264DECClose(s_mem);
            s_begun = false;
        }
        free(s_mem);
        s_mem = nullptr;
    }
    for (Frame& f : s_pool) { GfxWiiU::destroy(f.uv); GfxWiiU::destroy(f.y); }
    s_pool.clear();
    {
        std::lock_guard<std::mutex> g(s_outLock);
        s_out.clear();
    }
    free(s_bits);
    s_bits = nullptr;
    s_bitsCap = 0;
}

bool isOpen() { return s_mem != nullptr; }

bool decode(const uint8_t* data, int size, double pts)
{
    if (!s_mem || s_pool.empty() || size <= 0) return false;
    /* the decoder reads from aligned memory, with room past the end */
    if ((uint32_t)size + 64 > s_bitsCap) {
        free(s_bits);
        s_bitsCap = ((uint32_t)size + 64 + 0xFFFF) & ~0xFFFFu;
        s_bits = (uint8_t*)memalign(ALIGN, s_bitsCap);
        if (!s_bits) { s_bitsCap = 0; return false; }
    }
    memcpy(s_bits, data, size);
    memset(s_bits + size, 0, 64);
    int r = H264DECSetBitstream(s_mem, s_bits, (uint32_t)size, pts);
    if (r != H264_ERROR_OK) {
        SYS_Report("[H264WiiU] SetBitstream: 0x%07X\n", r);
        return false;
    }
    Frame& f = s_pool[s_next];
    s_next = (s_next + 1) % (int)s_pool.size();
    r = H264DECExecute(s_mem, f.buf);
    if ((r & ~0xFF) != 0) {
        SYS_Report("[H264WiiU] Execute: 0x%07X\n", r);
        return false;
    }
    return true;
}

void drain()
{
    if (s_mem && s_begun) H264DECFlush(s_mem);
}

void reset()
{
    if (!s_mem) return;
    if (s_begun) {
        H264DECFlush(s_mem);
        H264DECEnd(s_mem);
        H264DECBegin(s_mem);
    }
    std::lock_guard<std::mutex> g(s_outLock);
    s_out.clear();
}

bool next(TexWiiU*& y, TexWiiU*& uv, double& pts)
{
    std::lock_guard<std::mutex> g(s_outLock);
    if (s_out.empty()) return false;
    const Out o = s_out.front();
    s_out.pop_front();
    y = s_pool[o.frame].y;
    uv = s_pool[o.frame].uv;
    pts = o.pts;
    return true;
}

unsigned decoded() { return s_decoded; }

}
