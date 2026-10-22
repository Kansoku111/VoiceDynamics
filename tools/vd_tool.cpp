// Command-line helper for the evidence runs: generates speech-like test files, measures loudness,
// runs the analysis and the whole chain on WAV files, and times the learning of a long recording.
//   vd_tool gen <out.wav> [--seconds 60] [--seed 1] [--rate 48000] [--channels 2] [--db 0]
//   vd_tool measure <in.wav>
//   vd_tool analyze <in.wav> [settings]
//   vd_tool render <in.wav> <out.wav> [--gain <dB>|auto] [settings]
//   vd_tool bench [--minutes 60] [--channels 2] [--rate 48000] [settings]
// settings: --target <LUFS> --ceiling <dBTP> --mono 1ch|dual --limiter on|off --gate on|off --comp on|off --leveler on|off
#include "analyzer.h"
#include "chain.h"
#include "learner.h"
#include "loudness.h"
#include "voice.h"
#include "wavio.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

using namespace vd;

namespace {
using Args = std::map<std::string, std::string>;

Args parse(int argc, char** argv, int from, std::vector<std::string>& positional) {
    Args a;
    for (int i = from; i < argc; ++i) {
        if (std::strncmp(argv[i], "--", 2) == 0 && i + 1 < argc) {
            a[argv[i] + 2] = argv[i + 1];
            ++i;
        } else {
            positional.push_back(argv[i]);
        }
    }
    return a;
}
double number(const Args& a, const char* key, double fallback) {
    const auto it = a.find(key);
    return it == a.end() ? fallback : std::atof(it->second.c_str());
}
bool flag(const Args& a, const char* key, bool fallback) {
    const auto it = a.find(key);
    return it == a.end() ? fallback : (it->second == "on" || it->second == "1" || it->second == "dual");
}
Settings settingsFrom(const Args& a) {
    Settings s;
    s.target = number(a, "target", s.target);
    s.ceiling = number(a, "ceiling", s.ceiling);
    s.dualMono = flag(a, "mono", false);
    s.limiterOn = flag(a, "limiter", true);
    s.gateOn = flag(a, "gate", true);
    s.compOn = flag(a, "comp", true);
    s.levelerOn = flag(a, "leveler", false);
    return s;
}
double seconds(std::chrono::steady_clock::time_point since) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count(); }

int gen(const std::vector<std::string>& pos, const Args& a) {
    if (pos.empty()) return 2;
    const double rate = number(a, "rate", 48000);
    const int channels = int(number(a, "channels", 2));
    const auto samples = voice::make(rate, number(a, "seconds", 60), channels, uint64_t(number(a, "seed", 1)), number(a, "db", 0));
    std::string error;
    if (!wavWriteFloat(pos[0], rate, channels, samples, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("wrote %s (%zu frames, %.0f Hz, %d ch)\n", pos[0].c_str(), samples.size() / size_t(channels), rate, channels);
    return 0;
}

int measure(const std::vector<std::string>& pos, const Args& a) {
    if (pos.empty()) return 2;
    WavInfo info;
    ChunkedPcm pcm;
    std::string error;
    const int channels = int(number(a, "channels", 2));
    if (!wavRead(pos[0], channels, info, pcm, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    LoudnessMeter m;
    m.prepare(info.rate, channels, flag(a, "mono", false));
    TruePeakDetector tp;
    tp.prepare(info.rate, channels);
    double f[2]{}, peak = 0;
    for (size_t i = 0; i < pcm.frames(); ++i) {
        for (int c = 0; c < channels; ++c) f[c] = double(pcm.frame(i)[c]);
        m.push(f);
        peak = std::max(peak, tp.push(f, peak));
    }
    const double zero[2]{0, 0};
    for (int i = 0; i < tp.delay(); ++i) peak = std::max(peak, tp.push(zero, peak));
    std::printf("integrated_lufs %.4f\nmax_momentary_lufs %.4f\nmax_shortterm_lufs %.4f\nlra_lu %.4f\ntrue_peak_dbtp %.4f\nframes %zu\nrate %.0f\n", m.integrated(),
                m.maxMomentary(), m.maxShortTerm(), m.lra(), linToDb(peak), pcm.frames(), info.rate);
    return 0;
}

int analyze(const std::vector<std::string>& pos, const Args& a) {
    if (pos.empty()) return 2;
    WavInfo info;
    ChunkedPcm pcm;
    std::string error;
    const int channels = int(number(a, "channels", 2));
    if (!wavRead(pos[0], channels, info, pcm, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = analyzeFile(settingsFrom(a), channels, info.rate, pcm);
    std::printf("ok %d\nerror %s\ngain_db %.4f\npre_lufs %.4f\npredicted_lufs %.4f\ntrue_peak_dbtp %.4f\nmax_gr_db %.3f\nwarnings %u\nseconds %.2f\nanalysis_seconds %.2f\n",
                r.ok ? 1 : 0, r.error.c_str(), r.gainDb, r.preLufs, r.predictedLufs, r.truePeakDb, r.maxGrDb, r.warnings, r.durationSec, seconds(t0));
    return r.ok ? 0 : 1;
}

int render(const std::vector<std::string>& pos, const Args& a) {
    if (pos.size() < 2) return 2;
    WavInfo info;
    ChunkedPcm pcm;
    std::string error;
    const int channels = int(number(a, "channels", 2));
    if (!wavRead(pos[0], channels, info, pcm, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const Settings s = settingsFrom(a);
    double gain = 0;
    const auto g = a.find("gain");
    if (g != a.end() && g->second == "auto") {
        ChunkedPcm copy;
        copy.configure(channels);
        for (size_t i = 0; i < pcm.frames(); ++i) copy.append(pcm.frame(i), 1);
        const auto r = analyzeFile(s, channels, info.rate, copy);
        if (!r.ok) {
            std::fprintf(stderr, "%s\n", r.error.c_str());
            return 1;
        }
        gain = r.gainDb;
        std::printf("auto gain %.4f dB (predicted %.4f LUFS)\n", gain, r.predictedLufs);
    } else if (g != a.end()) {
        gain = std::atof(g->second.c_str());
    }
    Chain chain;
    chain.prepare(info.rate, channels);
    chain.set(s, gain);
    chain.reset();
    const size_t frames = pcm.frames(), latency = size_t(chain.latency());
    std::vector<float> out(frames * size_t(channels));
    double x[2]{}, y[2]{};
    for (size_t i = 0; i < frames + latency; ++i) {
        for (int c = 0; c < channels; ++c) x[c] = i < frames ? double(pcm.frame(i)[c]) : 0.0;
        chain.tick(x, y);
        if (i >= latency)
            for (int c = 0; c < channels; ++c) out[(i - latency) * size_t(channels) + size_t(c)] = float(y[c]);
    }
    if (!wavWriteFloat(pos[1], info.rate, channels, out, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}

// Learns a long synthetic recording the way a fast offline render would (as fast as the chain runs),
// then times the gain search that follows.
int bench(const Args& a) {
    const double minutes = number(a, "minutes", 60), rate = number(a, "rate", 48000);
    const int channels = int(number(a, "channels", 2));
    const Settings s = settingsFrom(a);
    const size_t chunk = size_t(60 * rate);
    std::vector<std::vector<float>> pieces;
    for (int k = 0; k < 6; ++k) pieces.push_back(voice::make(rate, 60, channels, 100 + uint64_t(k), (k % 3) * 2.0 - 2.0));
    Chain chain;
    chain.prepare(rate, channels);
    chain.set(s, 0);
    chain.reset();
    Learner learner;
    learner.arm([s] { return s; }, 1u << 21);
    std::vector<float> staged(4096 * 2);
    double x[2]{}, y[2]{}, p[2]{};
    const uint64_t hash = preStageHash(s, channels);
    const auto t0 = std::chrono::steady_clock::now();
    int64_t position = 0;
    const int total = int(minutes);
    for (int m = 0; m < total; ++m) {
        const auto& piece = pieces[size_t(m) % pieces.size()];
        for (size_t pos = 0; pos < chunk; pos += 4096) {
            const int n = int(std::min<size_t>(4096, chunk - pos));
            for (int i = 0; i < n; ++i) {
                for (int c = 0; c < channels; ++c) x[c] = double(piece[(pos + size_t(i)) * size_t(channels) + size_t(c)]);
                chain.tick(x, y, p);
                for (int c = 0; c < channels; ++c) staged[size_t(i) * size_t(channels) + size_t(c)] = float(p[c]);
            }
            // a host rendering faster than real time: wait when the FIFO is about to overflow (a real host cannot wait)
            learner.block(staged.data(), uint32_t(n), channels, rate, true, true, position, hash);
            position += n;
            while (learner.status().droppedFrames > 0) {
                std::fprintf(stderr, "FIFO overflow\n");
                return 1;
            }
        }
        if (m % 10 == 9) std::printf("  %d / %d minutes played (%.1f s elapsed)\n", m + 1, total, seconds(t0));
    }
    const double playSeconds = seconds(t0);
    const auto t1 = std::chrono::steady_clock::now();
    learner.finish();
    for (;;) {
        const auto st = learner.state();
        if (st == Learner::Done || st == Learner::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const double solveSeconds = seconds(t1);
    Learner::Result r;
    learner.result(r);
    std::printf("material %.0f minutes, %d ch, %.0f Hz\nplay-through (chain + capture) %.1f s (%.1fx real time)\nresult after stop: %.1f s\nok %d gain %.3f dB predicted %.3f LUFS true peak %.2f dBTP max GR %.2f dB passes %d flags %u\n",
                minutes, channels, rate, playSeconds, minutes * 60.0 / playSeconds, solveSeconds, r.analysis.ok ? 1 : 0, r.analysis.gainDb, r.analysis.predictedLufs,
                r.analysis.truePeakDb, r.analysis.maxGrDb, r.analysis.evaluations, r.flags);
    return r.analysis.ok ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: vd_tool gen|measure|analyze|render|bench ...\n");
        return 2;
    }
    std::vector<std::string> pos;
    const Args a = parse(argc, argv, 2, pos);
    const std::string cmd = argv[1];
    if (cmd == "gen") return gen(pos, a);
    if (cmd == "measure") return measure(pos, a);
    if (cmd == "analyze") return analyze(pos, a);
    if (cmd == "render") return render(pos, a);
    if (cmd == "bench") return bench(a);
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
