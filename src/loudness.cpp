#include "loudness.h"
#include <algorithm>
#include <cmath>

namespace vd {
double gatedIntegrated(const std::vector<double>& blocks, double gainDb, size_t* used) {
    const double scale = dbToLin(gainDb) * dbToLin(gainDb);  // power
    // Absolute gate on the scaled loudness: power threshold for -70 LUFS.
    const double absPower = std::pow(10.0, (AbsoluteGate - LoudnessOffset) / 10.0);
    double sum = 0;
    size_t n = 0;
    for (double b : blocks) {
        const double p = b * scale;
        if (p > absPower) {
            sum += p;
            ++n;
        }
    }
    if (!n) {
        if (used) *used = 0;
        return SilenceDb;
    }
    const double relPower = (sum / double(n)) * 0.1;  // -10 LU
    double sum2 = 0;
    size_t n2 = 0;
    for (double b : blocks) {
        const double p = b * scale;
        if (p > absPower && p >= relPower) {
            sum2 += p;
            ++n2;
        }
    }
    if (used) *used = n2;
    return n2 ? blockLoudness(sum2 / double(n2)) : SilenceDb;
}

void LoudnessMeter::Histogram::clear() noexcept {
    std::fill(count.begin(), count.end(), 0u);
    std::fill(sum.begin(), sum.end(), 0.0);
    total = 0;
    energy = 0;
}
void LoudnessMeter::Histogram::add(double loudness, double power) noexcept {
    if (!(loudness > AbsoluteGate)) return;
    const int i = std::clamp(int((loudness - BinLow) / BinStep), 0, Bins - 1);
    ++count[size_t(i)];
    sum[size_t(i)] += power;
    ++total;
    energy += power;
}

void LoudnessMeter::prepare(double fs, int channels, bool dualMono) {
    fs_ = fs;
    channels_ = std::clamp(channels, 1, 2);
    weight_ = (channels_ == 1 && dualMono) ? 2.0 : 1.0;
    segLen_ = std::max(1, int(std::lround(fs / 100.0)));
    for (int c = 0; c < 2; ++c) {
        shelf_[size_t(c)] = kShelf(fs);
        high_[size_t(c)] = kHighpass(fs);
    }
    blocks_.prepare();
    shortTerms_.prepare();
    reset();
}

void LoudnessMeter::reset() noexcept {
    for (auto& b : shelf_) b.reset();
    for (auto& b : high_) b.reset();
    acc_ = 0;
    inSegment_ = 0;
    segs_.fill(0);
    segments_ = 0;
    blocks_.clear();
    shortTerms_.clear();
    momentary_ = shortTerm_ = maxMomentary_ = maxShortTerm_ = integrated_ = SilenceDb;
    lra_ = 0;
    hasIntegrated_ = hasLra_ = false;
}

void LoudnessMeter::push(const double* frame) noexcept {
    for (int c = 0; c < channels_; ++c) {
        const double y = high_[size_t(c)].tick(shelf_[size_t(c)].tick(frame[c]));
        acc_ += weight_ * y * y;
    }
    if (++inSegment_ >= segLen_) finishSegment();
}

double LoudnessMeter::gatedMean(const Histogram& h, double relative, uint64_t* count, double* low, double* high) const noexcept {
    *count = 0;
    if (!h.total) return 0;
    const double threshold = blockLoudness(h.energy / double(h.total)) + relative;
    int first = int(std::ceil((threshold - BinLow) / BinStep - 0.5));
    first = std::clamp(first, 0, Bins);
    uint64_t n = 0;
    double sum = 0;
    for (int i = first; i < Bins; ++i) {
        n += h.count[size_t(i)];
        sum += h.sum[size_t(i)];
    }
    *count = n;
    if (!n) return 0;
    if (low) {
        // Percentiles of the gated distribution (index round((n-1) p) as in the Tech 3342 reference).
        const uint64_t lowIndex = uint64_t(std::llround(double(n - 1) * 0.10)), highIndex = uint64_t(std::llround(double(n - 1) * 0.95));
        uint64_t seen = 0;
        bool haveLow = false;
        for (int i = first; i < Bins; ++i) {
            seen += h.count[size_t(i)];
            const double center = BinLow + (i + 0.5) * BinStep;
            if (!haveLow && seen > lowIndex) {
                *low = center;
                haveLow = true;
            }
            if (seen > highIndex) {
                *high = center;
                break;
            }
        }
    }
    return sum / double(n);
}

void LoudnessMeter::finishSegment() noexcept {
    const double energy = acc_ / segLen_;
    acc_ = 0;
    inSegment_ = 0;
    segs_[size_t(segments_ % ShortTermSegments)] = energy;
    ++segments_;
    auto windowMean = [&](int length) {
        const int n = int(std::min<uint64_t>(segments_, uint64_t(length)));
        double sum = 0;
        for (int k = 0; k < n; ++k) sum += segs_[size_t((segments_ - 1 - uint64_t(k)) % ShortTermSegments)];
        return sum / n;
    };
    const double m = windowMean(MomentarySegments), s = windowMean(ShortTermSegments);
    momentary_ = blockLoudness(m);
    shortTerm_ = blockLoudness(s);
    if (segments_ >= uint64_t(MomentarySegments)) maxMomentary_ = std::max(maxMomentary_, momentary_);
    if (segments_ >= uint64_t(ShortTermSegments)) maxShortTerm_ = std::max(maxShortTerm_, shortTerm_);
    if (segments_ >= uint64_t(MomentarySegments) && (segments_ - MomentarySegments) % SegmentsPerHop == 0) {
        blocks_.add(momentary_, m);
        if (sink_) sink_->push_back(m);
        uint64_t n = 0;
        const double mean = gatedMean(blocks_, -10.0, &n, nullptr, nullptr);
        hasIntegrated_ = n > 0;
        integrated_ = n > 0 ? blockLoudness(mean) : SilenceDb;
    }
    if (segments_ >= uint64_t(ShortTermSegments) && (segments_ - ShortTermSegments) % SegmentsPerHop == 0) {
        shortTerms_.add(shortTerm_, s);
        uint64_t n = 0;
        double low = 0, high = 0;
        gatedMean(shortTerms_, -20.0, &n, &low, &high);
        hasLra_ = n > 0;
        lra_ = n > 0 ? std::max(0.0, high - low) : 0;
    }
}
}  // namespace vd
