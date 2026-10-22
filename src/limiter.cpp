#include "limiter.h"
#include <algorithm>
#include <cmath>

namespace vd {
void Limiter::prepare(double fs, int channels) {
    fs_ = fs;
    channels_ = std::clamp(channels, 1, 2);
    detector_.prepare(fs, channels_);
    lookahead_ = std::max(2, int(std::lround(LookaheadMs * 0.001 * fs)));
    latency_ = (lookahead_ - 1) + detector_.delay();
    delay_.assign(size_t(latency_) * 2, 0.0);
    minIndex_.assign(size_t(lookahead_) + 2, 0);
    minValue_.assign(size_t(lookahead_) + 2, 1.0);
    average_.assign(size_t(lookahead_), 1.0);
    set(true, -1.0, 80.0);
    reset();
}

void Limiter::reset() noexcept {
    detector_.reset();
    std::fill(delay_.begin(), delay_.end(), 0.0);
    std::fill(average_.begin(), average_.end(), 1.0);
    averageSum_ = double(lookahead_);
    averagePos_ = 0;
    minHead_ = minTail_ = 0;
    pos_ = 0;
    index_ = 0;
    reductionDb_ = 0;
}

void Limiter::set(bool on, double ceilingDb, double releaseMs) noexcept {
    on_ = on;
    ceilingLin_ = dbToLin(ceilingDb - GuardDb);
    releaseMs_ = releaseMs;
}

void Limiter::process(const double* x, double* y) noexcept {
    // 1. Largest gain allowed for the interval that just became known (detector delay samples ago).
    const double peak = detector_.push(x, ceilingLin_);
    const double allowed = (on_ && peak > ceilingLin_) ? ceilingLin_ / peak : 1.0;

    // 2. Sliding minimum over the last `lookahead_` allowed gains.
    const size_t capacity = minIndex_.size();
    while (minTail_ != minHead_) {
        const size_t last = size_t((minTail_ - 1 + int(capacity)) % int(capacity));
        if (minValue_[last] < allowed) break;
        minTail_ = int(last);
    }
    minIndex_[size_t(minTail_)] = index_;
    minValue_[size_t(minTail_)] = allowed;
    minTail_ = int((size_t(minTail_) + 1) % capacity);
    while (minIndex_[size_t(minHead_)] + uint64_t(lookahead_) <= index_) minHead_ = int((size_t(minHead_) + 1) % capacity);
    const double window = minValue_[size_t(minHead_)];
    ++index_;

    // 3. Moving average of the same length: every term is <= the allowed gain of the sample it covers.
    averageSum_ += window - average_[size_t(averagePos_)];
    average_[size_t(averagePos_)] = window;
    if (++averagePos_ >= lookahead_) averagePos_ = 0;
    double gain = averageSum_ / lookahead_;
    if (gain > 1.0) gain = 1.0;

    // 4. Attack follows the gain exactly; recovery is exponential and slower for deeper reduction.
    const double wantedDb = gain >= 1.0 ? 0.0 : linToDb(gain);
    if (wantedDb <= reductionDb_) {
        reductionDb_ = wantedDb;
    } else {
        const double coef = tauCoef(releaseMs_ * (1.0 + 0.125 * -reductionDb_), fs_);
        reductionDb_ = wantedDb + coef * (reductionDb_ - wantedDb);
        if (reductionDb_ > -1e-7) reductionDb_ = 0;
    }
    const double applied = reductionDb_ == 0 ? 1.0 : dbToLin(reductionDb_);

    for (int c = 0; c < channels_; ++c) {
        double* slot = &delay_[size_t(c) * size_t(latency_) + size_t(pos_)];
        y[c] = *slot * applied;
        *slot = x[c];
    }
    if (++pos_ >= latency_) pos_ = 0;
}
}  // namespace vd
