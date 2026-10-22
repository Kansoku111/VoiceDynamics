// Loudness measurement conformance: BS.1770-5 K-weighting and EBU Tech 3341 / 3342 minimum requirement
// signals, synthesized here from their written specification (the EBU files themselves were not available).
// Test 6 (5.0 channels) and the "authentic programme" cases 7/8 cannot be reproduced from the text;
// they are replaced by a cross-check against an independent implementation (tools/crosscheck.py).
#include "loudness.h"
#include "reference.h"
#include "testing.h"
#include <functional>

using namespace vd;
using t::Signal;

namespace {
struct Reading {
    double momentary, shortTerm, integrated, maxM, maxS, lra;
    bool hasI;
};
Reading measure(const Signal& s, bool dualMono = false) {
    LoudnessMeter m;
    m.prepare(s.fs, s.channels, dualMono);
    double f[2]{};
    for (size_t i = 0; i < s.frames(); ++i) {
        for (int c = 0; c < s.channels; ++c) f[c] = s.data[i * size_t(s.channels) + size_t(c)];
        m.push(f);
    }
    return {m.momentary(), m.shortTerm(), m.integrated(), m.maxMomentary(), m.maxShortTerm(), m.lra(), m.hasIntegrated()};
}

std::string rate(double fs) { return std::to_string(int(fs)) + " Hz"; }

void kFilter() {
    const Biquad s = kShelf(48000), h = kHighpass(48000);
    // BS.1770-5 tables 1 and 2 (48 kHz)
    t::near("K shelf b0 (48 kHz)", s.b0, 1.53512485958697, 1e-12);
    t::near("K shelf b1 (48 kHz)", s.b1, -2.69169618940638, 1e-12);
    t::near("K shelf b2 (48 kHz)", s.b2, 1.19839281085285, 1e-12);
    t::near("K shelf a1 (48 kHz)", s.a1, -1.69065929318241, 1e-12);
    t::near("K shelf a2 (48 kHz)", s.a2, 0.73248077421585, 1e-12);
    t::near("K highpass a1 (48 kHz)", h.a1, -1.99004745483398, 1e-12);
    t::near("K highpass a2 (48 kHz)", h.a2, 0.99007225036621, 1e-12);
    // 997 Hz gain of the cascade: +0.691 dB at 48 kHz, within 0.02 dB at the other rates (research/02)
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        Biquad a = kShelf(fs), b = kHighpass(fs);
        double peak = 0;
        const int n = int(fs);
        for (int i = 0; i < n; ++i) {
            const double y = b.tick(a.tick(std::sin(2 * t::Pi * 997.0 * i / fs)));
            if (i > n / 2) peak = std::max(peak, std::abs(y));
        }
        t::near("K-weighting gain at 997 Hz, " + rate(fs), t::db(peak), 0.691, 0.02, " dB");
    }
}

void tech3341(double fs) {
    const std::string at = " @" + rate(fs);
    {  // 1, 2
        Signal s(fs, 2);
        s.tone(1000, -23, 20);
        auto r = measure(s);
        t::near("3341-1 M" + at, r.momentary, -23.0, 0.1, " LUFS");
        t::near("3341-1 S" + at, r.shortTerm, -23.0, 0.1, " LUFS");
        t::near("3341-1 I" + at, r.integrated, -23.0, 0.1, " LUFS");
        Signal s2(fs, 2);
        s2.tone(1000, -33, 20);
        r = measure(s2);
        t::near("3341-2 M" + at, r.momentary, -33.0, 0.1, " LUFS");
        t::near("3341-2 S" + at, r.shortTerm, -33.0, 0.1, " LUFS");
        t::near("3341-2 I" + at, r.integrated, -33.0, 0.1, " LUFS");
    }
    {  // 3
        Signal s(fs, 2);
        s.tone(1000, -36, 10);
        s.tone(1000, -23, 60);
        s.tone(1000, -36, 10);
        t::near("3341-3 I" + at, measure(s).integrated, -23.0, 0.1, " LUFS");
    }
    {  // 4
        Signal s(fs, 2);
        s.tone(1000, -72, 10);
        s.tone(1000, -36, 10);
        s.tone(1000, -23, 60);
        s.tone(1000, -36, 10);
        s.tone(1000, -72, 10);
        t::near("3341-4 I" + at, measure(s).integrated, -23.0, 0.1, " LUFS");
    }
    {  // 5
        Signal s(fs, 2);
        s.tone(1000, -26, 20);
        s.tone(1000, -20, 20.1);
        s.tone(1000, -26, 20);
        t::near("3341-5 I" + at, measure(s).integrated, -23.0, 0.1, " LUFS");
    }
    {  // 9: S constant -23 after 3 s
        Signal s(fs, 2);
        for (int i = 0; i < 5; ++i) {
            s.tone(1000, -20, 1.34);
            s.tone(1000, -30, 1.66);
        }
        LoudnessMeter m;
        m.prepare(fs, 2, false);
        double f[2]{}, lo = 1e9, hi = -1e9;
        for (size_t i = 0; i < s.frames(); ++i) {
            f[0] = s.data[i * 2];
            f[1] = s.data[i * 2 + 1];
            m.push(f);
            if (double(i) / fs >= 3.0) {
                lo = std::min(lo, m.shortTerm());
                hi = std::max(hi, m.shortTerm());
            }
        }
        t::near("3341-9 S min after 3 s" + at, lo, -23.0, 0.1, " LUFS");
        t::near("3341-9 S max after 3 s" + at, hi, -23.0, 0.1, " LUFS");
    }
    {  // 12: M constant -23 after 1 s
        Signal s(fs, 2);
        for (int i = 0; i < 25; ++i) {
            s.tone(1000, -20, 0.18);
            s.tone(1000, -30, 0.22);
        }
        LoudnessMeter m;
        m.prepare(fs, 2, false);
        double f[2]{}, lo = 1e9, hi = -1e9;
        for (size_t i = 0; i < s.frames(); ++i) {
            f[0] = s.data[i * 2];
            f[1] = s.data[i * 2 + 1];
            m.push(f);
            if (double(i) / fs >= 1.0) {
                lo = std::min(lo, m.momentary());
                hi = std::max(hi, m.momentary());
            }
        }
        t::near("3341-12 M min after 1 s" + at, lo, -23.0, 0.1, " LUFS");
        t::near("3341-12 M max after 1 s" + at, hi, -23.0, 0.1, " LUFS");
    }
    {  // 10: Max S for each of 20 files
        double worst = 0;
        for (int i = 0; i < 20; ++i) {
            Signal s(fs, 2);
            s.silence(i * 0.15);
            s.tone(1000, -23, 3);
            s.silence(1);
            worst = std::max(worst, std::abs(measure(s).maxS + 23.0));
        }
        t::atMost("3341-10 Max S worst deviation (20 files)" + at, worst, 0.1, " LU");
    }
    {  // 13: Max M for each of 20 files
        double worst = 0;
        for (int i = 0; i < 20; ++i) {
            Signal s(fs, 2);
            s.silence(i * 0.02);
            s.tone(1000, -23, 0.4);
            s.silence(1);
            worst = std::max(worst, std::abs(measure(s).maxM + 23.0));
        }
        t::atMost("3341-13 Max M worst deviation (20 files)" + at, worst, 0.1, " LU");
    }
    {  // 11: live Max S, one long file, values -38 .. -19
        Signal s(fs, 2);
        for (int i = 0; i < 20; ++i) {
            s.silence(i * 0.15);
            s.tone(1000, -38.0 + i, 3);
            s.silence(3 - i * 0.15);
        }
        LoudnessMeter m;
        m.prepare(fs, 2, false);
        double f[2]{}, worst = 0;
        size_t i = 0;
        for (int k = 0; k < 20; ++k) {
            const size_t end = size_t(std::llround((k + 1) * 6.0 * fs));
            double best = -1e9;
            for (; i < end && i < s.frames(); ++i) {
                f[0] = s.data[i * 2];
                f[1] = s.data[i * 2 + 1];
                m.push(f);
                if (double(i) / fs >= 3.0 || k > 0) best = std::max(best, m.shortTerm());
            }
            worst = std::max(worst, std::abs(best - (-38.0 + k)));
        }
        t::atMost("3341-11 Max S worst deviation (20 successive values)" + at, worst, 0.1, " LU");
    }
    {  // 14: live Max M, values -38 .. -19
        Signal s(fs, 2);
        for (int i = 0; i < 20; ++i) {
            s.silence(i * 0.02);
            s.tone(1000, -38.0 + i, 0.4);
            s.silence(0.4 - i * 0.02);
        }
        LoudnessMeter m;
        m.prepare(fs, 2, false);
        double f[2]{}, worst = 0;
        size_t i = 0;
        for (int k = 0; k < 20; ++k) {
            const size_t end = size_t(std::llround((k + 1) * 0.8 * fs));
            double best = -1e9;
            for (; i < end && i < s.frames(); ++i) {
                f[0] = s.data[i * 2];
                f[1] = s.data[i * 2 + 1];
                m.push(f);
                if (double(i) / fs >= 0.4) best = std::max(best, m.momentary());
            }
            worst = std::max(worst, std::abs(best - (-38.0 + k)));
        }
        t::atMost("3341-14 Max M worst deviation (20 successive values)" + at, worst, 0.1, " LU");
    }
    {  // calibration: 1 kHz peak -18 dBFS in phase -> -18.0 LUFS; mono counts as 1 ch, dual mono +3.01 dB
        Signal s(fs, 2);
        s.tone(1000, -18, 10);
        t::near("calibration -18 dBFS stereo" + at, measure(s).integrated, -18.0, 0.05, " LUFS");
        Signal m1(fs, 1);
        m1.tone(1000, -18, 10);
        const double mono = measure(m1).integrated, dual = measure(m1, true).integrated;
        t::near("mono 1ch = stereo - 3.01 dB" + at, mono, -18.0 - 3.0103, 0.05, " LUFS");
        t::near("dual mono = stereo" + at, dual, -18.0, 0.05, " LUFS");
    }
}

double referenceTruePeak(const std::vector<double>& x) {
    static const ref::TruePeak16 r;
    return r.peak(x);
}

double detectorPeak(const std::vector<double>& x, double fs) {
    TruePeakDetector d;
    d.prepare(fs, 1);
    double peak = 0;
    for (double v : x) peak = std::max(peak, d.push(&v, 0));
    const double zero = 0;
    for (int i = 0; i < d.delay() + 2; ++i) peak = std::max(peak, d.push(&zero, 0));
    return peak;
}

std::vector<double> sineBurst(double fs, double hz, double amp, double phaseDeg, double seconds) {
    const size_t n = size_t(std::llround(seconds * fs));
    std::vector<double> x(n);
    const double fade = 0.01 * fs;
    for (size_t i = 0; i < n; ++i) {
        const double env = std::min({1.0, double(i) / fade, double(n - 1 - i) / fade});
        x[i] = amp * env * std::sin(2 * t::Pi * hz * double(i) / fs + phaseDeg * t::Pi / 180);
    }
    return x;
}

void tech3341TruePeak(double fs) {
    const std::string at = " @" + rate(fs);
    struct Case { const char* name; double hz, amp, phase, expected; };
    const Case cases[]{{"15 fs/4 0.50 FFS 0 deg", fs / 4, 0.5, 0, -6.0},   {"16 fs/4 0.50 FFS 45 deg", fs / 4, 0.5, 45, -6.0},
                       {"17 fs/6 0.50 FFS 60 deg", fs / 6, 0.5, 60, -6.0}, {"18 fs/8 0.50 FFS 67.5 deg", fs / 8, 0.5, 67.5, -6.0},
                       {"19 fs/4 1.41 FFS 45 deg", fs / 4, 1.41, 45, 3.0}};
    for (const auto& c : cases) {
        const auto x = sineBurst(fs, c.hz, c.amp, c.phase, 0.5);
        const double mine = t::db(detectorPeak(x, fs)), ref = t::db(referenceTruePeak(x));
        t::check(mine >= c.expected - 0.4 && mine <= c.expected + 0.2, std::string("3341-") + c.name + at,
                 t::num(mine) + " dBTP (window " + t::num(c.expected - 0.4, 1) + " .. " + t::num(c.expected + 0.2, 1) + "; 16x reference " + t::num(ref) + ")");
    }
    // 20-23: synthesized at 4*fs, low-pass filtered, taken every 4th sample with offset 0..3
    std::vector<double> hi;
    auto append = [&](double hz4, double amp, int samples, double phase) {
        for (int i = 0; i < samples; ++i) hi.push_back(amp * std::sin(2 * t::Pi * hz4 * i + phase));
    };
    const double sixth = 1.0 / 24.0, quarter = 1.0 / 16.0;  // cycles per sample at 4*fs
    append(sixth, 0.5, 24 * 12, 0);
    append(quarter, 1.0, 16, 0);
    append(sixth, 0.5, 24 * 12, 0);
    const int taps = 255, mid = taps / 2;
    std::vector<double> lp(taps);
    double sum = 0;
    for (int i = 0; i < taps; ++i) {
        const double tt = i - mid, w = 0.5 - 0.5 * std::cos(2 * t::Pi * (i + 0.5) / taps);
        lp[size_t(i)] = (tt == 0 ? 0.25 : std::sin(2 * t::Pi * 0.125 * tt) / (t::Pi * tt)) * w;
        sum += lp[size_t(i)];
    }
    for (auto& v : lp) v /= sum;
    std::vector<double> filtered(hi.size(), 0.0);
    for (size_t n = 0; n < hi.size(); ++n)
        for (int k = 0; k < taps; ++k) {
            const long j = long(n) + k - mid;
            if (j >= 0 && j < long(hi.size())) filtered[n] += lp[size_t(k)] * hi[size_t(j)];
        }
    for (int offset = 0; offset < 4; ++offset) {
        std::vector<double> x;
        for (size_t n = size_t(offset); n < filtered.size(); n += 4) x.push_back(filtered[n]);
        const double fade = 8;
        for (size_t i = 0; i < x.size(); ++i) x[i] *= std::min({1.0, double(i) / fade, double(x.size() - 1 - i) / fade});
        const double mine = t::db(detectorPeak(x, fs)), ref = t::db(referenceTruePeak(x));
        t::check(mine >= -0.4 && mine <= 0.2, "3341-" + std::to_string(20 + offset) + " single fs/4 period, offset " + std::to_string(offset) + at,
                 t::num(mine) + " dBTP (window -0.4 .. +0.2; 16x reference " + t::num(ref) + ")");
        t::near("detector vs 16x reference, case " + std::to_string(20 + offset) + at, mine, ref, 0.15, " dB");
    }
}

void tech3342(double fs) {
    const std::string at = " @" + rate(fs);
    auto lra = [&](std::initializer_list<double> levels) {
        Signal s(fs, 2);
        for (double l : levels) s.tone(1000, l, 20);
        s.silence(2.0);  // the analysis window latency (>= 1.5 s of silence)
        return measure(s).lra;
    };
    t::near("3342-1 LRA (-20/-30)" + at, lra({-20, -30}), 10, 1, " LU");
    t::near("3342-2 LRA (-20/-15)" + at, lra({-20, -15}), 5, 1, " LU");
    t::near("3342-3 LRA (-40/-20)" + at, lra({-40, -20}), 20, 1, " LU");
    t::near("3342-4 LRA (-50/-35/-20/-35/-50)" + at, lra({-50, -35, -20, -35, -50}), 15, 1, " LU");
    t::near("3342 repeated twice keeps LRA (-20/-30)" + at, lra({-20, -30, -20, -30}), 10, 1, " LU");
}
}  // namespace

int main() {
    kFilter();
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        tech3341(fs);
        tech3341TruePeak(fs);
        tech3342(fs);
    }
    // No valid block: Integrated is "not available", never -inf or NaN
    {
        Signal s(48000, 2);
        s.silence(3);
        const auto r = measure(s);
        t::check(!r.hasI && std::isfinite(r.integrated) && std::isfinite(r.momentary), "silence gives no Integrated and no NaN");
        Signal q(48000, 2);
        q.tone(1000, -80, 5);
        t::check(!measure(q).hasI, "a tone below the absolute gate gives no Integrated");
    }
    return t::finish("conformance_tests");
}
