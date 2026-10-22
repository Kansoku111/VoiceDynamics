#include "chain.h"
#include <algorithm>
#include <cmath>

namespace vd {
void Chain::prepare(double fs, int channels) {
    fs_ = fs;
    channels_ = std::clamp(channels, 1, 2);
    pre_.prepare(fs, channels_);
    limiter_.prepare(fs, channels_);
    dryLength_ = std::max(1, latency());
    dry_.assign(size_t(dryLength_) * 2, 0.0);
    gainCoef_ = tauCoef(15.0, fs);
    bypassCoef_ = tauCoef(10.0, fs);
    set(Settings{}, 0);
    reset();
}

void Chain::reset(bool primeGain) noexcept {
    pre_.reset();
    limiter_.reset();
    std::fill(dry_.begin(), dry_.end(), 0.0);
    dryPos_ = 0;
    if (primeGain) {
        gainCurrent_ = gainTarget_;
        bypassMix_ = bypassTarget_;
    }
    stats_.clear();
}

void Chain::set(const Settings& s, double autoGainDb) noexcept {
    pre_.set(s);
    limiter_.set(s.limiterOn, s.ceiling, s.limiterRelease);
    gainTargetDb_ = std::clamp(totalGainDb(s, autoGainDb), -80.0, 80.0);
    gainTarget_ = dbToLin(gainTargetDb_);
    bypassTarget_ = s.bypass ? 1.0 : 0.0;
}

void Chain::tick(const double* in, double* out, double* preOut) noexcept {
    double x[2]{0, 0}, a[2], b[2], c[2];
    for (int ch = 0; ch < channels_; ++ch) {
        x[ch] = sanitize(in[ch]);
        stats_.inputPeak = std::max(stats_.inputPeak, std::abs(x[ch]));
    }
    pre_.process(x, a);
    if (preOut)
        for (int ch = 0; ch < channels_; ++ch) preOut[ch] = a[ch];
    gainCurrent_ = gainTarget_ + gainCoef_ * (gainCurrent_ - gainTarget_);
    if (std::abs(gainCurrent_ - gainTarget_) < gainTarget_ * 1e-9) gainCurrent_ = gainTarget_;
    for (int ch = 0; ch < channels_; ++ch) a[ch] *= gainCurrent_;
    limiter_.process(a, b);
    bypassMix_ = bypassTarget_ + bypassCoef_ * (bypassMix_ - bypassTarget_);
    if (std::abs(bypassMix_ - bypassTarget_) < 1e-9) bypassMix_ = bypassTarget_;
    for (int ch = 0; ch < channels_; ++ch) {
        double* slot = &dry_[size_t(ch) * size_t(dryLength_) + size_t(dryPos_)];
        c[ch] = *slot;
        *slot = x[ch];
        out[ch] = bypassMix_ == 0 ? b[ch] : bypassMix_ == 1 ? c[ch] : b[ch] + bypassMix_ * (c[ch] - b[ch]);
        stats_.outputPeak = std::max(stats_.outputPeak, std::abs(out[ch]));
    }
    if (++dryPos_ >= dryLength_) dryPos_ = 0;
    stats_.gateGainMin = std::min(stats_.gateGainMin, pre_.gate().gain());
    stats_.levelerDbMin = std::min(stats_.levelerDbMin, pre_.levelerDb());
    stats_.levelerDbMax = std::max(stats_.levelerDbMax, pre_.levelerDb());
    stats_.compDbMin = std::min(stats_.compDbMin, pre_.compressorDb());
    stats_.limiterDbMin = std::min(stats_.limiterDbMin, limiter_.grDb());
}
}  // namespace vd
