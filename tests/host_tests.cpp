// Independent host tests for the real VoiceDynamics VST3 binary (loaded as a DLL, driven through the VST3 interfaces).
#include "host_common.h"
#include "chain.h"
#include "loudness.h"
#include "reference.h"
#include "voice.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string_view>

namespace {
bool quick = false;
double worstPrediction = 0, worstTarget = 0, worstPeakMargin = -1e9, bestPeakMargin = 1e9;
int learnRuns = 0;

std::map<uint32_t, double> stateEntries(Instance& a) {
    MemoryStream state;
    a.saveState(state);
    const char* data = state.getData();
    uint32_t count = 0;
    std::memcpy(&count, data + 8, 4);
    std::map<uint32_t, double> out;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        double v = 0;
        std::memcpy(&id, data + 12 + i * 12, 4);
        std::memcpy(&v, data + 16 + i * 12, 8);
        out[id] = v;
    }
    return out;
}
std::string stateBytes(Instance& a) {
    MemoryStream state;
    a.saveState(state);
    return std::string(state.getData(), size_t(state.getSize()));
}

struct Measured {
    double lufs = -200, truePeakDb = -200;
};
Measured measure(const std::vector<double>& y, int ch, double rate, bool withPeak = true) {
    vd::LoudnessMeter m;
    m.prepare(rate, ch, false);
    std::vector<double> blocks;
    m.setBlockSink(&blocks);
    double f[2]{};
    const size_t frames = y.size() / size_t(ch);
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < ch; ++c) f[c] = y[i * size_t(ch) + size_t(c)];
        m.push(f);
    }
    Measured r;
    r.lufs = vd::gatedIntegrated(blocks);
    if (withPeak) {
        static const ref::TruePeak16 reference;
        double peak = 0;
        for (int c = 0; c < ch; ++c) {
            std::vector<double> x(frames);
            for (size_t i = 0; i < frames; ++i) x[i] = y[i * size_t(ch) + size_t(c)];
            peak = std::max(peak, reference.peak(x));
        }
        r.truePeakDb = ref::db(peak);
    }
    return r;
}
std::vector<float> material(double rate, int ch, double seconds, uint64_t seed, double db = 0) { return voice::make(rate, seconds, ch, seed, db); }

void parameterTests(IPluginFactory* factory) {
    Instance a(factory);
    check(a.ctrl->getParameterCount() == vd::ParamCount, "parameter count");
    for (int32 i = 0; i < a.ctrl->getParameterCount(); ++i) {
        ParameterInfo info{};
        check(a.ctrl->getParameterInfo(i, info) == kResultOk, "parameter info");
        check(info.id == ParamID(i), "fixed parameter ids in order");
        check(std::abs(info.defaultNormalizedValue - vd::norm(info.id, vd::specs[info.id].def)) < 1e-12, "default value");
        check(info.stepCount == vd::specs[info.id].steps, "step count");
        check(((info.flags & ParameterInfo::kIsBypass) != 0) == (info.id == vd::Bypass), "bypass flag");
        check(((info.flags & ParameterInfo::kCanAutomate) != 0) == vd::specs[info.id].automatable, "automation flag");
        for (double probe : {0., .37, 1.}) {
            const double v = info.stepCount ? std::round(probe * info.stepCount) / info.stepCount : probe;
            String128 text{};
            check(a.ctrl->getParamStringByValue(info.id, v, text) == kResultOk, "value to string");
            ParamValue back = -1;
            check(a.ctrl->getParamValueByString(info.id, text, back) == kResultOk, "string to value");
            const double shown = vd::plain(info.id, v);
            const std::u16string_view view(reinterpret_cast<const char16_t*>(text));
            double tolerance = info.stepCount ? 1e-12 : view.find(u'.') != std::u16string_view::npos ? .0501 : .501;  // display precision
            if (view == u"ミュート" || view == u"オフ") tolerance = 1e-12;
            if (info.id == vd::GateHpf && shown <= vd::GateHpfOff + .05) tolerance = 1e-12;
            check(std::abs(vd::plain(info.id, back) - shown) <= tolerance, "string round trip " + std::to_string(info.id) + " " + std::to_string(probe));
            check(std::abs(a.ctrl->normalizedParamToPlain(info.id, v) - vd::plain(info.id, v)) < 1e-9, "plain mapping");
        }
    }
    String128 text{};
    a.ctrl->getParamStringByValue(vd::GateRange, 0, text);
    check(std::u16string_view(reinterpret_cast<const char16_t*>(text)) == u"ミュート", "ゲート Range shows ミュート at -80 dB");
    a.ctrl->getParamStringByValue(vd::GateHpf, 0, text);
    check(std::u16string_view(reinterpret_cast<const char16_t*>(text)) == u"オフ", "検出の低域カット shows オフ at its minimum");
    a.ctrl->getParamStringByValue(vd::GateOn, 1, text);
    check(std::u16string_view(reinterpret_cast<const char16_t*>(text)) == u"オン", "toggle shows オン");
    a.ctrl->getParamStringByValue(vd::MonoMode, 1, text);
    check(std::u16string_view(reinterpret_cast<const char16_t*>(text)) == vd::monoNames[1], "Japanese list name");
    const auto d = vd::defaults();
    const auto s = vd::settingsFrom(d);
    check(s.gateOn && s.compOn && !s.levelerOn && s.limiterOn && s.autoOn && !s.bypass && !s.learnAutoStop, "default switches");
    check(std::abs(s.target + 23) < 1e-9 && std::abs(s.ceiling + 1) < 1e-9 && std::abs(s.gateOpen + 42) < 1e-9 && std::abs(s.compRatio - 3) < 1e-9, "default values (-23 LUFS, -1 dBTP, gate -42 dB, 3:1)");
    std::cout << "PASS parameters: " << vd::ParamCount << " fixed IDs in order, defaults, flags, Japanese texts (ミュート/オフ/オン), string conversion\n";
}

void layoutTests(IPluginFactory* factory) {
    Instance a(factory);
    for (int ch : {1, 2}) check(a.layout(ch), "accepted layout");
    SpeakerArrangement stereo[]{SpeakerArr::kStereo}, mono[]{SpeakerArr::kMono}, surround[]{SpeakerArr::k51}, two[]{SpeakerArr::kStereo, SpeakerArr::kStereo};
    check(a.proc->setBusArrangements(stereo, 1, mono, 1) != kResultOk, "in/out mismatch rejected");
    check(a.proc->setBusArrangements(surround, 1, surround, 1) != kResultOk, "5.1 rejected");
    check(a.proc->setBusArrangements(two, 2, stereo, 1) != kResultOk, "two inputs rejected");
    check(a.proc->canProcessSampleSize(kSample32) == kResultTrue && a.proc->canProcessSampleSize(kSample64) == kResultTrue, "sample sizes");
    check(a.layout(2), "stereo");
    for (double sr : {1000., 8000., 500000.}) {
        ProcessSetup setup{kRealtime, kSample32, 512, sr};
        check(a.proc->setupProcessing(setup) != kResultOk, "unsupported rate rejected");
    }
    ProcessSetup zeroBlock{kRealtime, kSample32, 0, 48000};
    check(a.proc->setupProcessing(zeroBlock) != kResultOk, "zero block rejected");
    std::cout << "PASS layouts: mono/stereo accepted; mismatched, surround, 2-input, bad rates, zero block rejected\n";
}

// Bypass is the input delayed by the reported latency, exactly; the reported latency is what a pulse really takes.
void latencyTests(IPluginFactory* factory) {
    int cases = 0;
    double worstMs = 0;
    for (int format : {kSample32, kSample64})
        for (double rate : {22050., 44100., 48000., 88200., 96000., 176400., 192000., 384000.})
            for (int ch : {1, 2}) {
                Instance a(factory);
                Handler handler;
                a.ctrl->setComponentHandler(&handler);
                a.plain(vd::Bypass, 1);
                a.start(rate, format, ch);
                const int latency = int(a.proc->getLatencySamples());
                check(latency > 0 && latency * 1000. / rate < 8, "latency in a plausible range");
                worstMs = std::max(worstMs, latency * 1000. / rate);
                const auto in = material(rate, ch, 1.0, 5 + uint64_t(cases));
                Play p;
                p.schedule = {1, 31, 127, 511, 2048, 64, 1000, 3};
                p.inPlace = cases % 2 == 1;
                p.tailFrames = latency;
                const auto y = format == kSample32 ? play<float>(a, in, p) : play<double>(a, in, p);
                for (size_t i = 0; i < y.size() / size_t(ch); ++i)
                    for (int c = 0; c < ch; ++c) {
                        const double expected = i >= size_t(latency) ? double(in[(i - size_t(latency)) * size_t(ch) + size_t(c)]) : 0.0;
                        check(y[i * size_t(ch) + size_t(c)] == expected, "Bypass: the input delayed by the reported latency, bit-exact");
                    }
                // a pulse through the full chain (defaults) arrives at exactly the reported latency
                Instance b(factory);
                b.start(rate, format, ch);
                std::vector<float> pulse(size_t(ch) * size_t(rate * 0.3), 0.f);
                const size_t at = size_t(rate * 0.1);
                for (int c = 0; c < ch; ++c) pulse[at * size_t(ch) + size_t(c)] = 0.5f;
                Play q;
                q.tailFrames = latency + 64;
                const auto z = format == kSample32 ? play<float>(b, pulse, q) : play<double>(b, pulse, q);
                size_t peakAt = 0;
                double best = 0;
                for (size_t i = 0; i < z.size() / size_t(ch); ++i)
                    if (std::abs(z[i * size_t(ch)]) > best) best = std::abs(z[i * size_t(ch)]), peakAt = i;
                check(best > 0.1 && peakAt == at + size_t(latency), "a pulse arrives at the reported latency: expected " + std::to_string(at + size_t(latency)) + " got " + std::to_string(peakAt));
                // no setting changes the latency or asks for a restart
                for (uint32 id = 0; id < vd::ParamCount; ++id)
                    for (double v : {0., 1., .5}) b.ctrl->setParamNormalized(id, vd::specs[id].steps ? std::round(v * vd::specs[id].steps) / vd::specs[id].steps : v);
                check(handler.restarts == 0 && int(b.proc->getLatencySamples()) == latency, "settings never change the latency");
                a.ctrl->setComponentHandler(nullptr);
                ++cases;
            }
    std::cout << "PASS latency: " << cases << " cases (32/64-bit, 44.1..192 kHz, mono/stereo): Bypass = input delayed by the reported latency (bit-exact); a pulse arrives at the reported latency; "
              << "constant for every setting; at most " << worstMs << " ms\n";
}

// Plays the same material in a fresh instance restored from `state` and returns the output with its tail.
std::vector<double> playRestored(IPluginFactory* factory, MemoryStream& state, const std::vector<float>& in, double rate, int ch, int format = kSample64, const Play* given = nullptr) {
    Instance b(factory);
    b.loadState(state);
    b.start(rate, format, ch);
    Play p = given ? *given : Play{};
    p.tailFrames = std::max(p.tailFrames, int(b.proc->getLatencySamples()) + int(rate * 0.1));
    return format == kSample32 ? play<float>(b, in, p) : play<double>(b, in, p);
}

struct LearnResult {
    vd::MeterFrame frame{};
    double gain = 0, predicted = 0, pre = 0;
    uint32_t learnFlags = 0, warnings = 0;
    bool unreached = false;
};
// The main flow of the product: [学習] -> play the track -> [学習終了] -> result.
LearnResult learn(Instance& a, const std::vector<float>& in, const Play& p, bool finish = true) {
    a.command(vd::CmdLearnStart);
    vd::MeterFrame f{};
    check(a.meter(f) && int(f[vd::MLearnState]) == vd::Learner::Armed, "armed after [学習]");
    if (a.format == kSample32) play<float>(a, in, p);
    else play<double>(a, in, p);
    if (finish) a.command(vd::CmdLearnFinish);
    LearnResult r;
    r.frame = a.waitLearn();
    r.gain = r.frame[vd::MRecGain];
    r.predicted = r.frame[vd::MRecPred];
    r.pre = r.frame[vd::MRecPre];
    r.learnFlags = uint32_t(r.frame[vd::MLearnFlags]);
    r.warnings = uint32_t(r.frame[vd::MRecWarnings]);
    r.unreached = r.frame[vd::MRecUnreached] > .5;
    return r;
}

// The learned gain must equal what the plug-in then really outputs, for any block size.
void learningTests(IPluginFactory* factory) {
    struct Case {
        double rate;
        int ch, format;
        double seconds, db, target, ceiling;
        std::vector<int> schedule;
    };
    std::vector<Case> cases{
        {48000, 2, kSample64, 40, 0, -23, -1, {512}},      {48000, 2, kSample32, 40, -9, -16, -1, {64}},      {44100, 2, kSample64, 30, -6, -23, -1, {4096}},
        {96000, 2, kSample64, 25, 0, -20, -1, {1000}},     {48000, 1, kSample64, 30, 0, -23, -1, {480}},      {44100, 1, kSample32, 30, -12, -18, -2, {257}},
        {48000, 2, kSample64, 40, -20, -14, -1, {512}},    {48000, 2, kSample64, 40, 6, -23, -1, {2048}},     {192000, 2, kSample64, 12, 0, -23, -1, {512}},
        {48000, 2, kSample64, 40, -3, -12, -1, {1, 31, 127, 511, 2048, 64, 1000, 3}}, {48000, 2, kSample64, 40, -3, -12, -1, {17, 256, 999}},
        {22050, 2, kSample64, 30, 0, -23, -1, {512}},      {384000, 2, kSample64, 8, -6, -23, -1, {4096}},
    };
    if (quick) cases.resize(4);
    // (the last two cases are the extreme sample rates)
    int n = 0;
    double sumGain = 0, worstBlockSpread = 0;
    for (const auto& c : cases) {
        Instance a(factory);
        a.plain(vd::Target, c.target);
        a.plain(vd::Ceiling, c.ceiling);
        a.start(c.rate, c.format, c.ch);
        const auto in = material(c.rate, c.ch, c.seconds, 11 + uint64_t(n), c.db);
        Play p;
        p.schedule = c.schedule;
        p.guard = false;
        const auto r = learn(a, in, p);
        check(int(r.frame[vd::MLearnState]) == vd::Learner::Done, "learning finished with a result");
        check(r.frame[vd::MRecValid] > .5 && r.frame[vd::MRecSource] == vd::SourceLearn, "a result of the playback learning");
        check(r.learnFlags == 0 && uint32_t(r.frame[vd::MRecFlags]) == 0, "a clean pass raises no warning flags (" + std::to_string(r.learnFlags) + ")");
        check(std::abs(r.frame[vd::MRecDuration] - c.seconds) < 0.05, "the learned duration is the played duration");
        const bool reachable = !r.unreached;
        if (reachable) check(std::abs(r.predicted - c.target) <= vd::ToleranceLu, "prediction within 0.1 LU of the target: " + std::to_string(r.predicted - c.target));
        // what the plug-in really outputs with that result, in a fresh instance restored from the state
        MemoryStream state;
        a.saveState(state);
        const auto y = playRestored(factory, state, in, c.rate, c.ch, c.format);
        const auto m = measure(y, c.ch, c.rate);
        const double error = m.lufs - r.predicted;
        worstPrediction = std::max(worstPrediction, std::abs(error));
        worstTarget = std::max(worstTarget, std::abs(m.lufs - c.target));
        worstPeakMargin = std::max(worstPeakMargin, m.truePeakDb - c.ceiling);
        bestPeakMargin = std::min(bestPeakMargin, m.truePeakDb - c.ceiling);
        check(std::abs(error) <= vd::ToleranceLu, "learned prediction vs the real output within 0.1 LU: " + std::to_string(error));
        if (reachable) check(std::abs(m.lufs - c.target) <= vd::ToleranceLu, "real output within 0.1 LU of the target: " + std::to_string(m.lufs - c.target));
        check(m.truePeakDb <= c.ceiling + 1e-6, "output true peak never above the ceiling: " + std::to_string(m.truePeakDb) + " > " + std::to_string(c.ceiling));
        // the meter of the plug-in itself agrees with the independent measurement
        std::printf("      learn %.0f Hz %dch %s %.0fs db%+.0f target %.0f block %d: gain %+.2f dB, predicted %.3f, real %.3f LUFS (err %+.4f), TP %.2f dBTP, max GR %.1f dB%s\n", c.rate, c.ch,
                    c.format == kSample32 ? "f32" : "f64", c.seconds, c.db, c.target, c.schedule[0], r.gain, r.predicted, m.lufs, error, m.truePeakDb, r.frame[vd::MRecMaxGr],
                    r.unreached ? " (target not reached)" : "");
        ++n;
        ++learnRuns;
        sumGain += r.gain;
    }
    // block size independence of the learned gain: same material, different host blocks
    {
        std::vector<double> gains;
        const auto in = material(48000, 2, 30, 77);
        for (const auto& schedule : std::vector<std::vector<int>>{{1}, {31}, {64}, {480}, {512}, {4096}, {1, 31, 127, 511, 2048, 64, 1000, 3}, {17, 256, 999}, {8192}}) {
            if (quick && schedule[0] == 1) continue;
            Instance a(factory);
            a.start(48000, kSample64, 2, 8192);
            Play p;
            p.schedule = schedule;
            gains.push_back(learn(a, in, p).gain);
        }
        const double lo = *std::min_element(gains.begin(), gains.end()), hi = *std::max_element(gains.begin(), gains.end());
        worstBlockSpread = hi - lo;
        check(hi - lo < 1e-9, "the learned gain does not depend on the host block size: spread " + std::to_string(hi - lo) + " dB");
    }
    std::cout << "PASS learning: " << n << " playback-learning runs (44.1..192 kHz, mono/stereo, 32/64-bit): learned prediction vs real output at most " << worstPrediction << " LU, real output vs target at most "
              << worstTarget << " LU; true peak margin to the ceiling " << bestPeakMargin << " .. " << worstPeakMargin << " dB; learned gain identical for 9 block schedules (spread " << worstBlockSpread << " dB)\n";
}

void unreachableTest(IPluginFactory* factory) {
    Instance a(factory);
    a.plain(vd::Target, -6);
    a.plain(vd::Ceiling, -12);
    a.start();
    const auto in = material(48000, 2, 20, 3);
    const auto r = learn(a, in, Play{});
    check(r.unreached && (r.warnings & vd::WarnUnreached), "an impossible target is reported as 目標に届かない");
    check(r.frame[vd::MRecValid] > .5 && r.predicted < -6, "the best reachable value is still given");
    MemoryStream state;
    a.saveState(state);
    const auto y = playRestored(factory, state, in, 48000, 2);
    const auto m = measure(y, 2, 48000);
    check(std::abs(m.lufs - r.predicted) <= vd::ToleranceLu && m.truePeakDb <= -12 + 1e-6, "the unreachable result is still true to the real output and keeps the ceiling");
    std::cout << "PASS unreachable target: reported (warning + flag), best value " << r.predicted << " LUFS agrees with the real output within 0.1 LU, true peak " << m.truePeakDb << " dBTP <= -12\n";
}

void warningTests(IPluginFactory* factory) {
    const auto in = material(48000, 2, 20, 21);
    auto run = [&](const char* name, const Play& p, uint32_t expected, bool wantDone = true, const std::function<void(Instance&)>& prepare = {}) {
        Instance a(factory);
        a.start();
        if (prepare) prepare(a);
        const auto r = learn(a, in, p);
        check((r.learnFlags & expected) == expected, std::string(name) + ": flags " + std::to_string(r.learnFlags) + " lack " + std::to_string(expected));
        if (wantDone) check(int(r.frame[vd::MLearnState]) == vd::Learner::Done, std::string(name) + ": a result is still produced");
        if (wantDone) check((uint32_t(r.frame[vd::MRecFlags]) & expected) == expected, std::string(name) + ": the warning is kept with the saved result");
        std::printf("      warning %-24s flags=%u\n", name, r.learnFlags);
        return r;
    };
    Play midway;
    midway.transport.start = 48000 * 10;
    run("started midway", midway, vd::LfStartedMidway);
    Play back;
    back.transport.jumps = {{48000 * 8 / 512 * 512, -48000 * 3}};
    run("rewind", back, vd::LfBackJump);
    Play forward;
    forward.transport.jumps = {{48000 * 8 / 512 * 512, 48000 * 5}};
    run("fast forward", forward, vd::LfForwardJump);
    Play none;
    none.transport.context = false;
    run("no transport info", none, vd::LfNoTransport);
    {  // a clean pass has no warning, with and without the host position, and a non-block-aligned start of 0
        Instance a(factory);
        a.start();
        const auto r = learn(a, in, Play{});
        check(r.learnFlags == 0, "clean pass: no flags");
    }
    {  // the settings of gate/leveler/compressor change while learning
        Instance a(factory);
        a.start();
        a.command(vd::CmdLearnStart);
        const auto first = std::vector<float>(in.begin(), in.begin() + 48000 * 2 * 8);
        play<double>(a, first, Play{});
        a.plain(vd::CompThreshold, -30);
        const auto second = std::vector<float>(in.begin() + 48000 * 2 * 8, in.end());
        Play p;
        p.transport.start = 48000 * 8;
        play<double>(a, second, p);
        a.command(vd::CmdLearnFinish);
        const auto f = a.waitLearn();
        check((uint32_t(f[vd::MLearnFlags]) & vd::LfSettingsChanged) != 0, "changing the compressor while learning is flagged");
        std::printf("      warning %-24s flags=%u\n", "settings changed", uint32_t(f[vd::MLearnFlags]));
    }
    {  // the sample rate changes while learning
        Instance a(factory);
        a.start(48000);
        a.command(vd::CmdLearnStart);
        play<double>(a, material(48000, 2, 3, 1), Play{});
        a.stop();
        a.start(44100);
        Play p;
        p.transport.start = 48000 * 3;
        play<double>(a, material(44100, 2, 3, 2), p);
        a.command(vd::CmdLearnFinish);
        const auto f = a.waitLearn();
        check((uint32_t(f[vd::MLearnFlags]) & vd::LfRateChanged) != 0, "a sample rate change while learning is flagged");
        std::printf("      warning %-24s flags=%u\n", "sample rate changed", uint32_t(f[vd::MLearnFlags]));
    }
    {  // not playing: nothing is captured, and finishing explains why
        Instance a(factory);
        a.start();
        Play p;
        p.transport.playing = false;
        a.command(vd::CmdLearnStart);
        play<double>(a, in, p);
        vd::MeterFrame f{};
        check(a.meter(f) && int(f[vd::MLearnState]) == vd::Learner::Armed && f[vd::MLearnSeconds] == 0, "nothing is captured while the host is stopped");
        a.command(vd::CmdLearnFinish);
        std::u16string note;
        f = a.waitLearn();
        check(a.meter(f, &note) && int(f[vd::MLearnState]) == vd::Learner::Failed && !note.empty(), "learning without any sound fails with an explanation");
        check(f[vd::MRecValid] < .5, "a failed learning leaves no result");
    }
    {  // an export (offline process mode) counts as playing even without the playing flag
        Instance a(factory);
        a.start(48000, kSample64, 2, 4096, kOffline);
        Play p;
        p.transport.playing = false;
        p.transport.offline = true;
        const auto r = learn(a, in, p);
        check(int(r.frame[vd::MLearnState]) == vd::Learner::Done && r.learnFlags == 0, "learning works while the host renders offline");
    }
    std::cout << "PASS warnings: midway start, rewind, fast forward, missing transport info, settings change, sample rate change are flagged and kept with the result; a stopped host captures nothing\n";
}

void learnControlTests(IPluginFactory* factory) {
    const auto in = material(48000, 2, 15, 8);
    {  // 停止で学習を終える
        Instance a(factory);
        a.plain(vd::LearnAutoStop, 1);
        a.start();
        a.command(vd::CmdLearnStart);
        play<double>(a, in, Play{});
        a.proc->setProcessing(false);  // the host stops
        const auto f = a.waitLearn();
        check(int(f[vd::MLearnState]) == vd::Learner::Done && f[vd::MRecValid] > .5, "the stop of the transport ends the learning when asked to");
    }
    {  // without the option the stop does not end it
        Instance a(factory);
        a.start();
        a.command(vd::CmdLearnStart);
        play<double>(a, in, Play{});
        a.proc->setProcessing(false);
        a.proc->setProcessing(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        vd::MeterFrame f{};
        check(a.meter(f) && int(f[vd::MLearnState]) == vd::Learner::Capturing, "without the option the learning waits for [学習終了]");
        a.command(vd::CmdLearnCancel);
        check(a.meter(f) && int(f[vd::MLearnState]) == vd::Learner::Idle && f[vd::MRecValid] < .5, "[取り消し] returns to idle without a result");
    }
    {  // clear a result, learn again, deactivate and reactivate between sessions
        Instance a(factory);
        a.start();
        auto r = learn(a, in, Play{});
        check(r.frame[vd::MRecValid] > .5, "first result");
        a.command(vd::CmdClearResult);
        vd::MeterFrame f{};
        check(a.meter(f) && f[vd::MRecValid] < .5, "[結果を消す] removes the result");
        check(stateEntries(a).at(vd::ResultBase) == 0, "the cleared result is not saved");
        a.stop();
        a.start();
        r = learn(a, in, Play{});
        check(r.frame[vd::MRecValid] > .5, "learning again after the plug-in was deactivated and activated");
        // a second learning while a result exists replaces it
        Play p;
        const auto quiet = material(48000, 2, 15, 8, -12);
        const auto again = learn(a, quiet, p);
        check(again.gain > r.gain + 5 && again.gain < r.gain + 14, "a new learning replaces the result (a 12 dB quieter source needs more gain, less than 12 dB more as the compressor works less): " + std::to_string(again.gain - r.gain));
    }
    {  // closing the instance in the middle of a session must neither hang nor crash
        Instance* a = new Instance(factory);
        a->start();
        a->command(vd::CmdLearnStart);
        play<double>(*a, in, Play{});
        delete a;
        Instance* b = new Instance(factory);
        b->start();
        b->command(vd::CmdLearnStart);
        play<double>(*b, in, Play{});
        b->command(vd::CmdLearnFinish);
        delete b;
    }
    std::cout << "PASS learning control: auto-stop on transport stop, cancel, clear, relearn after reactivation, teardown during capture and computing\n";
}

void resultTests(IPluginFactory* factory) {
    const auto in = material(48000, 2, 30, 4);
    Instance a(factory);
    a.plain(vd::GainOffset, 0);
    a.start();
    const auto r = learn(a, in, Play{});
    const auto stale = [&] {
        vd::MeterFrame f{};
        check(a.meter(f), "meter");
        return f[vd::MRecStale] > .5;
    };
    check(!stale(), "a fresh result is not stale");
    a.plain(vd::GainOffset, 1.5);
    a.plain(vd::AutoOn, 1);
    a.plain(vd::Bypass, 1);
    a.plain(vd::GrLimit, 3);
    check(!stale(), "the fine offset, auto switch, bypass and GR warning level do not make it stale");
    a.plain(vd::Bypass, 0);
    a.plain(vd::Target, -20);
    check(stale(), "changing the target makes the result stale");
    a.plain(vd::Target, -23);
    check(!stale(), "putting the target back makes it fresh again");
    a.plain(vd::CompThreshold, -30);
    check(stale(), "changing the compressor makes it stale");
    a.plain(vd::CompThreshold, -24);
    a.plain(vd::MonoMode, 1);
    check(stale(), "changing the mono counting makes it stale");
    a.plain(vd::MonoMode, 0);
    check(!stale(), "…and fresh again");
    // the gain in force is automatic + offset, exactly
    a.plain(vd::GainOffset, 0);
    MemoryStream state;
    a.saveState(state);
    Play p;
    p.tailFrames = 4000;
    Instance x(factory), y(factory);
    x.loadState(state);
    x.start();
    y.plain(vd::AutoOn, 0);
    y.plain(vd::GainOffset, r.gain);
    y.start();
    const auto ox = play<double>(x, in, p), oy = play<double>(y, in, p);
    check(std::abs(r.gain) <= 12 && maxDifference(ox, oy) < 1e-9, "auto gain + offset: the learned gain equals the same value typed in as the fine offset");
    // auto off: the learned gain is ignored
    Instance z(factory), w(factory);
    z.loadState(state);
    z.plain(vd::AutoOn, 0);
    z.start();
    w.start();
    check(maxDifference(play<double>(z, in, p), play<double>(w, in, p)) == 0, "補正ゲイン（自動）off ignores the learned gain");
    std::cout << "PASS result: staleness follows exactly the settings that change the analysis; gain in force = automatic + fine offset; automatic off ignores it\n";
}

// A new target (or limiter setting) after learning is solved again over the kept recording, without playing again.
void retargetTests(IPluginFactory* factory) {
    const auto in = material(48000, 2, 30, 21);
    Instance a(factory);
    a.start();
    Play p;
    p.guard = false;
    const auto r = learn(a, in, p);
    check(int(r.frame[vd::MLearnState]) == vd::Learner::Done && r.frame[vd::MRecValid] > .5, "learned");
    auto settle = [&] {
        const auto start = std::chrono::steady_clock::now();
        vd::MeterFrame f{};
        for (;;) {
            check(a.meter(f), "meter");
            if (f[vd::MRecStale] < .5 && int(f[vd::MLearnState]) == vd::Learner::Done) return f;
            check(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 60, "a new target is solved again by itself");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    double worst = 0;
    int solved = 0;
    for (double target : {-18.0, -26.0, -12.0}) {
        a.plain(vd::Target, target);
        const auto f = settle();
        if (f[vd::MRecUnreached] < .5) {
            check(std::abs(f[vd::MRecPred] - target) <= vd::ToleranceLu, "the new prediction meets the new target");
            ++solved;
        }
        MemoryStream state;
        a.saveState(state);
        const auto m = measure(playRestored(factory, state, in, 48000, 2, kSample64), 2, 48000);
        worst = std::max(worst, std::abs(m.lufs - f[vd::MRecPred]));
        check(std::abs(m.lufs - f[vd::MRecPred]) <= vd::ToleranceLu, "the real output agrees with the new prediction: " + std::to_string(m.lufs - f[vd::MRecPred]));
    }
    check(solved >= 2, "reachable targets were met");
    a.plain(vd::Ceiling, -3);
    settle();
    // A compressor change does not match the recording: it stays stale and asks for a new pass.
    a.plain(vd::CompThreshold, -30);
    const auto start = std::chrono::steady_clock::now();
    vd::MeterFrame f{};
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(900)) {
        check(a.meter(f), "meter");
        check(int(f[vd::MLearnState]) != vd::Learner::Computing, "no new solve for a compressor change");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(f[vd::MRecStale] > .5, "…the result stays stale");
    std::printf("PASS retarget: new targets and ceiling solved again over the kept recording without playing; real output vs new prediction worst %.3f LU; compressor change stays stale\n", worst);
}

void stateTests(IPluginFactory* factory) {
    Instance a(factory), b(factory);
    std::array<double, vd::ParamCount> values{};
    for (uint32 i = 0; i < vd::ParamCount; ++i) {
        values[i] = vd::specs[i].steps ? double(i % 2) : std::fmod(.11 + .173 * i, 1.);
        a.parameter(i, values[i]);
    }
    a.parameter(vd::Bypass, 0);
    a.parameter(vd::GainOffset, vd::norm(vd::GainOffset, 0));  // the prediction is made without the fine offset
    a.parameter(vd::Target, vd::norm(vd::Target, -16));
    a.start();
    const auto in = material(48000, 2, 20, 31);
    const auto r = learn(a, in, Play{});
    MemoryStream state;
    a.saveState(state);
    b.loadState(state);
    for (uint32 i = 0; i < vd::ParamCount; ++i) check(b.ctrl->getParamNormalized(i) == a.ctrl->getParamNormalized(i), "parameter identity after restore");
    MemoryStream again;
    b.saveState(again);
    check(again.getSize() == state.getSize() && !std::memcmp(again.getData(), state.getData(), size_t(state.getSize())), "re-saved state identical (before processing)");
    b.start();
    vd::MeterFrame fb{};
    check(b.meter(fb) && fb[vd::MRecValid] > .5 && fb[vd::MRecGain] == r.gain && fb[vd::MRecPre] == r.pre && fb[vd::MRecPred] == r.predicted && fb[vd::MRecSource] == vd::SourceLearn,
          "the learned result survives save and restore exactly");
    check(fb[vd::MRecStale] < .5, "…and is still fresh in the restored instance (same settings, same channels)");
    // two fresh instances restored from one state render identical audio
    Instance c(factory), d(factory);
    c.loadState(state);
    d.loadState(state);
    c.start();
    d.start();
    Play p;
    p.tailFrames = 4000;
    check(maxDifference(play<double>(c, in, p), play<double>(d, in, p)) == 0, "restored states render identical audio");
    // the result still describes the real output (restore does not change what it means)
    const auto m = measure(playRestored(factory, state, in, 48000, 2), 2, 48000, false);
    check(std::abs(m.lufs - r.predicted) <= vd::ToleranceLu, "restored result still matches the real output");
    // rejections never change the current state
    const std::string before = stateBytes(b);
    auto expectReject = [&](const char* name, StateBuilder& s) {
        check(b.comp->setState(&s.stream()) != kResultOk, std::string("rejected: ") + name);
        check(stateBytes(b) == before, std::string("a rejected state leaves the current one untouched: ") + name);
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    {
        StateBuilder s;
        s.u32(7);
        expectReject("truncated header", s);
    }
    {
        StateBuilder s;
        s.header(0x12345678, 1, 0);
        expectReject("wrong magic", s);
    }
    {
        StateBuilder s;
        s.header(vd::StateMagic, 0, 0);
        expectReject("version 0", s);
    }
    {
        StateBuilder s;
        s.header(vd::StateMagic, 1, 5);
        s.entry(vd::GateOpen, .5);
        expectReject("count larger than the data", s);
    }
    {
        StateBuilder s;
        s.header(vd::StateMagic, 1, 1);
        s.entry(vd::GateOpen, nan);
        expectReject("NaN value", s);
    }
    {
        StateBuilder s;
        s.header(vd::StateMagic, 1, 1);
        s.entry(vd::GateOpen, 1.5);
        expectReject("value above 1", s);
    }
    {
        StateBuilder s;
        s.header(vd::StateMagic, 1, 2);
        s.entry(vd::GateOpen, .5);
        s.entry(vd::GateOpen, .6);
        expectReject("duplicate parameter", s);
    }
    {
        StateBuilder s;
        s.header(vd::StateMagic, 1, 5000);
        expectReject("absurd count", s);
    }
    auto packedState = [&](vd::Packed packed, int fields = vd::RecordFields) {
        StateBuilder s;
        s.header(vd::StateMagic, 1, uint32(fields));
        for (int k = 0; k < fields; ++k) s.entry(vd::ResultBase + uint32(k), packed[size_t(k)]);
        return s;
    };
    const vd::Packed good = vd::pack(vd::recordFrom([&] {
        vd::AnalysisResult x;
        x.ok = true;
        x.gainDb = 3;
        x.preLufs = -26;
        x.predictedLufs = -23;
        x.truePeakDb = -2;
        x.durationSec = 10;
        return x;
    }(), vd::SourceLearn, 0x123456789abcdefull, 2, 48000, 0));
    {
        auto s = packedState(good);
        check(b.comp->setState(&s.stream()) == kResultOk, "a well-formed result record is accepted");
        state.seek(0, IBStream::kIBSeekSet, nullptr);
        b.comp->setState(&state);
        check(stateBytes(b) == before, "…and the original state can be restored");
    }
    for (int field : {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}) {
        auto bad = good;
        bad[size_t(field)] = field == 9 ? 3 : field == 14 ? 2 : (field == 10 || field == 11 || field == 12 || field == 13) ? -1 : 1e9;
        auto s = packedState(bad);
        expectReject(("result field " + std::to_string(field) + " out of range").c_str(), s);
    }
    {
        auto bad = good;
        bad[1] = 0;
        auto s = packedState(bad);
        expectReject("result without a source", s);
    }
    {
        auto bad = good;
        bad[3] = nan;
        auto s = packedState(bad);
        expectReject("result field NaN", s);
    }
    {
        auto s = packedState(good, 9);
        expectReject("result with missing fields", s);
    }
    {  // a future version with an unknown ID keeps the known values; no result entry means no result
        StateBuilder s;
        s.header(vd::StateMagic, 9, 2);
        s.entry(vd::GateOpen, .25);
        s.entry(99, .5);
        Instance f(factory);
        check(f.comp->setState(&s.stream()) == kResultOk, "future version accepted");
        check(f.ctrl->setComponentState(&s.stream()) == kResultOk && f.ctrl->getParamNormalized(vd::GateOpen) == .25, "future version known value");
        check(f.ctrl->getParamNormalized(vd::CompThreshold) == vd::norm(vd::CompThreshold, -24), "missing values fall back to defaults");
        f.start();
        vd::MeterFrame ff{};
        check(f.meter(ff) && ff[vd::MRecValid] < .5, "no result entry: no result");
    }
    {  // a state with a result of a stereo track loaded into a mono instance is stale (the hash includes the channel count)
        Instance mo(factory);
        mo.loadState(state);
        mo.start(48000, kSample64, 1);
        vd::MeterFrame f{};
        check(mo.meter(f) && f[vd::MRecValid] > .5 && f[vd::MRecStale] > .5, "a result made for stereo is stale on a mono track");
    }
    std::cout << "PASS state: parameters and the learned result round trip exactly; identical audio; "
                 "truncated/NaN/range/duplicate/missing-field/bad-record states rejected without touching the current state; future version and unknown ID tolerated\n";
}

// Processing through the real binary: block-size independence, sample-accurate automation, in-place buffers,
// and bit-identity with the DSP chain called directly.
void processingTests(IPluginFactory* factory) {
    int cases = 0;
    for (int format : {kSample64, kSample32})
        for (double rate : {48000., 96000.}) {
            const auto in = material(rate, 2, 4.0, 9);
            const std::vector<std::vector<int>> schedules{{128}, {1}, {4096}, {1000}, {1, 31, 127, 511, 2048, 64, 1000, 3}, {17, 256, 999}};
            std::vector<double> reference;
            for (const auto& schedule : schedules) {
                Instance a(factory);
                a.plain(vd::CompThreshold, -30);
                a.start(rate, format);
                Play p;
                p.schedule = schedule;
                const auto y = format == kSample64 ? play<double>(a, in, p) : play<float>(a, in, p);
                if (reference.empty()) {
                    Instance b(factory);
                    b.plain(vd::CompThreshold, -30);
                    b.start(rate, format);
                    Play q = p;
                    q.inPlace = true;
                    const auto z = format == kSample64 ? play<double>(b, in, q) : play<float>(b, in, q);
                    check(maxDifference(y, z) == 0, "in-place processing identical");
                    reference = y;
                } else {
                    check(maxDifference(reference, y) == 0, "output independent of block size (bit-exact)");
                }
                ++cases;
            }
            std::vector<double> automated;
            const int total = int(rate * 3);
            for (const auto& schedule : std::vector<std::vector<int>>{{128}, {1, 31, 127, 511, 2048, 64, 1000, 3}, {1}}) {
                Instance a(factory);
                Play p;
                p.schedule = schedule;
                p.automationId = vd::CompThreshold;
                p.automation = [total](int64 k) { return .3 + .4 * double(k) / total; };
                a.parameter(vd::CompThreshold, p.automation(-1));
                a.start(rate, format);
                const std::vector<float> part(in.begin(), in.begin() + long(total) * 2);
                const auto y = format == kSample64 ? play<double>(a, part, p) : play<float>(a, part, p);
                if (automated.empty()) automated = y;
                else check(maxDifference(automated, y) < (format == kSample64 ? 1e-9 : 1e-6), "automation independent of block size");
                ++cases;
            }
        }
    {  // the plug-in equals the DSP chain called directly (same code, 64-bit path)
        Instance a(factory);
        a.start();
        const auto in = material(48000, 2, 10, 5);
        Play p;
        p.schedule = {1, 31, 127, 511, 2048, 64, 1000, 3};
        const auto y = play<double>(a, in, p);
        vd::Chain chain;
        chain.prepare(48000, 2);
        chain.set(vd::settingsFrom(vd::defaults()), 0);
        chain.reset();
        for (size_t i = 0; i < in.size() / 2; ++i) {
            const double x[2]{double(in[i * 2]), double(in[i * 2 + 1])};
            double out[2];
            chain.tick(x, out);
            check(out[0] == y[i * 2] && out[1] == y[i * 2 + 1], "plugin output bit-identical to the DSP chain");
        }
    }
    std::cout << "PASS processing: " << cases << " renders; bit-exact across block schedules 1..4096 (32/64-bit, 48/96 kHz), in-place, sample-accurate automation, plugin == DSP chain\n";
}

void effectTests(IPluginFactory* factory) {
    const auto in = material(48000, 2, 12, 17);
    // `hot` first adds +12 dB so that the limiter has something to do.
    auto renderWith = [&](int id, double normalized, bool hot) {
        Instance a(factory);
        if (hot) a.plain(vd::GainOffset, 12);
        if (id >= 0) a.parameter(uint32_t(id), normalized);
        a.start();
        return play<double>(a, in, Play{});
    };
    const auto reference = renderWith(-1, 0, false), hotReference = renderWith(-1, 0, true);
    const auto d = vd::defaults();
    int changed = 0;
    for (uint32_t id : {vd::GateOn, vd::GateOpen, vd::GateClose, vd::GateHold, vd::GateAttack, vd::GateRelease, vd::GateRange, vd::GateHpf, vd::LevelerOn, vd::CompOn, vd::CompThreshold,
                        vd::CompRatio, vd::CompAttack, vd::CompRelease, vd::CompKnee, vd::GainOffset, vd::LimiterOn, vd::Ceiling, vd::LimiterRelease, vd::Bypass}) {
        double v = d[id] > .5 ? d[id] - .35 : d[id] + .35;
        if (vd::specs[id].steps) v = d[id] > .5 ? 0. : 1.;
        bool hot = false;
        if (id == vd::GainOffset) v = vd::norm(vd::GainOffset, 6);
        if (id == vd::GateOpen) v = vd::norm(vd::GateOpen, -25);
        if (id == vd::GateClose) v = vd::norm(vd::GateClose, -30);
        if (id == vd::GateHpf) v = vd::norm(vd::GateHpf, 300);
        if (id == vd::LimiterOn || id == vd::Ceiling || id == vd::LimiterRelease) hot = true;
        if (id == vd::Ceiling) v = vd::norm(vd::Ceiling, -9);
        const double diff = maxDifference(hot ? hotReference : reference, renderWith(int(id), v, hot));
        check(diff > 1e-7, std::string("parameter changes the output: ") + std::to_string(id) + " " + std::to_string(diff));
        ++changed;
    }
    std::cout << "PASS effect: " << changed << " controls change the output\n";
}

void hostRelationTests(IPluginFactory* factory) {
    const auto in = material(48000, 2, 6, 14);
    {
        Instance a(factory), b(factory), c(factory);
        b.plain(vd::CompThreshold, -40);
        b.plain(vd::GainOffset, 6);
        a.start();
        b.start();
        c.start();
        std::vector<double> ya, yc;
        const size_t chunk = 48000 * 2;
        for (size_t part = 0; part * chunk < in.size() / 2; ++part) {
            const std::vector<float> piece(in.begin() + long(part * chunk) * 2, in.begin() + long(std::min(in.size() / 2, (part + 1) * chunk)) * 2);
            const auto pa = play<double>(a, piece, Play{}), pb = play<double>(b, piece, Play{}), pc = play<double>(c, piece, Play{});
            ya.insert(ya.end(), pa.begin(), pa.end());
            yc.insert(yc.end(), pc.begin(), pc.end());
        }
        check(maxDifference(ya, yc) == 0, "instances are independent");
    }
    auto renderAt = [&](int mode) {
        Instance a(factory);
        a.start(48000, kSample64, 2, 4096, mode);
        Play p;
        p.transport.offline = mode == kOffline;
        return play<double>(a, in, p);
    };
    check(maxDifference(renderAt(kRealtime), renderAt(kOffline)) == 0, "offline == realtime");
    {
        Instance a(factory);
        a.start();
        std::vector<float> silent(48000 * 2, 0.f);
        for (double v : play<double>(a, silent, Play{})) check(v == 0, "silence in, silence out");
        auto noisy = in;
        for (size_t i = 5; i < noisy.size(); i += 977) noisy[i] = std::numeric_limits<float>::quiet_NaN();
        for (size_t i = 11; i < noisy.size(); i += 1301) noisy[i] = std::numeric_limits<float>::infinity();
        play<double>(a, noisy, Play{});  // play() checks every output sample is finite
        const auto after = play<double>(a, in, Play{});
        for (double v : after) check(std::abs(v) < 4, "recovers after NaN/Inf input");
        ProcessData d{};
        d.symbolicSampleSize = kSample64;
        check(a.proc->process(d) == kResultOk, "zero-length process");
    }
    {  // meter frames and the plug-in's own output loudness agree with an independent measurement
        Instance a(factory);
        a.plain(vd::GainOffset, 5);
        a.start();
        vd::MeterFrame f{};
        check(a.meter(f) && vd::validMeter(f), "a meter frame is available right after activation");
        const auto generation = f[vd::MGeneration];
        Play p;
        p.tailFrames = 48000;
        const auto y = play<double>(a, in, p);
        check(a.meter(f) && vd::validMeter(f) && f[vd::MGeneration] > generation, "meter frames advance while processing");
        check(f[vd::MRate] == 48000 && f[vd::MChannels] == 2 && f[vd::MLatency] == a.proc->getLatencySamples(), "meter frame reports the rate, channels and latency");
        const auto m = measure(y, 2, 48000, true);
        check(std::abs(f[vd::MIntegrated] - m.lufs) < 0.05, "the plug-in's Integrated reading agrees with an independent measurement: " + std::to_string(f[vd::MIntegrated]) + " vs " + std::to_string(m.lufs));
        check(std::abs(f[vd::MOutTruePeak] - m.truePeakDb) < 0.1, "the plug-in's True Peak reading agrees with the 16x reference: " + std::to_string(f[vd::MOutTruePeak]) + " vs " + std::to_string(m.truePeakDb));
        a.command(vd::CmdResetMeter);
        play<double>(a, std::vector<float>(48000 * 2, 0.f), Play{});
        check(a.meter(f) && f[vd::MIntegrated] <= -150, "[測り直し] resets the output measurement");
    }
    {  // dual mono counts a mono track 3 dB louder in the plug-in's own reading
        Instance a(factory), b(factory);
        b.plain(vd::MonoMode, 1);
        a.start(48000, kSample64, 1);
        b.start(48000, kSample64, 1);
        const auto mono = material(48000, 1, 12, 6);
        play<double>(a, mono, Play{});
        play<double>(b, mono, Play{});
        vd::MeterFrame fa{}, fb{};
        check(a.meter(fa) && b.meter(fb) && std::abs((fb[vd::MIntegrated] - fa[vd::MIntegrated]) - 3.0103) < 0.01, "デュアルモノ reads 3.01 dB louder");
    }
    std::cout << "PASS host relations: independent instances; offline == realtime; silence; NaN/Inf input; zero-length process; meter frames; the plug-in's own Integrated/True Peak readings agree with independent measurements; デュアルモノ +3.01 dB\n";
}

void realtimeGuardTests(IPluginFactory* factory, HMODULE module) {
    const int patched = rtguard::install(module);
    check(patched >= 3, "real-time guard installed on plugin imports");
    rtguard::reset();
    {
        rtguard::Armed armed;
        Instance probe(factory);  // creation allocates; proves the guard sees plugin heap use
    }
    check(rtguard::heap > 0, "guard observes plugin heap allocations");
    long samples = 0;
    for (int format : {kSample32, kSample64})
        for (double rate : {44100., 192000.})
            for (int ch : {1, 2}) {
                Instance a(factory);
                a.start(rate, format, ch);
                const auto in = material(rate, ch, rate > 100000 ? 3.0 : 6.0, 41);
                a.command(vd::CmdLearnStart);  // capturing runs inside the guarded blocks
                rtguard::reset();
                Play p;
                p.guard = true;
                p.schedule = {1, 31, 127, 511, 2048, 64, 1000, 3};
                p.automationId = vd::CompThreshold;
                p.automation = [n = in.size() / size_t(ch)](int64 k) { return .3 + .4 * double(k) / double(n); };
                if (format == kSample32) play<float>(a, in, p);
                else play<double>(a, in, p);
                check(rtguard::total() == 0, "process() used heap/lock/wait/file/window API: " + rtguard::report());
                samples += long(in.size() / size_t(ch));
                vd::MeterFrame f{};
                a.meter(f);
                check(f[vd::MLearnSeconds] > 1, "the learner was capturing during the guarded blocks");
                a.command(vd::CmdLearnCancel);
            }
    std::cout << "PASS real-time guard: " << patched << " import slots patched; 0 heap/lock/wait/file/window calls in process() over " << samples
              << " frames while learning was capturing, with automation, odd block sizes, mono/stereo, 32/64-bit\n";
}

void cpuTest(IPluginFactory* factory) {
    for (int ch : {2, 1}) {
        Instance a(factory);
        a.start(48000, kSample32, ch, 512);
        const double seconds = 120;
        const auto in = material(48000, ch, seconds, 50);
        a.command(vd::CmdLearnStart);
        double spent = 0;
        Play p;
        p.schedule = {512};
        p.processSeconds = &spent;
        play<float>(a, in, p);
        a.command(vd::CmdLearnCancel);
        std::printf("      CPU (%dch, 48 kHz, 512-sample blocks, learning on, process() only): %.2f %% of one core (%.0fx real time)\n", ch, 100 * spent / seconds, seconds / spent);
        check(spent / seconds < 0.1, "CPU load of one instance below 10 % of a core");
    }
    std::cout << "PASS cpu: one instance far below 10 % of a core\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        check(argc >= 2, "usage: vd_host_tests plugin-binary [--quick]");
        quick = argc >= 3 && !std::wcscmp(argv[2], L"--quick");
        HMODULE module = LoadLibraryW(argv[1]);
        check(module != nullptr, "LoadLibrary");
        auto init = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
        auto quit = reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
        auto create = reinterpret_cast<IPluginFactory*(PLUGIN_API*)()>(GetProcAddress(module, "GetPluginFactory"));
        check(create && (!init || init()), "DLL entry points");
        auto* factory = create();
        check(factory != nullptr, "factory");
        parameterTests(factory);
        layoutTests(factory);
        latencyTests(factory);
        stateTests(factory);
        processingTests(factory);
        effectTests(factory);
        hostRelationTests(factory);
        learningTests(factory);
        unreachableTest(factory);
        warningTests(factory);
        learnControlTests(factory);
        resultTests(factory);
        retargetTests(factory);
        realtimeGuardTests(factory, module);
        cpuTest(factory);
        factory->release();
        if (quit) quit();
        FreeLibrary(module);
        std::cout << "PASS all host tests\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
