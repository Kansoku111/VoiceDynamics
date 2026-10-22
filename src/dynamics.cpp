#include "dynamics.h"
#include <algorithm>
#include <cmath>

namespace vd {
namespace {
bool sameGate(const Settings& a, const Settings& b) noexcept {
    return a.gateOn == b.gateOn && a.gateOpen == b.gateOpen && a.gateClose == b.gateClose && a.gateHold == b.gateHold && a.gateAttack == b.gateAttack &&
           a.gateRelease == b.gateRelease && a.gateRange == b.gateRange && a.gateHpf == b.gateHpf;
}
}  // namespace

// ---- Gate ------------------------------------------------------------------------------------------
void Gate::prepare(double fs, int channels) {
    fs_ = fs;
    channels_ = std::clamp(channels, 1, 2);
    delay_ = std::max(1, int(std::lround(LookaheadMs * 0.001 * fs)));
    ring_.assign(size_t(delay_) * 2, 0.0);
    powerCoef_ = 1.0 - tauCoef(10.0, fs);
    haveSettings_ = false;
    set(Settings{});
    reset();
}
void Gate::reset() noexcept {
    std::fill(ring_.begin(), ring_.end(), 0.0);
    for (auto& h : hpf_) h.reset();
    power_.fill(0);
    pos_ = 0;
    gain_ = 1;
    open_ = true;
    holdCount_ = 0;
}
void Gate::set(const Settings& s) noexcept {
    if (haveSettings_ && sameGate(s, s_)) return;
    s_ = s;
    haveSettings_ = true;
    update();
}
void Gate::update() noexcept {
    on_ = s_.gateOn;
    attack_ = timeCoef(s_.gateAttack, fs_);
    release_ = timeCoef(s_.gateRelease, fs_);
    holdSamples_ = int(std::lround(s_.gateHold * 0.001 * fs_));
    const double close = std::min(s_.gateClose, s_.gateOpen);
    openPower_ = std::pow(10.0, (s_.gateOpen - RmsCalibrationDb) / 10.0);
    closePower_ = std::pow(10.0, (close - RmsCalibrationDb) / 10.0);
    closedGain_ = s_.gateRange <= GateRangeMute + 1e-9 ? 0.0 : dbToLin(s_.gateRange);
    hpfOn_ = s_.gateHpf > GateHpfOff + 1e-9;
    if (hpfOn_) {
        const Biquad q = butterworthHighpass(std::min(s_.gateHpf, fs_ * 0.4), fs_);
        for (auto& h : hpf_) {
            const double z1 = h.z1, z2 = h.z2;
            h = q;
            h.z1 = z1;
            h.z2 = z2;
        }
    }
}
void Gate::process(const double* x, double* y) noexcept {
    double power = 0;
    for (int c = 0; c < channels_; ++c) {
        const double d = hpfOn_ ? hpf_[size_t(c)].tick(x[c]) : x[c];
        power_[size_t(c)] += powerCoef_ * (d * d - power_[size_t(c)]);
        power = std::max(power, power_[size_t(c)]);
    }
    double target = 1;
    if (on_) {
        if (!open_) {
            if (power >= openPower_) {
                open_ = true;
                holdCount_ = 0;
            }
        } else if (power <= closePower_) {
            if (++holdCount_ > holdSamples_) open_ = false;
        } else {
            holdCount_ = 0;
        }
        target = open_ ? 1.0 : closedGain_;
    } else {
        open_ = true;
        holdCount_ = 0;
    }
    const double coef = target > gain_ ? attack_ : release_;
    gain_ = target + coef * (gain_ - target);
    if (gain_ < 1e-12 && target == 0) gain_ = 0;
    for (int c = 0; c < channels_; ++c) {
        double* slot = &ring_[size_t(c) * size_t(delay_) + size_t(pos_)];
        y[c] = *slot * gain_;
        *slot = x[c];
    }
    if (++pos_ >= delay_) pos_ = 0;
}

// ---- Leveler ---------------------------------------------------------------------------------------
void Leveler::prepare(double fs, int channels) {
    fs_ = fs;
    channels_ = std::clamp(channels, 1, 2);
    powerCoef_ = 1.0 - tauCoef(300.0, fs);
    attack_ = timeCoef(AttackMs, fs);
    release_ = timeCoef(ReleaseMs, fs);
    fast_ = timeCoef(60.0, fs);
    reset();
}
void Leveler::reset() noexcept {
    power_ = 0;
    gainDb_ = 0;
}
void Leveler::set(const Settings& s) noexcept {
    on_ = s.levelerOn;
    target_ = s.levelerTarget;
    slope_ = 1.0 / std::max(1.0, s.levelerRatio) - 1.0;
}
void Leveler::process(const double* x, double* y, bool freeze) noexcept {
    double instant = 0;
    for (int c = 0; c < channels_; ++c) instant = std::max(instant, x[c] * x[c]);
    const double level = powerToDb(power_) + RmsCalibrationDb;
    const bool silent = level < -65.0;
    if (!freeze) power_ += powerCoef_ * (instant - power_);
    if (!on_) {
        gainDb_ *= fast_;
    } else if (!freeze && !silent) {
        const double wanted = std::clamp(slope_ * (level - target_), -LimitDb, LimitDb);
        const double coef = wanted < gainDb_ ? attack_ : release_;
        gainDb_ = wanted + coef * (gainDb_ - wanted);
    }
    const double g = std::abs(gainDb_) < 1e-9 ? 1.0 : dbToLin(gainDb_);
    for (int c = 0; c < channels_; ++c) y[c] = x[c] * g;
}

// ---- Compressor ------------------------------------------------------------------------------------
void Compressor::prepare(double fs, int channels) {
    fs_ = fs;
    channels_ = std::clamp(channels, 1, 2);
    powerCoef_ = 1.0 - tauCoef(2.0, fs);
    reset();
}
void Compressor::reset() noexcept {
    power_.fill(0);
    gainDb_ = 0;
}
void Compressor::set(const Settings& s) noexcept {
    on_ = s.compOn;
    attack_ = timeCoef(s.compAttack, fs_);
    release_ = timeCoef(s.compRelease, fs_);
    threshold_ = s.compThreshold;
    ratio_ = std::max(1.0, s.compRatio);
    knee_ = std::max(0.0, s.compKnee);
}
void Compressor::process(const double* x, double* y) noexcept {
    double power = 0;
    for (int c = 0; c < channels_; ++c) {
        power_[size_t(c)] += powerCoef_ * (x[c] * x[c] - power_[size_t(c)]);
        power = std::max(power, power_[size_t(c)]);
    }
    double wanted = 0;
    if (on_) {
        const double level = std::max(-100.0, powerToDb(power) + RmsCalibrationDb);
        const double over = level - threshold_;
        double out = level;
        if (2.0 * over >= knee_ && over > 0) out = threshold_ + over / ratio_;
        else if (knee_ > 0 && 2.0 * std::abs(over) < knee_) {
            const double t = over + knee_ / 2.0;
            out = level + (1.0 / ratio_ - 1.0) * t * t / (2.0 * knee_);
        }
        wanted = out - level;
    }
    const double coef = wanted < gainDb_ ? attack_ : release_;
    gainDb_ = wanted + coef * (gainDb_ - wanted);
    if (gainDb_ > -1e-9) gainDb_ = 0;
    const double g = gainDb_ > -1e-9 ? 1.0 : dbToLin(gainDb_);
    for (int c = 0; c < channels_; ++c) y[c] = x[c] * g;
}

// ---- PreStage --------------------------------------------------------------------------------------
void PreStage::prepare(double fs, int channels) {
    gate_.prepare(fs, channels);
    leveler_.prepare(fs, channels);
    comp_.prepare(fs, channels);
}
void PreStage::reset() noexcept {
    gate_.reset();
    leveler_.reset();
    comp_.reset();
}
void PreStage::set(const Settings& s) noexcept {
    gate_.set(s);
    leveler_.set(s);
    comp_.set(s);
}
void PreStage::process(const double* x, double* y) noexcept {
    double a[2], b[2];
    gate_.process(x, a);
    leveler_.process(a, b, !gate_.isOpen());
    comp_.process(b, y);
}
}  // namespace vd
