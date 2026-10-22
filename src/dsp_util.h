#pragma once
// Small DSP helpers shared by every stage (no allocation, no locks).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vd {
constexpr double Pi = 3.14159265358979323846;
constexpr double SilenceDb = -200.0;  // what a level readout shows when there is nothing to measure

inline double dbToLin(double db) noexcept { return std::exp(db * 0.11512925464970229); }
inline double linToDb(double lin) noexcept { return lin > 1e-12 ? 20.0 * std::log10(lin) : -240.0; }
inline double powerToDb(double power) noexcept { return power > 1e-24 ? 10.0 * std::log10(power) : -240.0; }

// One-pole coefficient reaching 90 % of a step after `ms` (the convention of research/06).
inline double timeCoef(double ms, double fs) noexcept {
    const double samples = std::max(1.0, ms * 0.001 * fs);
    return std::exp(-2.1972245773362196 / samples);
}
// Plain one-pole smoothing with time constant `ms` (1/e).
inline double tauCoef(double ms, double fs) noexcept { return std::exp(-1.0 / std::max(1.0, ms * 0.001 * fs)); }

inline double sanitize(double x) noexcept {
    // NaN becomes silence; infinity and absurd values are limited so nothing downstream can run away.
    if (!(std::abs(x) <= 1e4)) return std::isnan(x) ? 0.0 : (x > 0 ? 1e4 : -1e4);
    return x;
}

// Transposed direct form II.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void reset() noexcept { z1 = z2 = 0; }
    double tick(double x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

inline Biquad butterworthHighpass(double f, double fs) noexcept {
    Biquad q;
    const double k = std::tan(Pi * f / fs), n = 1.0 / (1.0 + std::sqrt(2.0) * k + k * k);
    q.b0 = n;
    q.b1 = -2 * n;
    q.b2 = n;
    q.a1 = 2 * (k * k - 1) * n;
    q.a2 = (1 - std::sqrt(2.0) * k + k * k) * n;
    return q;
}

// ITU-R BS.1770 K-weighting for any sample rate (analogue prototype of libebur128, bilinear transform).
inline Biquad kShelf(double fs) noexcept {
    const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(Pi * f0 / fs), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    Biquad s;
    s.b0 = (vh + vb * k / q + k * k) / a0;
    s.b1 = 2.0 * (k * k - vh) / a0;
    s.b2 = (vh - vb * k / q + k * k) / a0;
    s.a1 = 2.0 * (k * k - 1.0) / a0;
    s.a2 = (1.0 - k / q + k * k) / a0;
    return s;
}
inline Biquad kHighpass(double fs) noexcept {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(Pi * f0 / fs), a0 = 1.0 + k / q + k * k;
    Biquad s;
    s.b0 = 1;
    s.b1 = -2;
    s.b2 = 1;
    s.a1 = 2.0 * (k * k - 1.0) / a0;
    s.a2 = (1.0 - k / q + k * k) / a0;
    return s;
}
}  // namespace vd
