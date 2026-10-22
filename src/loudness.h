#pragma once
// ITU-R BS.1770-5 loudness (K-weighting, 400 ms blocks with 75 % overlap, absolute gate -70 LUFS,
// relative gate -10 LU) with EBU Tech 3341/3342 style Momentary, Short-term, Max and LRA values,
// and the oversampled True Peak detector of BS.1770-5 Annex 2.
//
// Nothing here allocates or locks after prepare(); every method named push() may run on the audio thread.
// The coefficient formulas follow libebur128 (MIT, Copyright (c) 2011 Jan Kokemuller); no code was copied.
#include "dsp_util.h"
#include <array>
#include <cstdint>
#include <vector>

namespace vd {
constexpr double LoudnessOffset = -0.691;  // BS.1770 constant
constexpr double AbsoluteGate = -70.0;

inline double blockLoudness(double power) noexcept { return power > 0 ? LoudnessOffset + 10.0 * std::log10(power) : SilenceDb; }

// Exact gated integrated loudness of a list of 400 ms block powers (mean square, K-weighted, channel
// weighted and summed), each scaled by gainDb. SilenceDb when no block passes the absolute gate.
// This is the offline reference; the streaming meter below quantizes blocks to 0.01 LU.
double gatedIntegrated(const std::vector<double>& blockPowers, double gainDb = 0, size_t* used = nullptr);

class LoudnessMeter {
public:
    static constexpr int SegmentsPerHop = 10;   // 10 ms segments, a gating block every 10 segments
    static constexpr int MomentarySegments = 40, ShortTermSegments = 300;
    static constexpr double BinStep = 0.01, BinLow = -70.0, BinHigh = 20.0;
    static constexpr int Bins = int((BinHigh - BinLow) / BinStep);

    // channels 1 or 2. dualMono counts a single channel twice (+3.01 dB), as for a mono file played on both speakers.
    void prepare(double fs, int channels, bool dualMono);
    void reset() noexcept;
    void push(const double* frame) noexcept;

    double momentary() const noexcept { return momentary_; }
    double shortTerm() const noexcept { return shortTerm_; }
    double maxMomentary() const noexcept { return maxMomentary_; }
    double maxShortTerm() const noexcept { return maxShortTerm_; }
    bool hasIntegrated() const noexcept { return hasIntegrated_; }
    double integrated() const noexcept { return integrated_; }  // SilenceDb until a block passes the gates
    bool hasLra() const noexcept { return hasLra_; }
    double lra() const noexcept { return lra_; }
    double seconds() const noexcept { return double(segments_) * segLen_ / fs_; }
    // Offline use only: also collect every 400 ms block power (allocates when the vector grows).
    void setBlockSink(std::vector<double>* sink) noexcept { sink_ = sink; }
    double sampleRate() const noexcept { return fs_; }

private:
    struct Histogram {
        std::vector<uint32_t> count;
        std::vector<double> sum;
        uint64_t total = 0;
        double energy = 0;
        void prepare() {
            count.assign(Bins, 0);
            sum.assign(Bins, 0);
        }
        void clear() noexcept;
        void add(double loudness, double power) noexcept;
    };
    void finishSegment() noexcept;
    double gatedMean(const Histogram& h, double relative, uint64_t* count, double* low, double* high) const noexcept;

    double fs_ = 48000, weight_ = 1;
    int channels_ = 2, segLen_ = 480, inSegment_ = 0;
    std::array<Biquad, 2> shelf_{}, high_{};
    double acc_ = 0;
    std::array<double, ShortTermSegments> segs_{};
    uint64_t segments_ = 0;
    Histogram blocks_, shortTerms_;
    double momentary_ = SilenceDb, shortTerm_ = SilenceDb, maxMomentary_ = SilenceDb, maxShortTerm_ = SilenceDb, integrated_ = SilenceDb, lra_ = 0;
    bool hasIntegrated_ = false, hasLra_ = false;
    std::vector<double>* sink_ = nullptr;
};

// Linear-phase Kaiser-windowed-sinc polyphase interpolator that reports the largest interpolated
// magnitude (the true peak) over each sample interval.
class TruePeakDetector {
public:
    static constexpr int Taps = 64;  // taps per phase; the detector looks Taps/2 samples ahead
    static int factorFor(double fs) noexcept { return fs <= 50000 ? 16 : fs <= 100000 ? 8 : fs <= 200000 ? 4 : 2; }

    void prepare(double fs, int channels);
    void reset() noexcept;
    int factor() const noexcept { return factor_; }
    int delay() const noexcept { return Taps / 2; }
    // Upper bound of |interpolated value| per unit of the largest |sample| in the filter span.
    double lambda() const noexcept { return lambda_; }
    // Pushes one frame. Returns the exact maximum magnitude over the interval that ended delay() samples
    // ago when that exceeds skipBelow; otherwise some value not above skipBelow (the filter is skipped).
    double push(const double* frame, double skipBelow = 0) noexcept;

private:
    int channels_ = 2, factor_ = 8, head_ = 0;
    double lambda_ = 2;
    std::vector<double> coef_;                      // (factor-1) phases x Taps
    std::array<std::vector<double>, 2> history_;    // 2 x Taps per channel (mirrored ring)
    std::array<double, Taps> magnitude_{};
    int magnitudeHead_ = 0;
};
}  // namespace vd
