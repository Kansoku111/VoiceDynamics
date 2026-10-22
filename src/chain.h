#pragma once
// The whole signal path: gate -> leveler -> compressor -> correction gain -> True Peak limiter, with a
// delayed dry path for bypass. One object per plug-in instance; the analysis builds the same stages.
#include "dynamics.h"
#include "limiter.h"
#include <vector>

namespace vd {
// What the editor shows between two meter publications (reset by the caller).
struct ChainStats {
    double gateGainMin = 1;       // linear
    double levelerDbMin = 0, levelerDbMax = 0;
    double compDbMin = 0;         // <= 0
    double limiterDbMin = 0;      // <= 0
    double inputPeak = 0, outputPeak = 0;  // sample peaks, linear
    void clear() noexcept { *this = ChainStats{}; }
};

class Chain {
public:
    void prepare(double fs, int channels);
    void reset(bool primeGain = true) noexcept;
    // totalGainDb = the automatic gain when enabled, otherwise the manual gain; passed in by the caller.
    void set(const Settings& s, double autoGainDb) noexcept;
    int latency() const noexcept { return pre_.latency() + limiter_.latency(); }
    int channels() const noexcept { return channels_; }
    double sampleRate() const noexcept { return fs_; }
    // pre (optional) receives the signal after the compressor, before the correction gain (what the learning captures).
    void tick(const double* in, double* out, double* pre = nullptr) noexcept;
    ChainStats& stats() noexcept { return stats_; }
    const PreStage& pre() const noexcept { return pre_; }
    double gainDb() const noexcept { return gainTargetDb_; }

private:
    double fs_ = 48000;
    int channels_ = 2, dryPos_ = 0, dryLength_ = 1;
    PreStage pre_;
    Limiter limiter_;
    std::vector<double> dry_;
    double gainCurrent_ = 1, gainTarget_ = 1, gainTargetDb_ = 0, gainCoef_ = 0, bypassMix_ = 0, bypassTarget_ = 0, bypassCoef_ = 0;
    ChainStats stats_;
};

// Total correction gain of a settings/auto-gain pair (shared by the plug-in and the analysis).
// Automatic on: the learned gain alone (the manual value would push the output past the target). Off: the manual value.
inline double totalGainDb(const Settings& s, double autoGainDb) noexcept { return s.autoOn ? autoGainDb : s.offsetDb; }
}  // namespace vd
