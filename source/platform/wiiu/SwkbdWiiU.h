#pragma once
/* -----------------------------------------------------------------------
 * SwkbdWiiU — the Wii U's own keyboard (nn::swkbd) for WiiFin's text
 * fields: Keyboard::update asks it (edit) each frame while a field is
 * typed in; it shows the field's text, the GamePad's keyboard under it
 * (on the TV too), and gives the text back on OK.  GfxWiiU draws it over
 * WiiFin's frame and InputWiiU gives it the controls (none to WiiFin
 * meanwhile).  When it can't start: WiiFin's own keyboard.
 * ----------------------------------------------------------------------- */
#include <string>

namespace SwkbdWiiU {

enum class Result { Busy, Ok, Cancel, Unavailable };

/* Called each frame while the field is typed in: the keyboard comes up
 * (with text), Ok puts what was typed into text */
Result edit(std::string& text, const char* okLabel, bool password, size_t maxLen);
/* it is up (or going away): WiiFin's controls wait */
bool shown();

/* at exit: the keyboard and its FS client gone */
void shutdown();

/* GfxWiiU, each frame: the keyboard's work, then its drawing in each
 * screen's pass */
void calc();
void drawTV();
void drawDRC();

}
