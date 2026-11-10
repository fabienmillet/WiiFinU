#pragma once
/* -----------------------------------------------------------------------
 * Images3ds — posters, thumbnails and backdrops for the 3DS's screens,
 * fetched by a thread of their own (the last asked first: what is on the
 * screen), kept while used, the oldest dropped past a budget.
 * ----------------------------------------------------------------------- */
#include <grrlib.h>
#include <string>
#include "../../../jellyfin/JellyfinClient.h"

namespace Images3ds {

enum class Kind { Poster, Thumb, Backdrop };

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
