#pragma once
// Gate, slow leveler and compressor. Per-sample processing, no allocation after prepare().
// Level readings are RMS in dB calibrated so that a sine wave of peak 0 dBFS reads 0 dB.
#include "dsp_util.h"
#include "parameters.h"
#include <array>
#include <vector>

namespace vd {
constexpr double RmsCalibrationDb = 3.0102999566398120;  // 10 log10(2): sine peak -> RMS

// Hysteresis gate with Hold, Attack/Release smoothing and a floor (Range). The audio is delayed by the
// look-ahead (3 ms) while the detector looks at the undelayed input, so the gate opens before a word starts.
class Gate {
public:
    static constexpr double LookaheadMs = 3.0;
    void prepare(double fs, int channels);
    void reset() noexcept;
    void set(const Settings& s) noexcept;
    int latency() const noexcept { return delay_; }
    void process(const double* x, double* y) noexcept;
    bool isOpen() const noexcept { return open_; }
    double gain() const noexcept { return gain_; }  // linear, 1 = fully open

private:
    void update() noexcept;
    double fs_ = 48000;
    int channels_ = 2, delay_ = 144, pos_ = 0;
    std::vector<double> ring_;  // channels * delay
    std::array<Biquad, 2> hpf_{};
    std::array<double, 2> power_{};
    double powerCoef_ = 0, attack_ = 0, release_ = 0, openPower_ = 0, closePower_ = 0, closedGain_ = 0.25, gain_ = 1;
    int holdSamples_ = 0, holdCount_ = 0;
    bool open_ = true, on_ = true, hpfOn_ = true;
    Settings s_{};
    bool haveSettings_ = false;
};

// Slow gain rider around a reference level (disabled by default). Frozen while the gate is closed.
class Leveler {
public:
    static constexpr double LimitDb = 6.0, AttackMs = 800, ReleaseMs = 2000;
    void prepare(double fs, int channels);
    void reset() noexcept;
    void set(const Settings& s) noexcept;
    void process(const double* x, double* y, bool freeze) noexcept;
    double gainDb() const noexcept { return gainDb_; }

private:
    double fs_ = 48000;
    int channels_ = 2;
    double power_ = 0, powerCoef_ = 0, attack_ = 0, release_ = 0, fast_ = 0, gainDb_ = 0, target_ = -24, slope_ = -1.0 / 3;
    bool on_ = false;
};

// Feed-forward compressor: dB-domain soft knee, branching attack/release smoothing, stereo max link.
class Compressor {
public:
    void prepare(double fs, int channels);
    void reset() noexcept;
    void set(const Settings& s) noexcept;
    void process(const double* x, double* y) noexcept;
    double grDb() const noexcept { return gainDb_; }  // <= 0

private:
    double fs_ = 48000;
    int channels_ = 2;
    std::array<double, 2> power_{};
    double powerCoef_ = 0, attack_ = 0, release_ = 0, threshold_ = -24, ratio_ = 3, knee_ = 6, gainDb_ = 0;
    bool on_ = true;
};

// Gate -> leveler -> compressor. The part of the chain that does not depend on the correction gain, so
// the analysis can run it once and keep the result.
class PreStage {
public:
    void prepare(double fs, int channels);
    void reset() noexcept;
    void set(const Settings& s) noexcept;
    int latency() const noexcept { return gate_.latency(); }
    void process(const double* x, double* y) noexcept;
    const Gate& gate() const noexcept { return gate_; }
    double levelerDb() const noexcept { return leveler_.gainDb(); }
    double compressorDb() const noexcept { return comp_.grDb(); }

private:
    Gate gate_;
    Leveler leveler_;
    Compressor comp_;
};
}  // namespace vd
