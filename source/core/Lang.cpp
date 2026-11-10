/* The interface's language: see Lang.h.  Each language is a gettext .mo
 * catalogue (msgfmt --endianness=big) built into the DOL; tr() looks a
 * string up in it by a binary search, msgfmt having sorted the originals. */
#include "Lang.h"
#include <ogc/conf.h>
#include <string.h>
#include <stdio.h>

extern "C" {
struct LangMo { const char* code; const unsigned char* data; unsigned size; };
extern const LangMo g_lang_mo[];    /* build/lang_mo.c, from lang/<code>.po */
extern const int    g_lang_mo_count;
}

namespace {

/* the names the languages give themselves */
const struct { const char* code; const char* name; } NAMES[] = {
    { "en", "English" },        { "fr", "Fran\xc3\xa7" "ais" }, { "de", "Deutsch" },
    { "es", "Espa\xc3\xb1ol" }, { "it", "Italiano" },           { "nl", "Nederlands" },
    { "pt", "Portugu\xc3\xaas" }, { "pt_BR", "Portugu\xc3\xaas (Brasil)" },
    { "ja", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" },             /* 日本語 */
    { "ko", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4" },             /* 한국어 */
    { "zh_CN", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87" }, /* 简体中文 */
    { "zh_TW", "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87" }, /* 繁體中文 */
    { "pl", "Polski" },         { "ru", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9" },
};

int s_choice  = -1;   /* -1: the Wii's language */
int s_current = 0;
bool s_hangul = false, s_han = false;   /* the SD card's fonts/ draws them */

/* the fonts draw it: Japanese is built in, Korean and Chinese come from
 * the SD card's fonts/ */
bool drawable(int i) {
    const char* c = Lang::code(i);
    if (strcmp(c, "ko") == 0) return s_hangul;
    if (strncmp(c, "zh", 2) == 0) return s_han;
    return true;
}

unsigned be32(const unsigned char* p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

/* the Wii's language, as one of ours (English when there is none) */
int wiiLanguage() {
    const char* want = "en";
    switch (CONF_GetLanguage()) {
        case CONF_LANG_JAPANESE:     want = "ja";    break;
        case CONF_LANG_GERMAN:       want = "de";    break;
        case CONF_LANG_FRENCH:       want = "fr";    break;
        case CONF_LANG_SPANISH:      want = "es";    break;
        case CONF_LANG_ITALIAN:      want = "it";    break;
        case CONF_LANG_DUTCH:        want = "nl";    break;
        case CONF_LANG_SIMP_CHINESE: want = "zh_CN"; break;
        case CONF_LANG_TRAD_CHINESE: want = "zh_TW"; break;
        case CONF_LANG_KOREAN:       want = "ko";    break;
        default: break;
    }
    for (int i = 1; i < Lang::count(); i++)
        if (strcmp(Lang::code(i), want) == 0) return drawable(i) ? i : 0;
    return 0;
}

void update() {
    s_current = s_choice >= 0 && s_choice < Lang::count() && drawable(s_choice) ? s_choice : wiiLanguage();
}

} // namespace

#ifdef __3DS__
/* The 3DS says "the console" where the Wii's interface says "the Wii": its
 * own English, translated in turn (the lang folder) */
static const char* forThe3ds(const char* en)
{
    static const struct { const char* wii; const char* ds; } MAP[] = {
        { "Anyone who opens WiiFin can then open this profile",
          "Anyone who opens the app can then open this profile" },
        { "Chosen here, whatever the Wii's language",
          "Chosen here, whatever the console's language" },
        { "Light, like the Wii Menu",
          "Light and bright" },
        { "Opens at once on this Wii",
          "Opens at once on this console" },
        { "Or Quick Connect: this profile keeps no sign-in on the Wii",
          "Or Quick Connect: this profile keeps no sign-in on the console" },
        { "Sign in automatically on this Wii?",
          "Sign in automatically on this console?" },
        { "Soft gradients, like the Wii Menu",
          "Soft gradients" },
        { "The video comes in, but the Wii can't show it: the picture stays stuck.",
          "The video comes in, but the console can't show it: the picture stays stuck." },
        { "The Wii's language, as set in the Wii Settings",
          "The console's language, as set in its System Settings" },
        { "Files the Wii can decode play as they are (SD DivX, DVD...): no server work",
          "Small files the console can decode play as they are: no server work" },
        { "Too heavy for the Wii: transcoding...",
          "Too heavy for the console: transcoding..." },
        { "Check the Wii's Internet settings, then try again.",
          "Check the console's Internet settings, then try again." },
        { "The Wii must be on the same network as the server.",
          "The console must be on the same network as the server." },
        { "This is taking long: the Wii's network may need a restart.",
          "This is taking long: the console's network may need a restart." },
        { "Red and charcoal, browse in rows like the old video channels",
          "Red and charcoal" },
        { "Wii Menu",
          "Exit" },
        { "WiiFin Menu",
          "Menu" },
    };
    for (const auto& m : MAP)
        if (strcmp(en, m.wii) == 0) return m.ds;
    return en;
}
#endif

const char* tr(const char* en)
{
#ifdef __3DS__
    if (en && *en) en = forThe3ds(en);
#endif
    if (s_current <= 0 || !en || !*en) return en;
    const LangMo& mo = g_lang_mo[s_current - 1];
    const unsigned char* d = mo.data;
    if (mo.size < 28 || be32(d) != 0x950412DEu) return en;
    unsigned n = be32(d + 8), orig = be32(d + 12), trans = be32(d + 16);
    if (orig + n * 8 > mo.size || trans + n * 8 > mo.size) return en;
    int lo = 0, hi = (int)n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned len = be32(d + orig + mid * 8), off = be32(d + orig + mid * 8 + 4);
        if (off + len >= mo.size) return en;
        int c = strcmp(en, (const char*)d + off);
        if (c == 0) {
            unsigned toff = be32(d + trans + mid * 8 + 4), tlen = be32(d + trans + mid * 8);
            return toff + tlen < mo.size && tlen ? (const char*)d + toff : en;
        }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return en;
}

namespace Lang {

int count() { return 1 + g_lang_mo_count; }

const char* code(int i) { return i <= 0 || i >= count() ? "en" : g_lang_mo[i - 1].code; }

const char* name(int i) {
    const char* c = code(i);
    for (const auto& n : NAMES)
        if (strcmp(n.code, c) == 0) return n.name;
    return c;
}

void choose(int i) { s_choice = i >= 0 && i < count() ? i : -1; update(); }

void setFonts(bool hangul, bool han) { s_hangul = hangul; s_han = han; update(); }

int next(int i) {
    for (int k = i + 1; k < count(); ++k) if (drawable(k)) return k;
    return -1;
}
int  choice()      { return s_choice; }
int  current()     { update(); return s_current; }

const char* choiceKey() { return s_choice < 0 ? "wii" : code(s_choice); }

void chooseKey(const char* key) {
    for (int i = 0; i < count(); i++)
        if (strcmp(code(i), key) == 0) { choose(i); return; }
    choose(-1);
}

} // namespace Lang

std::string Lang::runTime(int minutes)
{
    char buf[48] = "";
    /* TRANSLATORS: a running time, hours and minutes (no zero before the minutes) */
    if (minutes >= 60)    snprintf(buf, sizeof(buf), tr("%dh %dmin"), minutes / 60, minutes % 60);
    else if (minutes > 0) snprintf(buf, sizeof(buf), tr("%dmin"), minutes);
    return buf;
}
