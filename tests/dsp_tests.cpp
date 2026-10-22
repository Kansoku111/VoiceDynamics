// DSP behaviour of the gate, leveler, compressor, True Peak limiter, the chain, the whole-file analysis,
// the playback learner, abnormal input and real-time safety (no allocation in the processing path).
#include "analyzer.h"
#include "chain.h"
#include "learner.h"
#include "loudness.h"
#include "reference.h"
#include "testing.h"
#include "voice.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>

// ---- allocation counter (armed around code that must not allocate) --------------------------------
namespace {
thread_local bool g_armed = false;
std::atomic<long> g_allocations{0};
}  // namespace
void* operator new(size_t n) {
    if (g_armed) ++g_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) {
    if (g_armed) ++g_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

using namespace vd;
using t::Signal;

namespace {
const ref::TruePeak16& reference() {
    static const ref::TruePeak16 r;
    return r;
}

// Runs the whole chain; the output is latency-compensated and has the length of the input.
std::vector<float> runChain(Chain& chain, const std::vector<float>& in, int channels, std::vector<float>* pre = nullptr) {
    const size_t frames = in.size() / size_t(channels), latency = size_t(chain.latency());
    std::vector<float> out(in.size());
    if (pre) pre->assign(in.size(), 0.f);
    double x[2]{}, y[2]{}, p[2]{};
    for (size_t i = 0; i < frames + latency; ++i) {
        for (int c = 0; c < channels; ++c) x[c] = i < frames ? double(in[i * size_t(channels) + size_t(c)]) : 0.0;
        chain.tick(x, y, p);
        if (i >= latency)
            for (int c = 0; c < channels; ++c) out[(i - latency) * size_t(channels) + size_t(c)] = float(y[c]);
        if (pre && i >= size_t(chain.pre().latency()) && i - size_t(chain.pre().latency()) < frames)
            for (int c = 0; c < channels; ++c) (*pre)[(i - size_t(chain.pre().latency())) * size_t(channels) + size_t(c)] = float(p[c]);
    }
    return out;
}

double integratedOf(const std::vector<float>& s, double fs, int channels, bool dualMono = false, double* maxMomentary = nullptr) {
    LoudnessMeter m;
    m.prepare(fs, channels, dualMono);
    double f[2]{};
    for (size_t i = 0; i < s.size() / size_t(channels); ++i) {
        for (int c = 0; c < channels; ++c) f[c] = s[i * size_t(channels) + size_t(c)];
        m.push(f);
    }
    if (maxMomentary) *maxMomentary = m.maxMomentary();
    return m.integrated();
}

double peakDb(const std::vector<float>& s, int channels) { return ref::db(reference().peak(s, channels)); }

Settings quiet() {  // everything off except the limiter
    Settings s;
    s.gateOn = false;
    s.compOn = false;
    s.levelerOn = false;
    return s;
}

// ---- gate ------------------------------------------------------------------------------------------
void gateTests() {
    const double fs = 48000;
    auto run = [&](Settings s, const std::vector<float>& in) {
        Gate g;
        g.prepare(fs, 1);
        g.set(s);
        g.reset();
        std::vector<double> out, gain;
        double x[2]{}, y[2]{};
        for (float v : in) {
            x[0] = v;
            g.process(x, y);
            out.push_back(y[0]);
            gain.push_back(g.gain());
        }
        return std::make_pair(out, gain);
    };
    Settings s;
    s.gateOpen = -42;
    s.gateClose = -48;
    s.gateHold = 120;
    s.gateAttack = 3;
    s.gateRelease = 180;
    s.gateRange = -12;
    Signal low(fs, 1);  // quiet noise-like tone at -60 dB: below the close threshold
    low.tone(300, -60, 2);
    auto r = run(s, low.data);
    t::near("gate: signal below the threshold ends 12 dB down (Range)", t::db(r.second.back()), -12.0, 0.2, " dB");
    Signal loud(fs, 1);
    loud.tone(300, -20, 1);
    r = run(s, loud.data);
    t::near("gate: loud signal passes at unity", r.second.back(), 1.0, 1e-6);
    // hysteresis: a tone between close and open thresholds keeps the previous state
    Signal mid(fs, 1);
    mid.silence(1.0);       // the gate closes first
    mid.tone(300, -45, 2);  // -45 dB: above close (-48), below open (-42)
    r = run(s, mid.data);
    t::near("gate: between the thresholds a closed gate stays closed", t::db(r.second.back()), -12.0, 0.2, " dB");
    Signal openThenMid(fs, 1);
    openThenMid.tone(300, -20, 0.5);
    openThenMid.tone(300, -45, 2);
    r = run(s, openThenMid.data);
    t::near("gate: between the thresholds an open gate stays open", r.second.back(), 1.0, 1e-6);
    // hold: after the tone ends the gate stays open for about Hold
    Signal burst(fs, 1);
    burst.tone(300, -20, 0.5);
    burst.silence(1.0);
    r = run(s, burst.data);
    const size_t end = size_t(0.5 * fs);
    size_t closing = end;
    while (closing < r.second.size() && r.second[closing] > 0.999) ++closing;
    // the 10 ms RMS detector needs about 65 ms to fall from -20 dB to the close threshold, then Hold counts
    t::near("gate: Hold keeps it open after the sound ends (Hold 120 ms + detector decay)", double(closing - end) / fs * 1000, 120.0 + 65.0, 30.0, " ms");
    // look-ahead: the gate is fully open when a burst reaches the output
    Settings fast = s;
    fast.gateAttack = 1;
    Signal onset(fs, 1);
    onset.silence(1.0);
    onset.tone(500, -10, 0.2, 0);
    r = run(fast, onset.data);
    const size_t start = size_t(1.0 * fs) + size_t(0.003 * fs);  // where the burst appears at the output (3 ms later)
    t::atLeast("gate: look-ahead opens before the word arrives (gain at the first output sample)", t::db(r.second[start]), -3.0, " dB");
    // mute
    Settings mute = s;
    mute.gateRange = -80;
    r = run(mute, low.data);
    t::near("gate: Range -80 dB is a full mute", r.first.back(), 0.0, 1e-12);
    // no sample-to-sample jump of the gain larger than the smoothing allows (no clicks)
    double maxStep = 0;
    for (size_t i = 1; i < r.second.size(); ++i) maxStep = std::max(maxStep, std::abs(r.second[i] - r.second[i - 1]));
    Signal chatter(fs, 1);
    for (int i = 0; i < 20; ++i) {
        chatter.tone(300, -30, 0.05);
        chatter.tone(300, -60, 0.05);
    }
    auto c = run(s, chatter.data);
    maxStep = 0;
    for (size_t i = 1; i < c.second.size(); ++i) maxStep = std::max(maxStep, std::abs(c.second[i] - c.second[i - 1]));
    t::atMost("gate: largest gain step per sample with a chattering signal", maxStep, 0.01);
    // off: transparent apart from the delay
    Settings off = s;
    off.gateOn = false;
    r = run(off, loud.data);
    const size_t d = size_t(std::lround(0.003 * fs));
    double diff = 0;
    for (size_t i = d; i < loud.data.size(); ++i) diff = std::max(diff, std::abs(r.first[i] - double(loud.data[i - d])));
    t::near("gate off: output equals the input delayed by 3 ms", diff, 0.0, 1e-9);
}

// ---- compressor and leveler ------------------------------------------------------------------------
double steadyLevel(Settings s, double toneDb, double seconds, double fs, double* grDb = nullptr) {
    Compressor c;
    c.prepare(fs, 1);
    c.set(s);
    c.reset();
    Signal sig(fs, 1);
    sig.tone(997, toneDb, seconds);
    double x[2]{}, y[2]{}, peak = 0;
    for (size_t i = 0; i < sig.data.size(); ++i) {
        x[0] = sig.data[i];
        c.process(x, y);
        if (i > sig.data.size() * 9 / 10) peak = std::max(peak, std::abs(y[0]));
    }
    if (grDb) *grDb = c.grDb();
    return t::db(peak);
}

void compressorTests() {
    const double fs = 48000;
    Settings s;
    s.compThreshold = -24;
    s.compRatio = 3;
    s.compKnee = 0;
    s.compAttack = 5;
    s.compRelease = 100;
    t::near("compressor: below the threshold nothing changes (-40 dB)", steadyLevel(s, -40, 1.5, fs), -40.0, 0.1, " dB");
    t::near("compressor: 3:1 above the threshold (-12 dB -> -20 dB)", steadyLevel(s, -12, 1.5, fs), -20.0, 0.25, " dB");
    t::near("compressor: 3:1 above the threshold (-6 dB -> -18 dB)", steadyLevel(s, -6, 1.5, fs), -18.0, 0.25, " dB");
    s.compKnee = 6;
    t::near("compressor: soft knee at the threshold reduces by 0.5 dB", steadyLevel(s, -24, 1.5, fs), -24.5, 0.12, " dB");
    t::near("compressor: soft knee well below the knee is untouched", steadyLevel(s, -32, 1.5, fs), -32.0, 0.1, " dB");
    t::near("compressor: soft knee well above follows the ratio", steadyLevel(s, -12, 1.5, fs), -20.0, 0.3, " dB");
    // timing: the gain reduction follows a step with the attack time and releases with the release time
    s.compKnee = 0;
    s.compAttack = 20;
    s.compRelease = 200;
    Compressor c;
    c.prepare(fs, 1);
    c.set(s);
    c.reset();
    Signal step(fs, 1);
    step.tone(997, -50, 0.5);
    step.tone(997, -6, 1.0);
    step.tone(997, -50, 1.5);
    double x[2]{}, y[2]{}, finalGr = 0, atAttack = 0, atRelease = 0;
    for (size_t i = 0; i < step.data.size(); ++i) {
        x[0] = step.data[i];
        c.process(x, y);
        const double time = double(i) / fs;
        if (std::abs(time - (0.5 + 0.02 + 0.002)) < 0.5 / fs) atAttack = c.grDb();
        if (std::abs(time - 1.499) < 0.5 / fs) finalGr = c.grDb();
        if (std::abs(time - (1.5 + 0.2)) < 0.5 / fs) atRelease = c.grDb();
    }
    t::atMost("compressor: gain reduction at the attack time is at least 80 % of the final value", atAttack, 0.8 * finalGr, " dB");
    t::near("compressor: gain reduction before the release (final value) matches the static curve (-12 dB)", finalGr, -12.0, 0.8, " dB");
    t::atLeast("compressor: after the release time most of the reduction has recovered", atRelease, finalGr * 0.5, " dB");
    // stereo link: a loud left channel reduces both channels by the same amount
    Compressor link;
    link.prepare(fs, 2);
    link.set(s);
    link.reset();
    double a[2], b[2], lastL = 0, lastR = 0;
    for (int i = 0; i < int(fs); ++i) {
        a[0] = std::sin(2 * t::Pi * 440 * i / fs) * t::lin(-6);
        a[1] = std::sin(2 * t::Pi * 660 * i / fs) * t::lin(-40);
        link.process(a, b);
        if (i > int(fs) - 2000) {
            lastL = std::max(lastL, std::abs(b[0]) / t::lin(-6));
            lastR = std::max(lastR, std::abs(b[1]) / t::lin(-40));
        }
    }
    t::near("compressor: stereo link applies the same reduction to both channels", t::db(lastL), t::db(lastR), 0.2, " dB");
}

void levelerTests() {
    const double fs = 48000;
    Settings s;
    s.levelerOn = true;
    s.levelerTarget = -24;
    s.levelerRatio = 1.5;
    auto settle = [&](double toneDb, double seconds, bool freeze) {
        Leveler l;
        l.prepare(fs, 1);
        l.set(s);
        l.reset();
        Signal sig(fs, 1);
        sig.tone(500, toneDb, seconds);
        double x[2]{}, y[2]{};
        for (float v : sig.data) {
            x[0] = v;
            l.process(x, y, freeze);
        }
        return l.gainDb();
    };
    t::near("leveler: -12 dB tone is lowered toward the reference ((1/1.5-1)*12 = -4 dB)", settle(-12, 12, false), -4.0, 0.35, " dB");
    t::near("leveler: -36 dB tone is raised toward the reference (+4 dB)", settle(-36, 14, false), 4.0, 0.5, " dB");
    t::near("leveler: the correction is limited to -6 dB", settle(0, 12, false), -6.0, 0.05, " dB");
    t::near("leveler: frozen while the gate is closed", settle(-12, 8, true), 0.0, 1e-9, " dB");
    Leveler l;
    l.prepare(fs, 1);
    Settings off = s;
    off.levelerOn = false;
    l.set(off);
    l.reset();
    double x[2]{0.1, 0}, y[2]{};
    l.process(x, y, false);
    t::near("leveler: off passes the signal unchanged", y[0], 0.1, 1e-12);
}

// ---- limiter and True Peak -------------------------------------------------------------------------
struct Limited {
    std::vector<float> out;
    double maxOvershootDb = -1000;  // output true peak minus the ceiling
    double maxGrDb = 0;
};
Limited runLimiter(const std::vector<float>& in, double fs, int channels, double ceilingDb, double releaseMs = 80) {
    Limiter lim;
    lim.prepare(fs, channels);
    lim.set(true, ceilingDb, releaseMs);
    lim.reset();
    const size_t frames = in.size() / size_t(channels), latency = size_t(lim.latency());
    Limited r;
    r.out.assign(in.size(), 0.f);
    double x[2]{}, y[2]{};
    for (size_t i = 0; i < frames + latency; ++i) {
        for (int c = 0; c < channels; ++c) x[c] = i < frames ? double(in[i * size_t(channels) + size_t(c)]) : 0.0;
        lim.process(x, y);
        r.maxGrDb = std::min(r.maxGrDb, lim.grDb());
        if (i >= latency)
            for (int c = 0; c < channels; ++c) r.out[(i - latency) * size_t(channels) + size_t(c)] = float(y[c]);
    }
    r.maxOvershootDb = peakDb(r.out, channels) - ceilingDb;
    return r;
}

void limiterTests() {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        const std::string at = " @" + std::to_string(int(fs)) + " Hz";
        // the reported latency is the real delay
        {
            Limiter lim;
            lim.prepare(fs, 1);
            lim.set(true, -1, 80);
            lim.reset();
            double x[2]{0, 0}, y[2]{};
            int where = -1;
            for (int i = 0; i < lim.latency() + 50; ++i) {
                x[0] = i == 10 ? 0.25 : 0.0;
                lim.process(x, y);
                if (std::abs(y[0]) > 0.2 && where < 0) where = i;
            }
            t::check(where == 10 + lim.latency(), "limiter: latency() equals the measured delay" + at, std::to_string(lim.latency()) + " samples, impulse out at +" + std::to_string(where - 10));
            t::atMost("limiter: latency in milliseconds (limit 10 ms)" + at, lim.latency() * 1000.0 / fs, 10.0, " ms");
        }
        // below the ceiling the output is the delayed input, bit for bit
        {
            const auto in = voice::make(fs, 5, 2, 7, -26);
            auto r = runLimiter(in, fs, 2, -1);
            double diff = 0;
            for (size_t i = 0; i < in.size(); ++i) diff = std::max(diff, std::abs(double(r.out[i]) - double(in[i])));
            t::near("limiter: transparent below the ceiling" + at, diff, 0.0, 1e-7);
            t::near("limiter: no gain reduction below the ceiling" + at, r.maxGrDb, 0.0, 1e-9, " dB");
        }
        // True Peak never above the ceiling: voice pushed 6..24 dB over, plus adversarial signals
        double worst = -1000;
        auto check = [&](const std::string& name, std::vector<float> in, int channels, double ceilingDb) {
            auto r = runLimiter(in, fs, channels, ceilingDb);
            worst = std::max(worst, r.maxOvershootDb);
            t::atMost("limiter: true peak vs ceiling, " + name + at, r.maxOvershootDb, 0.0, " dB over");
        };
        for (double over : {6.0, 12.0, 24.0}) {
            auto v = voice::make(fs, 8, 2, 11 + uint64_t(over), over);
            check("voice +" + std::to_string(int(over)) + " dB stereo", v, 2, -1);
        }
        check("voice +12 dB mono -3 dBTP", voice::make(fs, 8, 1, 5, 12), 1, -3);
        {  // high-frequency sines with phase steps at full scale (up to 0.43 of the sample rate)
            for (double hz : {9000.0, 15000.0, 18000.0, 19500.0}) {
                if (hz > fs * 0.43) continue;
                Signal s(fs, 1);
                s.tone(hz, 0, 0.3);
                s.tone(hz, 0, 0.3, 1.234);
                s.tone(hz, -6, 0.2, 2.1);
                s.tone(hz, 0, 0.3, 0.3);
                check("sine " + std::to_string(int(hz)) + " Hz with phase steps", s.data, 1, -1);
            }
        }
        {  // clipped square
            Signal sq(fs, 1);
            sq.tone(2000, 12, 0.5);
            for (auto& v : sq.data) v = std::clamp(v, -1.f, 1.f);
            check("clipped 2 kHz sine", sq.data, 1, -1);
        }
        {  // band-limited noise: white noise through a steep low-pass at 0.42 of the sample rate (what a recording's anti-alias filter leaves)
            voice::Rng rng(99);
            const size_t n = size_t(fs * 3);
            std::vector<double> white(n);
            for (auto& v : white) v = rng.next() * 2 - 1;
            const int taps = 401, mid = taps / 2;
            std::vector<double> lp(taps);
            double sum = 0;
            for (int i = 0; i < taps; ++i) {
                const double tt = i - mid, w = 0.5 - 0.5 * std::cos(2 * t::Pi * (i + 0.5) / taps);
                lp[size_t(i)] = (tt == 0 ? 0.84 : std::sin(t::Pi * 0.84 * tt) / (t::Pi * tt)) * w;
                sum += lp[size_t(i)];
            }
            std::vector<float> noise(n * 2);
            double peak = 0;
            std::vector<double> filtered(n, 0.0);
            for (size_t i = 0; i < n; ++i) {
                double acc = 0;
                for (int k = 0; k < taps; ++k) {
                    const long j = long(i) + k - mid;
                    if (j >= 0 && j < long(n)) acc += lp[size_t(k)] / sum * white[size_t(j)];
                }
                filtered[i] = acc;
                peak = std::max(peak, std::abs(acc));
            }
            for (size_t i = 0; i < n; ++i) noise[i * 2] = noise[i * 2 + 1] = float(filtered[i] / peak * 1.6);
            check("band-limited noise (0.42 fs) at +4 dBFS", noise, 2, -1);
        }
        {  // isolated single-sample impulses of both polarities
            std::vector<float> imp(int(fs * 0.3), 0.f);
            imp[3000] = 3.0f;
            imp[3001] = -3.0f;
            imp[9000] = 5.0f;
            check("sample impulses", imp, 1, -1);
        }
        {  // informational only: a full-scale signal at exactly the Nyquist rate has no unique reconstruction
            std::vector<float> alt(int(fs * 0.5) * 2);
            for (size_t i = 0; i < alt.size() / 2; ++i) alt[i * 2] = alt[i * 2 + 1] = (i % 2 ? 1.f : -1.f);
            const auto r = runLimiter(alt, fs, 2, -1);
            std::printf("      info (not a pass criterion): Nyquist-rate alternation ends %.2f dB over the ceiling by the 16x reference%s\n", r.maxOvershootDb, at.c_str());
        }
        std::printf("      worst overshoot over all limiter signals%s: %.4f dB\n", at.c_str(), worst);
    }
    // gain reduction recovers: after a very loud burst, quiet audio returns to unity
    {
        const double fs = 48000;
        Signal s(fs, 1);
        s.tone(300, 6, 0.1);
        s.tone(300, -30, 3);
        Limiter lim;
        lim.prepare(fs, 1);
        lim.set(true, -1, 80);
        lim.reset();
        double x[2]{}, y[2]{};
        for (float v : s.data) {
            x[0] = v;
            lim.process(x, y);
        }
        t::near("limiter: gain reduction fully recovers after a burst", lim.grDb(), 0.0, 0.01, " dB");
    }
}

// ---- chain, bypass, abnormal input -----------------------------------------------------------------
void chainTests() {
    const double fs = 48000;
    {
        Chain c;
        c.prepare(fs, 2);
        t::check(c.latency() > 0, "chain: latency is positive", std::to_string(c.latency()));
        t::atMost("chain: total latency in milliseconds (limit 10 ms)", c.latency() * 1000.0 / fs, 10.0, " ms");
        std::printf("      chain latency: %d samples = %.3f ms at 48 kHz\n", c.latency(), c.latency() * 1000.0 / fs);
        for (double rate : {44100.0, 96000.0, 192000.0}) {
            Chain d;
            d.prepare(rate, 2);
            t::atMost("chain: total latency in milliseconds @" + std::to_string(int(rate)) + " Hz", d.latency() * 1000.0 / rate, 10.0, " ms");
        }
    }
    // determinism: two instances give identical output
    {
        const auto in = voice::make(fs, 10, 2, 21, 0);
        Settings s;
        Chain a, b;
        a.prepare(fs, 2);
        b.prepare(fs, 2);
        a.set(s, 3.0);
        b.set(s, 3.0);
        a.reset();
        b.reset();
        const auto oa = runChain(a, in, 2), ob = runChain(b, in, 2);
        t::check(oa == ob, "chain: identical input and settings give identical output");
    }
    // abnormal input
    {
        Chain c;
        c.prepare(fs, 2);
        Settings s;
        c.set(s, 6.0);
        c.reset();
        double x[2], y[2];
        bool finite = true;
        double biggest = 0;
        const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
        const double nasty[]{nan, inf, -inf, 1e30, -1e30, 1e300, 0.0, 1e-320, 0.5};
        voice::Rng rng(3);
        for (int i = 0; i < 200000; ++i) {
            x[0] = nasty[size_t(rng.next() * 9)];
            x[1] = (i % 3 == 0) ? nasty[size_t(rng.next() * 9)] : std::sin(i * 0.05) * 0.5;
            c.tick(x, y);
            finite &= std::isfinite(y[0]) && std::isfinite(y[1]);
            biggest = std::max({biggest, std::abs(y[0]), std::abs(y[1])});
        }
        t::check(finite, "chain: NaN, infinity and huge values never produce a non-finite output");
        for (int i = 0; i < 100000; ++i) {
            x[0] = x[1] = 0.1 * std::sin(i * 0.03);
            c.tick(x, y);
            finite &= std::isfinite(y[0]) && std::isfinite(y[1]);
        }
        t::check(finite, "chain: recovers after abnormal input");
        t::atMost("chain: the output never runs away after abnormal input (peak)", biggest, 1e4 * 4);
        // after the abnormal block, normal audio returns to normal levels
        double peak = 0;
        for (int i = 0; i < 48000; ++i) {
            x[0] = x[1] = 0.1 * std::sin(i * 0.03);
            c.tick(x, y);
            if (i > 40000) peak = std::max(peak, std::abs(y[0]));
        }
        t::atMost("chain: normal level again after abnormal input", peak, 0.9);
    }
    // bypass keeps the delay and passes the input through
    {
        Chain c;
        c.prepare(fs, 2);
        Settings s;
        s.bypass = true;
        c.set(s, 0);
        c.reset();
        const auto in = voice::make(fs, 3, 2, 8, 0);
        const auto out = runChain(c, in, 2);
        double diff = 0;
        for (size_t i = 0; i < in.size(); ++i) diff = std::max(diff, std::abs(double(out[i]) - double(in[i])));
        t::near("chain: bypass is the input, latency compensated", diff, 0.0, 1e-7);
    }
    // silence in, silence out (no idle noise, no denormals-driven garbage)
    {
        Chain c;
        c.prepare(fs, 2);
        Settings s;
        c.set(s, 10.0);
        c.reset();
        double x[2]{0, 0}, y[2], peak = 0;
        for (int i = 0; i < 96000; ++i) {
            c.tick(x, y);
            peak = std::max({peak, std::abs(y[0]), std::abs(y[1])});
        }
        t::near("chain: silence stays silent", peak, 0.0, 1e-15);
    }
}

// ---- analysis: prediction vs the real output -------------------------------------------------------
struct Case {
    const char* name;
    double fs;
    int channels;
    uint64_t seed;
    double materialDb, target, ceiling;
    bool dualMono = false;
    bool limiterOff = false;
    bool leveler = false;
};

struct Outcome {
    AnalysisResult analysis;
    double measuredLufs = SilenceDb, measuredPeakDb = SilenceDb;
};

Outcome predictAndMeasure(const Case& k, const std::vector<float>& material, double seconds) {
    (void)seconds;
    Settings s;
    s.target = k.target;
    s.ceiling = k.ceiling;
    s.dualMono = k.dualMono;
    s.limiterOn = !k.limiterOff;
    s.levelerOn = k.leveler;
    ChunkedPcm pcm;
    pcm.configure(k.channels);
    pcm.append(material.data(), material.size() / size_t(k.channels));
    Outcome o;
    o.analysis = analyzeFile(s, k.channels, k.fs, pcm);
    Chain chain;
    chain.prepare(k.fs, k.channels);
    chain.set(s, o.analysis.gainDb);
    chain.reset();
    const auto out = runChain(chain, material, k.channels);
    o.measuredLufs = integratedOf(out, k.fs, k.channels, k.dualMono);
    o.measuredPeakDb = peakDb(out, k.channels);
    return o;
}

void predictionTests() {
    const Case cases[]{
        {"voice stereo 48k -23 LUFS", 48000, 2, 1, 0, -23, -1},
        {"voice stereo 48k -16 LUFS", 48000, 2, 2, 0, -16, -1},
        {"voice stereo 44.1k -18 LUFS", 44100, 2, 3, -6, -18, -1},
        {"voice stereo 96k -20 LUFS", 96000, 2, 4, 3, -20, -2},
        {"voice mono 48k -23 LUFS (1ch)", 48000, 1, 5, 0, -23, -1},
        {"voice mono 48k -23 LUFS (dual mono)", 48000, 1, 5, 0, -23, -1, true},
        {"quiet voice -30 dB, target -16 (limiter works hard)", 48000, 2, 6, -30, -16, -1},
        {"loud voice +6 dB, target -30", 48000, 2, 7, 6, -30, -1},
        {"limiter off, target -23", 48000, 2, 8, 0, -23, -1, false, true},
        {"leveler on, target -20", 48000, 2, 9, 0, -20, -1, false, false, true},
        {"ceiling -3 dBTP, target -14 (limited)", 48000, 2, 10, 0, -14, -3},
    };
    double worstError = 0, worstPeak = -1000;
    for (const auto& k : cases) {
        const auto material = voice::make(k.fs, 40, k.channels, k.seed, k.materialDb);
        const auto o = predictAndMeasure(k, material, 40);
        std::printf("      %s: G=%+.2f dB, predicted %.3f, measured %.3f LUFS, TP %.2f dBTP (ceiling %.1f), max GR %.2f dB, passes %d\n", k.name, o.analysis.gainDb,
                    o.analysis.predictedLufs, o.measuredLufs, o.measuredPeakDb, k.ceiling, o.analysis.maxGrDb, o.analysis.evaluations);
        t::check(o.analysis.ok, std::string("analysis succeeds: ") + k.name, o.analysis.error);
        const double error = o.analysis.predictedLufs - o.measuredLufs;
        worstError = std::max(worstError, std::abs(error));
        t::atMost(std::string("prediction vs measured output: ") + k.name, std::abs(error), 0.1, " LU");
        if (!k.limiterOff) {
            t::atMost(std::string("measured true peak <= ceiling: ") + k.name, o.measuredPeakDb, k.ceiling, " dBTP");
            worstPeak = std::max(worstPeak, o.measuredPeakDb - k.ceiling);
        }
        if (!o.analysis.unreached) t::atMost(std::string("target reached: ") + k.name, std::abs(o.analysis.predictedLufs - k.target), 0.1, " LU");
    }
    std::printf("      worst |prediction - measured| = %.4f LU; worst true peak minus ceiling = %.3f dB\n", worstError, worstPeak);
    // a target that cannot be reached is reported, never faked
    {
        Settings s;
        s.target = -3;
        s.ceiling = -1;
        const auto material = voice::make(48000, 30, 2, 12, 0);
        ChunkedPcm pcm;
        pcm.configure(2);
        pcm.append(material.data(), material.size() / 2);
        const auto r = analyzeFile(s, 2, 48000, pcm);
        t::check(r.ok && r.unreached && (r.warnings & WarnUnreached), "target -3 LUFS with a -1 dBTP ceiling is reported as unreached", t::num(r.predictedLufs) + " LUFS, error " + t::num(r.errorLu));
        t::atMost("unreached: the true peak ceiling is still kept", r.truePeakDb, s.ceiling + 0.01, " dBTP");
    }
    // silence and too-quiet material
    {
        Settings s;
        ChunkedPcm pcm;
        pcm.configure(2);
        std::vector<float> zeros(2 * 48000 * 5, 0.f);
        pcm.append(zeros.data(), zeros.size() / 2);
        const auto r = analyzeFile(s, 2, 48000, pcm);
        t::check(!r.ok && !r.error.empty(), "analysis of silence fails with a message", r.error);
    }
    // short material warns
    {
        Settings s;
        const auto material = voice::make(48000, 2.0, 2, 13, 0);
        ChunkedPcm pcm;
        pcm.configure(2);
        pcm.append(material.data(), material.size() / 2);
        const auto r = analyzeFile(s, 2, 48000, pcm);
        t::check(r.ok && (r.warnings & WarnShort), "material under 3 s carries the short-material warning");
    }
}

// ---- playback learning -----------------------------------------------------------------------------
bool waitFor(Learner& l, Learner::State s, int ms = 60000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (l.state() == s) return true;
        if (l.state() == Learner::Failed || l.state() == Learner::Idle) return l.state() == s;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

struct LearnRun {
    bool sent = false;
    Learner::Result result;
    Learner::Status status;
};

// Plays `material` through a chain in blocks (schedule cycles), feeding the learner like the plug-in does.
LearnRun learnByPlaying(const Case& k, const std::vector<float>& material, const std::vector<int>& schedule, bool jumpBack, bool fastLearner = false, double stopAt = 0) {
    Settings s;
    s.target = k.target;
    s.ceiling = k.ceiling;
    s.dualMono = k.dualMono;
    Chain chain;
    chain.prepare(k.fs, k.channels);
    chain.set(s, 0);
    chain.reset();
    Learner learner;
    learner.arm([s] { return s; }, fastLearner ? 1u << 12 : 1u << 20);
    const size_t frames = material.size() / size_t(k.channels);
    std::vector<float> staged(8192 * 2);
    double x[2]{}, y[2]{}, p[2]{};
    size_t pos = 0;
    int64_t timeline = 0;
    bool jumped = false;
    size_t block = 0, total = frames;
    if (stopAt > 0) total = std::min(total, size_t(stopAt * k.fs));
    const uint64_t hash = preStageHash(s, k.channels);
    while (pos < total) {
        const int n = int(std::min<size_t>(size_t(schedule[block++ % schedule.size()]), total - pos));
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < k.channels; ++c) x[c] = double(material[(pos + size_t(i)) * size_t(k.channels) + size_t(c)]);
            chain.tick(x, y, p);
            for (int c = 0; c < k.channels; ++c) staged[size_t(i) * size_t(k.channels) + size_t(c)] = float(p[c]);
        }
        if (jumpBack && !jumped && pos >= frames / 2) {
            timeline -= int64_t(frames / 4);
            jumped = true;
        }
        learner.block(staged.data(), uint32_t(n), k.channels, k.fs, true, true, timeline, hash);
        pos += size_t(n);
        timeline += n;
    }
    // the pre-stage still holds a few ms of delayed audio: flush it like further playback would
    std::fill(x, x + 2, 0.0);
    for (int i = 0; i < chain.pre().latency(); ++i) {
        chain.tick(x, y, p);
        for (int c = 0; c < k.channels; ++c) staged[size_t(i) * size_t(k.channels) + size_t(c)] = float(p[c]);
    }
    learner.block(staged.data(), uint32_t(chain.pre().latency()), k.channels, k.fs, true, true, timeline, hash);
    LearnRun run;
    learner.finish();
    waitFor(learner, Learner::Done, 120000);
    run.status = learner.status();
    run.sent = learner.result(run.result);
    return run;
}

void learnerTests() {
    const Case k{"learning", 48000, 2, 31, 0, -23, -1};
    const auto material = voice::make(k.fs, 40, k.channels, k.seed, k.materialDb);
    const Outcome file = predictAndMeasure(k, material, 40);
    // different block sizes give the same result as the file analysis and as each other
    std::vector<std::vector<int>> schedules{{64}, {480}, {1, 31, 127, 511, 2048, 64, 1000, 3, 4096}, {4096}};
    std::vector<double> gains;
    for (const auto& sc : schedules) {
        const auto run = learnByPlaying(k, material, sc, false);
        t::check(run.sent && run.result.analysis.ok, "learner: result is produced (block " + std::to_string(sc[0]) + "...)", run.result.analysis.error);
        gains.push_back(run.result.analysis.gainDb);
        t::check(run.result.flags == 0, "learner: a clean pass carries no warning flag", "flags=" + std::to_string(run.result.flags));
        // The learner sees pre-stage output that includes the 3 ms gate delay and keeps the tail; the file analysis drops
        // the delay. Both describe the same audio, so the gains agree closely.
        t::near("learner: gain agrees with the file analysis (block " + std::to_string(sc[0]) + "...)", run.result.analysis.gainDb, file.analysis.gainDb, 0.05, " dB");
        // and it predicts the real output of the chain within 0.1 LU
        Settings s;
        s.target = k.target;
        s.ceiling = k.ceiling;
        Chain chain;
        chain.prepare(k.fs, k.channels);
        chain.set(s, run.result.analysis.gainDb);
        chain.reset();
        const auto out = runChain(chain, material, k.channels);
        const double measured = integratedOf(out, k.fs, k.channels);
        t::atMost("learner: prediction vs measured output (block " + std::to_string(sc[0]) + "...)", std::abs(run.result.analysis.predictedLufs - measured), 0.1, " LU");
        std::printf("      learner block %d..: G=%+.3f dB predicted %.3f measured %.3f LUFS\n", sc[0], run.result.analysis.gainDb, run.result.analysis.predictedLufs, measured);
    }
    double spread = 0;
    for (double g : gains) spread = std::max(spread, std::abs(g - gains[0]));
    t::atMost("learner: the block size changes the gain by at most 1e-6 dB", spread, 1e-6, " dB");
    // a rewind in the middle is detected
    {
        const auto run = learnByPlaying(k, material, {512}, true);
        t::check((run.result.flags & LfBackJump) != 0, "learner: a position jump backwards is flagged", "flags=" + std::to_string(run.result.flags));
    }
    // learning that began away from the start is flagged
    {
        Settings s;
        Learner l;
        l.arm([s] { return s; });
        std::vector<float> block(512 * 2, 0.1f);
        l.block(block.data(), 512, 2, 48000, true, true, 48000 * 20, preStageHash(s, 2));
        l.block(block.data(), 512, 2, 48000, true, true, 48000 * 20 + 512, preStageHash(s, 2));
        l.finish();
        waitFor(l, Learner::Failed, 5000);
        t::check((l.status().flags & LfStartedMidway) != 0, "learner: starting away from the beginning is flagged");
    }
    // a forward jump is detected
    {
        Settings s;
        Learner l;
        l.arm([s] { return s; });
        std::vector<float> block(512 * 2, 0.1f);
        const auto h = preStageHash(s, 2);
        l.block(block.data(), 512, 2, 48000, true, true, 0, h);
        l.block(block.data(), 512, 2, 48000, true, true, 512, h);
        l.block(block.data(), 512, 2, 48000, true, true, 512 * 40, h);
        l.finish();
        waitFor(l, Learner::Failed, 5000);
        t::check((l.status().flags & LfForwardJump) != 0, "learner: a position jump forwards is flagged");
    }
    // changing the settings (or the rate) while learning is flagged
    {
        Settings s;
        Learner l;
        l.arm([s] { return s; });
        std::vector<float> block(512 * 2, 0.1f);
        const auto h = preStageHash(s, 2);
        l.block(block.data(), 512, 2, 48000, true, true, 0, h);
        l.block(block.data(), 512, 2, 48000, true, true, 512, h + 1);
        l.block(block.data(), 512, 2, 44100, true, true, 1024, h);
        l.finish();
        waitFor(l, Learner::Failed, 5000);
        t::check((l.status().flags & LfSettingsChanged) != 0, "learner: a settings change while learning is flagged");
        t::check((l.status().flags & LfRateChanged) != 0, "learner: a sample rate change while learning is flagged");
    }
    // no transport information is flagged; a stopped transport captures nothing
    {
        Settings s;
        Learner l;
        l.arm([s] { return s; });
        std::vector<float> block(512 * 2, 0.1f);
        const auto h = preStageHash(s, 2);
        l.block(block.data(), 512, 2, 48000, false, true, 0, h);  // stopped: not captured, learning stays armed
        t::check(l.state() == Learner::Armed, "learner: a stopped transport does not start the capture");
        l.block(block.data(), 512, 2, 48000, true, false, 0, h);
        t::check((l.status().flags & LfNoTransport) != 0, "learner: missing position information is flagged");
    }
    // FIFO overflow is detected and counted (a tiny FIFO and a stalled reader)
    {
        Settings s;
        Learner l;
        l.arm([s] { return s; }, 1u << 12);
        std::vector<float> block(2048 * 2, 0.1f);
        const auto h = preStageHash(s, 2);
        for (int i = 0; i < 200; ++i) l.block(block.data(), 2048, 2, 48000, true, true, int64_t(i) * 2048, h);  // no pause: the worker cannot keep up
        const auto st = l.status();
        t::check((st.flags & LfDropped) != 0 && st.droppedFrames > 0, "learner: a FIFO overflow is detected", "dropped " + std::to_string(st.droppedFrames) + " frames");
        l.cancel();
    }
    // nothing captured -> a clear failure
    {
        Settings s;
        Learner l;
        l.arm([s] { return s; });
        l.finish();
        waitFor(l, Learner::Failed, 5000);
        t::check(l.state() == Learner::Failed && !l.status().message.empty(), "learner: finishing without audio fails with a message", l.status().message);
    }
    // stop-ends-learning
    {
        Settings s;
        Learner l;
        l.setAutoStop(true);
        l.arm([s] { return s; });
        std::vector<float> block(480 * 2, 0.05f);
        const auto h = preStageHash(s, 2);
        for (int i = 0; i < 10; ++i) l.block(block.data(), 480, 2, 48000, true, true, i * 480, h);
        l.block(block.data(), 480, 2, 48000, false, true, 10 * 480, h);
        for (int i = 0; i < 400 && l.state() == Learner::Capturing; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        t::check(l.state() != Learner::Capturing && l.state() != Learner::Armed, "learner: the transport stopping ends the learning when that option is on");
    }
    // starting again after a result works and drops the old result
    {
        Settings s;
        Learner l;
        l.arm([s] { return s; });
        l.finish();
        waitFor(l, Learner::Failed, 5000);
        t::check(l.arm([s] { return s; }) && l.state() == Learner::Armed, "learner: can be armed again after a result");
        Learner::Result r;
        t::check(!l.result(r), "learner: arming again clears the old result");
    }
}

// ---- real-time safety ------------------------------------------------------------------------------
void realtimeTests() {
    const double fs = 48000;
    Chain chain;
    chain.prepare(fs, 2);
    Settings s;
    s.levelerOn = true;
    chain.set(s, 4.0);
    chain.reset();
    Learner learner;
    learner.arm([s] { return s; });
    const auto material = voice::make(fs, 6, 2, 41, 6);
    std::vector<float> staged(512 * 2);
    double x[2], y[2], p[2];
    const uint64_t hash = preStageHash(s, 2);
    long allocations = -1;
    {
        g_allocations = 0;
        g_armed = true;
        for (size_t pos = 0; pos + 512 <= material.size() / 2; pos += 512) {
            for (int i = 0; i < 512; ++i) {
                x[0] = double(material[(pos + size_t(i)) * 2]);
                x[1] = double(material[(pos + size_t(i)) * 2 + 1]);
                chain.tick(x, y, p);
                staged[size_t(i) * 2] = float(p[0]);
                staged[size_t(i) * 2 + 1] = float(p[1]);
            }
            learner.block(staged.data(), 512, 2, fs, true, true, int64_t(pos), hash);
            chain.set(s, 4.0);
        }
        g_armed = false;
        allocations = g_allocations.load();
    }
    t::check(allocations == 0, "real time: no memory allocation while processing and capturing", std::to_string(allocations) + " allocations");
    learner.cancel();
    learner.release();
}

// ---- speed -----------------------------------------------------------------------------------------
void speedTests() {
    for (int channels : {1, 2}) {
        const double fs = 48000;
        const auto in = voice::make(fs, 30, channels, 51, 6);
        Chain chain;
        chain.prepare(fs, channels);
        Settings s;
        chain.set(s, 4.0);
        chain.reset();
        double x[2]{}, y[2]{};
        const auto t0 = std::chrono::steady_clock::now();
        double sink = 0;
        for (size_t i = 0; i < in.size() / size_t(channels); ++i) {
            for (int c = 0; c < channels; ++c) x[c] = double(in[i * size_t(channels) + size_t(c)]);
            chain.tick(x, y);
            sink += y[0];
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("      speed: %d ch, 30 s of audio in %.3f s => %.2f %% of one core in real time (%.0fx real time) [%g]\n", channels, seconds, 100.0 * seconds / 30.0,
                    30.0 / seconds, sink * 0);
        t::atMost("speed: one stereo/mono instance uses less than 10 % of one core", 100.0 * seconds / 30.0, 10.0, " %");
    }
}
}  // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    gateTests();
    compressorTests();
    levelerTests();
    limiterTests();
    chainTests();
    predictionTests();
    if (!quick) learnerTests();
    realtimeTests();
    speedTests();
    return t::finish("dsp_tests");
}
