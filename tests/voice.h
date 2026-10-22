#pragma once
// Deterministic speech-like test material: voiced syllables (pulse train through three formant
// resonators) with a wide level range, pauses with a noise floor, breaths, whispers and shouts.
#include "dsp_util.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace voice {
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    double next() {  // 0..1
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return double(s >> 11) / double(1ull << 53);
    }
    double range(double a, double b) { return a + (b - a) * next(); }
};

// Interleaved float, `channels` identical (or slightly different) channels. `loudDb` scales the whole piece.
inline std::vector<float> make(double fs, double seconds, int channels, uint64_t seed, double loudDb = 0) {
    Rng rng(seed);
    const size_t n = size_t(seconds * fs);
    std::vector<double> mono(n, 0.0);
    vd::Biquad f1, f2, f3;
    auto resonator = [&](vd::Biquad& b, double hz, double q) {
        const double w = 2 * vd::Pi * hz / fs, alpha = std::sin(w) / (2 * q), cw = std::cos(w), a0 = 1 + alpha;
        b.b0 = alpha / a0;
        b.b1 = 0;
        b.b2 = -alpha / a0;
        b.a1 = -2 * cw / a0;
        b.a2 = (1 - alpha) / a0;
    };
    size_t pos = 0;
    double phase = 0, noiseState = 0;
    int index = 0;
    while (pos < n) {
        ++index;
        // 1 s of speech-like phrase: 3-6 syllables then a pause
        const int syllables = 3 + int(rng.next() * 4);
        for (int k = 0; k < syllables && pos < n; ++k) {
            double levelDb = rng.range(-30, -12);
            if (index % 9 == 4 && k == 1) levelDb = rng.range(-6, -2);        // a shout
            else if (index % 7 == 3) levelDb = rng.range(-42, -34);           // a whisper-like quiet phrase
            const double hz = rng.range(95, 210), dur = rng.range(0.12, 0.28);
            resonator(f1, rng.range(500, 800), 6);
            resonator(f2, rng.range(1000, 1800), 8);
            resonator(f3, rng.range(2300, 3000), 10);
            const size_t len = size_t(dur * fs);
            const double amp = std::pow(10.0, levelDb / 20) * 3.0;
            for (size_t i = 0; i < len && pos + i < n; ++i) {
                const double t = double(i) / double(len);
                const double env = std::min(1.0, t * 12) * std::pow(1 - t, 0.6);
                phase += hz / fs;
                if (phase >= 1) phase -= 1;
                const double pulse = (phase < 0.08 ? 1.0 : 0.0) - 0.08;
                const double y = f1.tick(pulse) + 0.7 * f2.tick(pulse) + 0.4 * f3.tick(pulse);
                mono[pos + i] += amp * env * y;
            }
            pos += len;
        }
        // pause with a breath-like noise floor
        const double gap = rng.range(0.15, 0.6);
        const size_t len = size_t(gap * fs);
        const double breath = (index % 5 == 0) ? rng.range(-50, -42) : -66.0;
        const double a = std::pow(10.0, breath / 20) * 1.7;
        for (size_t i = 0; i < len && pos + i < n; ++i) {
            noiseState = 0.97 * noiseState + 0.03 * (rng.next() * 2 - 1);
            mono[pos + i] += a * noiseState * 5;
        }
        pos += len;
    }
    const double g = std::pow(10.0, loudDb / 20);
    std::vector<float> out(n * size_t(channels));
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c) out[i * size_t(channels) + size_t(c)] = float(std::max(-1.0, std::min(1.0, mono[i] * g * (c == 0 ? 1.0 : 0.93))));
    return out;
}
}  // namespace voice
