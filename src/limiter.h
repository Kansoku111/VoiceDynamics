#pragma once
// True Peak limiter. The input is checked with an oversampled detector (loudness.h), the largest allowed
// gain for every sample interval is computed, a sliding minimum over the look-ahead window and a moving
// average of equal length turn that into a smooth gain that never exceeds any value in its window, and
// only the recovery is slow. All state is allocated in prepare().
#include "dsp_util.h"
#include "loudness.h"
#include <vector>

namespace vd {
class Limiter {
public:
    static constexpr double LookaheadMs = 3.0;
    // The internal ceiling sits this far below the requested one: inter-sample peaks that the gain
    // slope between two samples can add, and the remaining under-read of the detector.
    static constexpr double GuardDb = 0.1;

    void prepare(double fs, int channels);
    void reset() noexcept;
    void set(bool on, double ceilingDb, double releaseMs) noexcept;
    // Samples between an input frame and the same frame at the output.
    int latency() const noexcept { return latency_; }
    void process(const double* x, double* y) noexcept;
    double grDb() const noexcept { return reductionDb_; }  // <= 0

private:
    double fs_ = 48000;
    int channels_ = 2, lookahead_ = 144, latency_ = 160, pos_ = 0, minHead_ = 0, minTail_ = 0, averagePos_ = 0;
    uint64_t index_ = 0;
    TruePeakDetector detector_;
    std::vector<double> delay_;                    // channels * latency
    std::vector<uint64_t> minIndex_;               // monotonic deque for the sliding minimum
    std::vector<double> minValue_, average_;
    double averageSum_ = 0, ceilingLin_ = 0.89, releaseMs_ = 80, reductionDb_ = 0;
    bool on_ = true;
};
}  // namespace vd
