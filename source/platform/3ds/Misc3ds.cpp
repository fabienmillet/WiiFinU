/* Wii-only pieces with nothing to do on the 3DS */
#include "../../core/Reload.h"
#include "../../player/AudioSpectrum.h"

/* WiiFin started again from its DOL: not on the 3DS (back to the loader) */
bool Reload::self(const char* path) { (void)path; return false; }

/* the music player's bars: from the DSP's output on the Wii; none here */
bool AudioSpectrum::analyse(float* bands, int n, float* bass)
{
    for (int i = 0; i < n; ++i) bands[i] = 0.0f;
    if (bass) *bass = 0.0f;
    return false;
}
void AudioSpectrum::reset() {}
