#pragma once
/* -----------------------------------------------------------------------
 * PlatformWiiU — the Wii U's start, its two screens, the GamePad's remote.
 *
 * Screens (Settings > Screens, saved as wiiu_screens):
 *   Auto      a TV plugged in (HDMI): WiiFin on the TV, the GamePad a
 *             remote control; none: WiiFin on the GamePad
 *   TV        WiiFin on the TV, the GamePad a remote control
 *   GamePad   WiiFin on the GamePad (the TV shows the same)
 * L3 + R3 (both sticks pressed) swaps them at any time, and the remote
 * has a button to bring WiiFin to the GamePad.
 * ----------------------------------------------------------------------- */
namespace PlatformWiiU {

/* First thing in main(): ProcUI, the GPU, both screens */
void init();

enum class Screens { Auto = 0, TV = 1, GamePad = 2 };
void     setScreens(Screens s);
Screens  screens();
/* a TV seems plugged in (HDMI) */
bool     tvPresent();

/* What the remote shows as playing (the player sets it, "" none) */
void setNowPlaying(const char* title, const char* subtitle);

}
