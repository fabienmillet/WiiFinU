#pragma once
/* -----------------------------------------------------------------------
 * ImagesWiiU — posters, thumbnails and backdrops for the Wii U's browsing
 * screens, in HD (a backdrop at the TV's own size), fetched by threads of
 * their own (the last asked first: what is on the screen), kept while
 * used, the oldest dropped past a budget.
 * ----------------------------------------------------------------------- */
#include <grrlib.h>
#include <string>
#include "../../../jellyfin/JellyfinClient.h"

namespace ImagesWiiU {

/* Poster: the item's picture (a cover: square) for tiles; Thumb: 16:9
 * (a library's, an episode's still); Backdrop: the TV's background;
 * Large: the poster or cover big on the TV */
enum class Kind { Poster, Thumb, Backdrop, Large };

void start(JellyfinClient& client, const JellyfinAuth& auth, const std::string& serverUrl);
void stop();   /* the thread stopped, every texture freed */

/* The picture of item (kind), nullptr while it loads (or has none).  Each
 * call marks it used this frame. */
GRRLIB_texImg* get(const JellyfinItem& item, Kind kind);
inline GRRLIB_texImg* get(const std::string& id, Kind kind)
{ JellyfinItem it; it.id = id; return get(it, kind); }

/* Once a frame: drops what wasn't used for a while past the budget */
void endFrame();

}
