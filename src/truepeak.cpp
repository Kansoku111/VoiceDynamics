#include "loudness.h"
#include <algorithm>
#include <cmath>

namespace vd {
namespace {
double besselI0(double x) noexcept {
    double sum = 1, term = 1;
    const double q = x * x / 4.0;
    for (int k = 1; k < 60; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-18) break;
    }
    return sum;
}
}  // namespace

void TruePeakDetector::prepare(double fs, int channels) {
    channels_ = std::clamp(channels, 1, 2);
    factor_ = factorFor(fs);
    const int phases = std::max(0, factor_ - 1);
    coef_.assign(size_t(std::max(1, phases)) * Taps, 0.0);
    constexpr double beta = 9.0, half = Taps / 2.0;
    const double norm = besselI0(beta);
    lambda_ = 1;
    for (int p = 1; p < factor_; ++p) {
        double sum = 0;
        double* h = &coef_[size_t(p - 1) * Taps];
        for (int m = 0; m < Taps; ++m) {
            const double t = double(p) / factor_ - double(m - Taps / 2 + 1);
            const double sinc = std::abs(t) < 1e-12 ? 1.0 : std::sin(Pi * t) / (Pi * t);
            const double r = std::clamp(t / half, -1.0, 1.0);
            h[m] = sinc * besselI0(beta * std::sqrt(1.0 - r * r)) / norm;
            sum += h[m];
        }
        double absSum = 0;
        for (int m = 0; m < Taps; ++m) {
            h[m] /= sum;  // unity gain at DC for every phase
            absSum += std::abs(h[m]);
        }
        lambda_ = std::max(lambda_, absSum);
    }
    for (auto& h : history_) h.assign(size_t(2 * Taps), 0.0);
    reset();
}

void TruePeakDetector::reset() noexcept {
    for (auto& h : history_) std::fill(h.begin(), h.end(), 0.0);
    magnitude_.fill(0);
    head_ = 0;
    magnitudeHead_ = 0;
}

double TruePeakDetector::push(const double* frame, double skipBelow) noexcept {
    double mag = 0;
    for (int c = 0; c < channels_; ++c) {
        auto& h = history_[size_t(c)];
        h[size_t(head_)] = h[size_t(head_ + Taps)] = frame[c];
        mag = std::max(mag, std::abs(frame[c]));
    }
    head_ = (head_ + 1) % Taps;
    magnitude_[size_t(magnitudeHead_)] = mag;
    magnitudeHead_ = (magnitudeHead_ + 1) % Taps;
    double span = 0;
    for (double v : magnitude_) span = std::max(span, v);
    // The interval that just became fully known starts at the sample Taps/2 - 1 steps before the newest.
    double peak = 0;
    for (int c = 0; c < channels_; ++c) peak = std::max(peak, std::abs(history_[size_t(c)][size_t(head_ + Taps / 2 - 1)]));
    if (factor_ <= 1 || span * lambda_ <= skipBelow) return peak;
    for (int c = 0; c < channels_; ++c) {
        const double* w = &history_[size_t(c)][size_t(head_)];
        for (int p = 1; p < factor_; ++p) {
            const double* h = &coef_[size_t(p - 1) * Taps];
            double acc = 0;
            for (int m = 0; m < Taps; ++m) acc += h[m] * w[m];
            peak = std::max(peak, std::abs(acc));
        }
    }
    return peak;
}
}  // namespace vd
