#pragma once
// Independent true-peak reference for the tests: 16x interpolation with a long Kaiser-windowed sinc
// (128 taps per phase). Deliberately a different filter from the detector under test.
#include <algorithm>
#include <cmath>
#include <vector>

namespace ref {
inline double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 80; ++k) {
        term *= (x / 2) * (x / 2) / (double(k) * k);
        sum += term;
        if (term < sum * 1e-18) break;
    }
    return sum;
}

class TruePeak16 {
public:
    static constexpr int Factor = 16, Half = 64;
    TruePeak16() : table_(size_t(Factor) * 2 * Half) {
        const double pi = 3.14159265358979323846, beta = 12.0, norm = besselI0(beta);
        for (int p = 1; p < Factor; ++p) {
            double sum = 0;
            for (int k = -Half + 1; k <= Half; ++k) {
                const double t = double(p) / Factor - k, sinc = std::sin(pi * t) / (pi * t), r = std::clamp(t / Half, -1.0, 1.0);
                const double v = sinc * besselI0(beta * std::sqrt(1 - r * r)) / norm;
                table_[size_t(p) * 2 * Half + size_t(k + Half - 1)] = v;
                sum += v;
            }
            double absSum = 0;
            for (int m = 0; m < 2 * Half; ++m) {
                table_[size_t(p) * 2 * Half + size_t(m)] /= sum;
                absSum += std::abs(table_[size_t(p) * 2 * Half + size_t(m)]);
            }
            lambda_ = std::max(lambda_, absSum);
        }
    }
    // Largest |value| of the interpolated signal (linear). x is one channel.
    double peak(const std::vector<double>& x) const {
        double best = 0;
        for (double v : x) best = std::max(best, std::abs(v));
        const long n = long(x.size());
        for (long i = 0; i < n; ++i) {
            // Skip intervals that cannot reach the current best (the interpolated value is at most lambda times the local maximum).
            double local = 0;
            for (long j = std::max(0L, i - Half + 1); j <= std::min(n - 1, i + Half); j += 1) local = std::max(local, std::abs(x[size_t(j)]));
            if (local * lambda_ <= best) continue;
            for (int p = 1; p < Factor; ++p) {
                const double* h = &table_[size_t(p) * 2 * Half];
                double acc = 0;
                for (int m = 0; m < 2 * Half; ++m) {
                    const long j = i + m - Half + 1;
                    if (j >= 0 && j < n) acc += h[m] * x[size_t(j)];
                }
                best = std::max(best, std::abs(acc));
            }
        }
        return best;
    }
    // Interleaved float, all channels.
    double peak(const std::vector<float>& interleaved, int channels) const {
        double best = 0;
        for (int c = 0; c < channels; ++c) {
            std::vector<double> x(interleaved.size() / size_t(channels));
            for (size_t i = 0; i < x.size(); ++i) x[i] = interleaved[i * size_t(channels) + size_t(c)];
            best = std::max(best, peak(x));
        }
        return best;
    }

private:
    std::vector<double> table_;
    double lambda_ = 1;
};
inline double db(double lin) { return lin > 1e-12 ? 20.0 * std::log10(lin) : -240.0; }
}  // namespace ref
