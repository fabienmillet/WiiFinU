/* -----------------------------------------------------------------------
 * SwkbdWiiU — the Wii U's keyboard (nn::swkbd), see SwkbdWiiU.h; as the
 * SDL2 port for the Wii U drives it (its work memory and an FSClient to
 * create it, Calc once a frame with the controllers' state, DrawTV /
 * DrawDRC over the frame).
 * ----------------------------------------------------------------------- */
#include "SwkbdWiiU.h"
#include "GfxWiiU.h"
#include "InputWiiU.h"

#include <nn/swkbd.h>
#include <gx2/context.h>
#include <coreinit/filesystem.h>
#include <coreinit/mcp.h>
#include <coreinit/userconfig.h>
#include <malloc.h>
#include <string.h>
#include <ogc/system.h>

namespace {

enum class St { Off, Shown, Ok, Cancel };

bool      s_created = false, s_failed = false;
void*     s_work = nullptr;
FSClient* s_fs = nullptr;
St        s_st = St::Off;
unsigned  s_lastAsked = 0;        /* the frame edit() was last called in */
std::u16string s_initial, s_ok, s_result;

std::u16string toU16(const std::string& s)
{
    std::u16string o;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = (uint8_t)s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        if (n) c &= 0x3F >> n;
        ++i;
        for (int k = 0; k < n && i < s.size(); ++k, ++i) c = (c << 6) | ((uint8_t)s[i] & 0x3F);
        if (c >= 0x10000) { c -= 0x10000; o += (char16_t)(0xD800 + (c >> 10)); o += (char16_t)(0xDC00 + (c & 0x3FF)); }
        else o += (char16_t)c;
    }
    return o;
}

std::string toU8(const char16_t* s)
{
    std::string o;
    for (; s && *s; ++s) {
        uint32_t c = *s;
        if (c >= 0xD800 && c < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00); ++s; }
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}

uint32_t systemLanguage()
{
    UCHandle h = UCOpen();
    if (h < 0) return 1;
    alignas(64) static uint32_t value;
    alignas(64) static UCSysConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.name, "cafe.language");
    cfg.dataType = UC_DATATYPE_UNSIGNED_INT;
    cfg.dataSize = 4;
    cfg.data = &value;
    const UCError e = UCReadSysConfig(h, 1, &cfg);
    UCClose(h);
    return e == UC_ERROR_OK && value <= 11 ? value : 1;
}

nn::swkbd::RegionType systemRegion()
{
    alignas(64) static MCPSysProdSettings s;
    int h = MCP_Open();
    if (h < 0) return nn::swkbd::RegionType::Europe;
    const bool ok = MCP_GetSysProdSettings(h, &s) == 0;
    MCP_Close(h);
    if (!ok) return nn::swkbd::RegionType::Europe;
    if (s.product_area & MCP_REGION_JAPAN) return nn::swkbd::RegionType::Japan;
    if (s.product_area & MCP_REGION_USA) return nn::swkbd::RegionType::USA;
    if (s.product_area & MCP_REGION_CHINA) return nn::swkbd::RegionType::China;
    if (s.product_area & MCP_REGION_KOREA) return nn::swkbd::RegionType::Korea;
    if (s.product_area & MCP_REGION_TAIWAN) return nn::swkbd::RegionType::Taiwan;
    return nn::swkbd::RegionType::Europe;
}

bool create()
{
    if (s_created) return true;
    if (s_failed) return false;
    FSInit();
    s_fs = (FSClient*)memalign(0x40, sizeof(FSClient));
    if (!s_fs || FSAddClient(s_fs, FS_ERROR_FLAG_ALL) != FS_STATUS_OK) {
        SYS_Report("[SwkbdWiiU] no FS client\n");
        free(s_fs); s_fs = nullptr;
        s_failed = true;
        return false;
    }
    s_work = memalign(0x40, nn::swkbd::GetWorkMemorySize(0));
    nn::swkbd::CreateArg arg;
    arg.workMemory = s_work;
    arg.regionType = systemRegion();
    arg.fsClient = s_fs;
    if (!s_work || !nn::swkbd::Create(arg)) {
        SYS_Report("[SwkbdWiiU] the system's keyboard did not start: WiiFin's\n");
        FSDelClient(s_fs, FS_ERROR_FLAG_NONE);
        free(s_fs); s_fs = nullptr;
        free(s_work); s_work = nullptr;
        s_failed = true;
        return false;
    }
    s_created = true;
    SYS_Report("[SwkbdWiiU] the system's keyboard is ready\n");
    return true;
}

} // namespace

namespace SwkbdWiiU {

Result edit(std::string& text, const char* okLabel, bool password, size_t maxLen)
{
    s_lastAsked = GfxWiiU::frames();
    switch (s_st) {
    case St::Ok:
        text = toU8(s_result.c_str());
        if (text.size() > maxLen) text.resize(maxLen);
        s_st = St::Off;
        return Result::Ok;
    case St::Cancel:
        s_st = St::Off;
        return Result::Cancel;
    case St::Shown:
        return Result::Busy;
    case St::Off:
        break;
    }
    /* a new one: once the last one is gone */
    if (s_created && nn::swkbd::GetStateInputForm() != nn::swkbd::State::Hidden) return Result::Busy;
    if (!create()) return Result::Unavailable;
    s_initial = toU16(text);
    s_ok = toU16(okLabel ? okLabel : "OK");
    nn::swkbd::AppearArg arg;
    arg.keyboardArg.configArg.languageType = (nn::swkbd::LanguageType)systemLanguage();
    arg.keyboardArg.configArg.controllerType = nn::swkbd::ControllerType::DrcGamepad;
    arg.keyboardArg.configArg.keyboardMode = nn::swkbd::KeyboardMode::Full;
    arg.keyboardArg.configArg.okString = s_ok.c_str();
    arg.keyboardArg.configArg.showWordSuggestions = false;
    arg.keyboardArg.configArg.disableNewLine = true;
    arg.inputFormArg.initialText = s_initial.c_str();
    arg.inputFormArg.maxTextLength = (int32_t)(maxLen > 0 && maxLen < 1000 ? maxLen : -1);
    arg.inputFormArg.passwordMode = password ? nn::swkbd::PasswordMode::Fade : nn::swkbd::PasswordMode::Clear;
    if (!nn::swkbd::AppearInputForm(arg)) {
        SYS_Report("[SwkbdWiiU] the keyboard did not appear\n");
        return Result::Unavailable;
    }
    s_st = St::Shown;
    return Result::Busy;
}

bool shown()
{
    return s_created && (s_st == St::Shown || nn::swkbd::GetStateInputForm() != nn::swkbd::State::Hidden);
}

void calc()
{
    if (!s_created) return;
    nn::swkbd::ControllerInfo info;
    info.vpad = (VPADStatus*)InputWiiU::rawVpad();
    for (int i = 0; i < 4; ++i) info.kpad[i] = (KPADStatus*)InputWiiU::rawKpad(i);
    nn::swkbd::Calc(info);
    if (nn::swkbd::IsNeedCalcSubThreadFont()) nn::swkbd::CalcSubThreadFont();
    if (nn::swkbd::IsNeedCalcSubThreadPredict()) nn::swkbd::CalcSubThreadPredict();
    if (s_st != St::Shown) return;
    if (nn::swkbd::IsDecideOkButton(nullptr)) {
        const char16_t* s = nn::swkbd::GetInputFormString();
        s_result = s ? s : u"";
        nn::swkbd::DisappearInputForm();
        s_st = St::Ok;
    } else if (nn::swkbd::IsDecideCancelButton(nullptr)) {
        nn::swkbd::DisappearInputForm();
        s_st = St::Cancel;
    } else if (GfxWiiU::frames() - s_lastAsked > 3) {
        /* the field closed by WiiFin meanwhile */
        nn::swkbd::DisappearInputForm();
        s_st = St::Off;
    }
}

/* drawn with no context state of ours (as SDL2 does: the keyboard sets its
 * own state, which would be kept in WHBGfx's otherwise); the next screen's
 * pass sets its own again */
void drawTV()
{
    if (!s_created || nn::swkbd::GetStateInputForm() == nn::swkbd::State::Hidden) return;
    GX2SetContextState(nullptr);
    nn::swkbd::DrawTV();
}

void drawDRC()
{
    if (!s_created || nn::swkbd::GetStateInputForm() == nn::swkbd::State::Hidden) return;
    GX2SetContextState(nullptr);
    nn::swkbd::DrawDRC();
}

}
