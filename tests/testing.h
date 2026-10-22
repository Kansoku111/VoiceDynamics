#pragma once
// Tiny test reporter and signal helpers shared by the test programs.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace t {
inline int failures = 0, checks = 0;

inline void check(bool ok, const std::string& label, const std::string& detail = {}) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", label.c_str(), detail.empty() ? "" : " : ", detail.c_str());
    std::fflush(stdout);
}
inline std::string num(double v, int digits = 4) {
    char s[64];
    std::snprintf(s, sizeof(s), "%.*f", digits, v);
    return s;
}
// |value - expected| <= tolerance
inline void near(const std::string& label, double value, double expected, double tolerance, const char* unit = "") {
    check(std::isfinite(value) && std::abs(value - expected) <= tolerance, label,
          num(value) + unit + " (expected " + num(expected) + " +/- " + num(tolerance) + ")");
}
inline void atMost(const std::string& label, double value, double limit, const char* unit = "") {
    check(std::isfinite(value) && value <= limit, label, num(value) + unit + " (limit " + num(limit) + ")");
}
inline void atLeast(const std::string& label, double value, double limit, const char* unit = "") {
    check(std::isfinite(value) && value >= limit, label, num(value) + unit + " (minimum " + num(limit) + ")");
}
inline int finish(const char* name) {
    std::printf("%s: %d checks, %d failed\n", name, checks, failures);
    return failures ? 1 : 0;
}

constexpr double Pi = 3.14159265358979323846;
inline double db(double lin) { return lin > 1e-12 ? 20.0 * std::log10(lin) : -240.0; }
inline double lin(double dB) { return std::pow(10.0, dB / 20.0); }

// Interleaved test signal builder. Tones are continuous in phase across calls.
struct Signal {
    double fs;
    int channels;
    std::vector<float> data;
    double phase = 0;
    Signal(double rate, int ch) : fs(rate), channels(ch) {}
    size_t frames() const { return data.size() / size_t(channels); }
    void silence(double seconds) { data.resize(data.size() + size_t(std::llround(seconds * fs)) * size_t(channels), 0.f); }
    // Sine with the given peak level (dBFS) in every channel (in phase).
    void tone(double hz, double peakDb, double seconds, double startPhase = -1, double fadeSeconds = 0) {
        if (startPhase >= 0) phase = startPhase;
        const size_t n = size_t(std::llround(seconds * fs));
        const double a = lin(peakDb), step = 2 * Pi * hz / fs;
        for (size_t i = 0; i < n; ++i) {
            double env = 1;
            if (fadeSeconds > 0) {
                const double t = double(i) / fs, tail = double(n - 1 - i) / fs;
                env = std::min({1.0, t / fadeSeconds, tail / fadeSeconds});
            }
            const float v = float(a * env * std::sin(phase));
            for (int c = 0; c < channels; ++c) data.push_back(v);
            phase += step;
        }
        phase = std::fmod(phase, 2 * Pi);
    }
};
}  // namespace t
