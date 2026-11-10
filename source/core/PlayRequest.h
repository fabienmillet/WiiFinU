#pragma once
/* -----------------------------------------------------------------------
 * PlayRequest — what a browsing screen hands to the player when Play is
 * chosen (runPlaySession, MusicPlayerView): the stream, the item, its
 * tracks, the episodes around it.  LibraryView is one (the Wii's
 * screens); the 3DS's own screens are another.
 * ----------------------------------------------------------------------- */
#include <string>
#include <vector>
#include "../jellyfin/JellyfinClient.h"
#include "../ui/MusicPlayerView.h"

struct PlayRequest {
    std::string pendingPlayUrl;           // set before returning true when the user hits Play
    std::string pendingPlayTitle;         // shown in the player's top bar
    std::string pendingPlayItemId;        // Jellyfin item id for /Sessions/Playing reporting
    std::string pendingPlayMediaSourceId; // MediaSourceId (same as itemId for transcoded streams)
    std::string pendingPlaySessionId;     // PlaySessionId from PlaybackInfo (for progress reporting)
    long long   pendingPlayStartTimeTicks = 0; // seek offset passed to the transcoder (100-ns ticks)
    long long   pendingPlayRuntimeTicks  = 0; // RunTimeTicks from Jellyfin (for progress bar duration)
    JellyfinClient::PlaybackChoice pendingPlayHow; // direct play or transcode

    // Music play request (set when returning true from a music library)
    bool                              pendingPlayIsMusic  = false;
    std::vector<MusicTrack>  pendingMusicTracks;
    int                               pendingMusicTrackIdx = 0;
    bool                              pendingMusicShuffle  = false;  // the player starts shuffled

    // Episode list context for the player overlay (next/prev navigation)
    // Non-empty only when playing a TV-show episode.
    std::vector<JellyfinEpisode> pendingPlayEpisodes;   // all episodes in current season
    int                          pendingPlayEpisodeIdx; // index of current episode in the list
    std::string                  pendingPlaySeriesId;   // for re-fetching URLs on track change

    // Media stream lists (for in-player audio/subtitle switching)
    std::vector<MediaStream> pendingPlayAudioStreams;
    std::vector<MediaStream> pendingPlaySubStreams;
    int pendingPlayAudioIdx = 0;   // currently-selected audio stream index (from API)
    int pendingPlaySubIdx   = -1;  // currently-selected sub stream index (-1 = none)
};
