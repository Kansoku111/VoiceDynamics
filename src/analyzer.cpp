#include "analyzer.h"
#include "chain.h"
#include <algorithm>
#include <cmath>

namespace vd {
namespace {
constexpr double MaxAutoGainDb = 60.0, Converged = 0.03;

struct Evaluation {
    double gain = 0, lufs = SilenceDb, truePeakDb = SilenceDb, maxGr = 0;
};

class Solver {
public:
    Solver(const Settings& s, int channels, double fs, const ChunkedPcm& pre, const PreMeasurement& m, const Progress& progress)
        : s_(s), ch_(channels), fs_(fs), pre_(pre), m_(m), progress_(progress), frames_(pre.frames()) {}

    AnalysisResult run() {
        AnalysisResult r;
        r.durationSec = double(frames_) / fs_;
        if (!frames_ || ch_ < 1 || ch_ > 2 || !(fs_ >= 8000)) {
            r.error = "解析できる音声がありません。";
            return r;
        }
        const double l0 = gatedIntegrated(m_.blocks, 0);
        r.preLufs = l0;
        r.prePeakDb = linToDb(m_.peak);
        if (l0 <= SilenceDb + 1) {
            r.error = "測定できる音がありません（無音、または−70 LUFS未満です）。";
            return r;
        }
        // Gain that reaches the target when nothing is limited (the gates move with the gain, so iterate).
        double g = s_.target - l0;
        for (int i = 0; i < 60; ++i) {
            const double l = gatedIntegrated(m_.blocks, g);
            const double err = s_.target - l;
            if (std::abs(err) < 1e-5) break;
            g += err;
        }
        g = std::clamp(g, -MaxAutoGainDb, MaxAutoGainDb);
        const double ceilingDb = s_.ceiling - Limiter::GuardDb;
        Evaluation best;
        if (!s_.limiterOn || r.prePeakDb + g <= ceilingDb - 0.001) {
            // The limiter never engages: the output is the compressed signal times the gain.
            best.gain = g;
            best.lufs = gatedIntegrated(m_.blocks, g);
            best.truePeakDb = r.prePeakDb + g;
            best.maxGr = 0;
        } else if (!search(g, best, r)) {
            return r;
        }
        r.gainDb = best.gain;
        r.predictedLufs = best.lufs;
        r.truePeakDb = best.truePeakDb;
        r.maxGrDb = -best.maxGr;
        r.errorLu = best.lufs - s_.target;
        r.unreached = std::abs(r.errorLu) > ToleranceLu;
        if (r.unreached) r.warnings |= WarnUnreached;
        if (r.truePeakDb > s_.ceiling + 0.01) r.warnings |= s_.limiterOn ? WarnTruePeak : WarnLimiterOff;
        if (r.durationSec < ShortSeconds) r.warnings |= WarnShort;
        r.ok = true;
        return r;
    }

private:
    bool report(double fraction) { return !progress_ || progress_(std::clamp(fraction, 0.0, 1.0)); }

    // One pass of the limiter over the stored signal with `gainDb` applied.
    bool evaluate(double gainDb, Evaluation& e, AnalysisResult& r) {
        Limiter limiter;
        limiter.prepare(fs_, ch_);
        limiter.set(s_.limiterOn, s_.ceiling, s_.limiterRelease);
        limiter.reset();
        TruePeakDetector detector;
        detector.prepare(fs_, ch_);
        LoudnessMeter meter;
        meter.prepare(fs_, ch_, s_.dualMono);
        std::vector<double> blocks;
        meter.setBlockSink(&blocks);
        const size_t delay = size_t(limiter.latency()), total = frames_ + delay;
        const double lin = dbToLin(gainDb);
        double x[2]{0, 0}, y[2]{0, 0}, peak = 0, minGr = 0;
        for (size_t i = 0; i < total; ++i) {
            if (i < frames_) {
                const float* f = pre_.frame(i);
                for (int c = 0; c < ch_; ++c) x[c] = double(f[c]) * lin;
            } else {
                x[0] = x[1] = 0;
            }
            limiter.process(x, y);
            minGr = std::min(minGr, limiter.grDb());
            if (i >= delay) {
                meter.push(y);
                peak = std::max(peak, detector.push(y, peak));
            }
            if ((i & 0xFFFF) == 0 && !report(passBase_ + passSpan_ * double(i) / double(total))) {
                r.cancelled = true;
                r.error = "中止しました。";
                return false;
            }
        }
        const double zero[2]{0, 0};
        for (int i = 0; i < detector.delay(); ++i) peak = std::max(peak, detector.push(zero, peak));
        e.gain = gainDb;
        e.lufs = gatedIntegrated(blocks, 0);
        e.truePeakDb = linToDb(peak);
        e.maxGr = minGr;
        ++r.evaluations;
        passBase_ += passSpan_;
        return true;
    }

    // Secant search on the real chain output; keeps the closest evaluation.
    bool search(double start, Evaluation& best, AnalysisResult& r) {
        passBase_ = 0.0;
        passSpan_ = 1.0 / 6;
        Evaluation previous, current;
        double g = start;
        bool havePrevious = false;
        double bestError = 1e9;
        for (int i = 0; i < 8; ++i) {
            if (!evaluate(g, current, r)) return false;
            const double err = s_.target - current.lufs;
            if (current.lufs <= SilenceDb + 1) {
                r.error = "リミッター後の音を測定できません。";
                return false;
            }
            if (std::abs(err) < bestError) {
                bestError = std::abs(err);
                best = current;
            }
            if (std::abs(err) <= Converged) break;
            double next = g + err;  // a unit slope until two points give a better one
            if (havePrevious) {
                const double slope = (current.lufs - previous.lufs) / (current.gain - previous.gain);
                if (slope < 0.02) break;  // louder input no longer makes it louder: the limiter is saturated
                next = g + err / std::clamp(slope, 0.05, 1.5);
            }
            next = std::clamp(next, g - 12.0, g + 12.0);
            next = std::clamp(next, -MaxAutoGainDb, MaxAutoGainDb);
            if (std::abs(next - g) < 1e-6) break;
            previous = current;
            havePrevious = true;
            g = next;
        }
        return true;
    }

    Settings s_;
    int ch_;
    double fs_;
    const ChunkedPcm& pre_;
    const PreMeasurement& m_;
    const Progress& progress_;
    size_t frames_;
    double passBase_ = 0, passSpan_ = 0.1;
};
}  // namespace

AnalysisResult solveGain(const Settings& settings, int channels, double fs, const ChunkedPcm& pre, const PreMeasurement& measured, const Progress& progress) {
    return Solver(settings, channels, fs, pre, measured, progress).run();
}

AnalysisResult analyzeFile(const Settings& s, int channels, double fs, ChunkedPcm& pcm, const Progress& progress) {
    AnalysisResult failed;
    const size_t frames = pcm.frames();
    if (!frames || channels < 1 || channels > 2 || pcm.channels() != channels) {
        failed.error = "解析できる音声がありません。";
        return failed;
    }
    PreStage pre;
    pre.prepare(fs, channels);
    pre.set(s);
    pre.reset();
    TruePeakDetector detector;
    detector.prepare(fs, channels);
    LoudnessMeter meter;
    meter.prepare(fs, channels, s.dualMono);
    PreMeasurement m;
    meter.setBlockSink(&m.blocks);
    const size_t delay = size_t(pre.latency()), total = frames + delay;
    double x[2]{0, 0}, y[2]{0, 0}, flat[2]{0, 0};
    for (size_t i = 0; i < total; ++i) {
        for (int c = 0; c < channels; ++c) x[c] = i < frames ? double(pcm.frame(i)[c]) : 0.0;
        pre.process(x, y);
        if (i >= delay) {
            float* out = pcm.frame(i - delay);
            for (int c = 0; c < channels; ++c) {
                out[c] = float(y[c]);
                flat[c] = double(out[c]);
            }
            meter.push(flat);
            m.peak = std::max(m.peak, detector.push(flat, m.peak));
        }
        if ((i & 0xFFFF) == 0 && progress && !progress(0.3 * double(i) / double(total))) {
            failed.cancelled = true;
            failed.error = "中止しました。";
            return failed;
        }
    }
    const double zero[2]{0, 0};
    for (int i = 0; i < detector.delay(); ++i) m.peak = std::max(m.peak, detector.push(zero, m.peak));
    Progress rest = progress ? Progress([&](double f) { return progress(0.3 + 0.7 * f); }) : Progress();
    return solveGain(s, channels, fs, pcm, m, rest);
}
}  // namespace vd
