/* Wii pieces done otherwise on the Wii U */
#include "../../core/Reload.h"
#include "../../player/AudioSpectrum.h"
#include "AudioWiiU.h"
#include <math.h>

/* WiiFin started again from its DOL: not on the Wii U */
bool Reload::self(const char* path) { (void)path; return false; }

/* The music player's bars: on the Wii from the DSP's output; here from
 * the mix's last samples (12 kHz, AudioWiiU::recent), a Goertzel filter
 * a band, the bands log-spaced from 60 Hz to 5 kHz */
bool AudioSpectrum::analyse(float* bands, int n, float* bass)
{
    const int N = 512;
    static float s[N];
    if (AudioWiiU::recent(s, N) < N) {
        for (int i = 0; i < n; ++i) bands[i] = 0.0f;
        if (bass) *bass = 0.0f;
        return false;
    }
    const float RATE = 12000.0f;
    float total = 0;
    for (int b = 0; b < n; ++b) {
        const float f = 60.0f * powf(5000.0f / 60.0f, n > 1 ? (float)b / (n - 1) : 0.0f);
        const float c = 2.0f * cosf(2.0f * (float)M_PI * f / RATE);
        float q1 = 0, q2 = 0;
        for (int i = 0; i < N; ++i) {
            /* a Hann window */
            const float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1));
            const float q0 = c * q1 - q2 + s[i] * w;
            q2 = q1;
            q1 = q0;
        }
        const float mag = sqrtf(q1 * q1 + q2 * q2 - c * q1 * q2) / (N * 0.25f);
        float v = mag > 0 ? 1.0f + log10f(mag) / 2.5f : 0.0f;   /* -50 dB..0 */
        bands[b] = v < 0 ? 0 : v > 1 ? 1 : v;
        total += bands[b];
    }
    if (bass) *bass = n >= 2 ? (bands[0] + bands[1]) * 0.5f : bands[0];
    return total > 0.0f;
}
void AudioSpectrum::reset() {}
