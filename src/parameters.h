#pragma once
// Parameter IDs and the frozen mapping between normalized (host) and plain values.
// IDs are saved in songs and presets: never renumber or reuse one. New parameters get new IDs at the end.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vd {
enum ParamId : uint32_t {
    GateOn, GateOpen, GateClose, GateHold, GateAttack, GateRelease, GateRange, GateHpf,
    LevelerOn, LevelerTarget, LevelerRatio,
    CompOn, CompThreshold, CompRatio, CompAttack, CompRelease, CompKnee,
    AutoOn, GainOffset,
    LimiterOn, Ceiling, LimiterRelease,
    Target, MonoMode, GrLimit,
    Bypass,
    LearnAutoStop,
    ParamCount
};

enum class Scale { Linear, Log, Toggle, List };
struct Spec {
    const char16_t* name;
    const char16_t* unit;
    double lo, hi, def;
    Scale scale;
    int steps;  // 0 = continuous; otherwise the number of steps (1 = toggle)
    bool automatable;
};

// Gate: -80 dB range means full mute; hpf at its lowest value means off.
constexpr double GateRangeMute = -80.0, GateHpfOff = 20.0;

inline constexpr std::array<Spec, ParamCount> specs{{
    {u"ゲート", u"", 0, 1, 1, Scale::Toggle, 1, true},
    {u"ゲート 開く閾値", u"dB", -80, 0, -42, Scale::Linear, 0, true},
    {u"ゲート 閉じる閾値", u"dB", -80, 0, -48, Scale::Linear, 0, true},
    {u"ゲート Hold", u"ms", 0, 1000, 120, Scale::Linear, 0, true},
    {u"ゲート Attack", u"ms", 0.1, 50, 3, Scale::Log, 0, true},
    {u"ゲート Release", u"ms", 5, 2000, 180, Scale::Log, 0, true},
    {u"ゲート Range", u"dB", -80, 0, -12, Scale::Linear, 0, true},
    {u"ゲート 検出の低域カット", u"Hz", 20, 400, 80, Scale::Log, 0, true},
    {u"レベラー", u"", 0, 1, 0, Scale::Toggle, 1, true},
    {u"レベラー 基準", u"dB", -50, -6, -24, Scale::Linear, 0, true},
    {u"レベラー Ratio", u"", 1, 4, 1.5, Scale::Linear, 0, true},
    {u"コンプ", u"", 0, 1, 1, Scale::Toggle, 1, true},
    {u"コンプ Threshold", u"dB", -60, 0, -24, Scale::Linear, 0, true},
    {u"コンプ Ratio", u"", 1, 20, 3, Scale::Log, 0, true},
    {u"コンプ Attack", u"ms", 0.5, 200, 10, Scale::Log, 0, true},
    {u"コンプ Release", u"ms", 10, 2000, 150, Scale::Log, 0, true},
    {u"コンプ Knee", u"dB", 0, 24, 6, Scale::Linear, 0, true},
    {u"補正ゲイン（自動）", u"", 0, 1, 1, Scale::Toggle, 1, true},
    {u"補正ゲイン 微調整", u"dB", -12, 12, 0, Scale::Linear, 0, true},
    {u"リミッター", u"", 0, 1, 1, Scale::Toggle, 1, true},
    {u"リミッター 上限", u"dBTP", -12, 0, -1, Scale::Linear, 0, true},
    {u"リミッター Release", u"ms", 10, 500, 80, Scale::Log, 0, true},
    {u"目標ラウドネス", u"LUFS", -40, -5, -23, Scale::Linear, 0, false},
    {u"モノラルの測り方", u"", 0, 1, 0, Scale::List, 1, false},
    {u"リミッターGR警告", u"dB", 1, 12, 6, Scale::Linear, 0, false},
    {u"Bypass", u"", 0, 1, 0, Scale::Toggle, 1, true},
    {u"停止で学習を終える", u"", 0, 1, 0, Scale::Toggle, 1, false},
}};

inline const char16_t* const monoNames[]{u"1ch（既定）", u"デュアルモノ（+3 dB）"};

using Values = std::array<double, ParamCount>;
inline double normalized(double v) noexcept { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }
inline double plain(uint32_t id, double n) noexcept {
    const auto& s = specs[id];
    n = normalized(n);
    switch (s.scale) {
    case Scale::Log: return s.lo * std::pow(s.hi / s.lo, n);
    case Scale::Toggle:
    case Scale::List: return std::round(n * s.steps);
    default: return s.lo + (s.hi - s.lo) * n;
    }
}
inline double norm(uint32_t id, double p) noexcept {
    const auto& s = specs[id];
    if (!std::isfinite(p)) return normalized(0);
    p = std::clamp(p, s.lo, s.hi);
    switch (s.scale) {
    case Scale::Log: return std::log(p / s.lo) / std::log(s.hi / s.lo);
    case Scale::Toggle:
    case Scale::List: return std::clamp(std::round(p), s.lo, s.hi) / s.steps;
    default: return (p - s.lo) / (s.hi - s.lo);
    }
}
inline int index(uint32_t id, double n) noexcept { return int(std::lround(plain(id, n))); }
inline Values defaults() noexcept {
    Values v{};
    for (uint32_t i = 0; i < ParamCount; ++i) v[i] = norm(i, specs[i].def);
    return v;
}

// All plain values at once: what the DSP and the analysis use.
struct Settings {
    bool gateOn = true;
    double gateOpen = -42, gateClose = -48, gateHold = 120, gateAttack = 3, gateRelease = 180, gateRange = -12, gateHpf = 80;
    bool levelerOn = false;
    double levelerTarget = -24, levelerRatio = 1.5;
    bool compOn = true;
    double compThreshold = -24, compRatio = 3, compAttack = 10, compRelease = 150, compKnee = 6;
    bool autoOn = true;
    double offsetDb = 0;
    bool limiterOn = true;
    double ceiling = -1, limiterRelease = 80;
    double target = -23;
    bool dualMono = false;
    double grLimit = 6;
    bool bypass = false;
    bool learnAutoStop = false;
};
inline Settings settingsFrom(const Values& n) noexcept {
    Settings s;
    auto on = [&](uint32_t id) { return plain(id, n[id]) >= .5; };
    auto p = [&](uint32_t id) { return plain(id, n[id]); };
    s.gateOn = on(GateOn);
    s.gateOpen = p(GateOpen);
    s.gateClose = std::min(p(GateClose), s.gateOpen);  // the close threshold never sits above the open one
    s.gateHold = p(GateHold);
    s.gateAttack = p(GateAttack);
    s.gateRelease = p(GateRelease);
    s.gateRange = p(GateRange);
    s.gateHpf = p(GateHpf);
    s.levelerOn = on(LevelerOn);
    s.levelerTarget = p(LevelerTarget);
    s.levelerRatio = p(LevelerRatio);
    s.compOn = on(CompOn);
    s.compThreshold = p(CompThreshold);
    s.compRatio = p(CompRatio);
    s.compAttack = p(CompAttack);
    s.compRelease = p(CompRelease);
    s.compKnee = p(CompKnee);
    s.autoOn = on(AutoOn);
    s.offsetDb = p(GainOffset);
    s.limiterOn = on(LimiterOn);
    s.ceiling = p(Ceiling);
    s.limiterRelease = p(LimiterRelease);
    s.target = p(Target);
    s.dualMono = index(MonoMode, n[MonoMode]) == 1;
    s.grLimit = p(GrLimit);
    s.bypass = on(Bypass);
    s.learnAutoStop = on(LearnAutoStop);
    return s;
}

namespace detail {
struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void mix(double v) noexcept {
        uint64_t bits;
        static_assert(sizeof(bits) == sizeof(v));
        const double canonical = v == 0 ? 0.0 : v;  // -0 and +0 are the same setting
        std::memcpy(&bits, &canonical, sizeof(bits));
        for (int i = 0; i < 8; ++i) {
            h ^= (bits >> (8 * i)) & 0xff;
            h *= 1099511628211ull;
        }
    }
};
inline void mixPreStage(Fnv& f, const Settings& s, int channels) noexcept {
    f.mix(channels);
    f.mix(s.gateOn);
    f.mix(s.gateOpen);
    f.mix(s.gateClose);
    f.mix(s.gateHold);
    f.mix(s.gateAttack);
    f.mix(s.gateRelease);
    f.mix(s.gateRange);
    f.mix(s.gateHpf);
    f.mix(s.levelerOn);
    f.mix(s.levelerTarget);
    f.mix(s.levelerRatio);
    f.mix(s.compOn);
    f.mix(s.compThreshold);
    f.mix(s.compRatio);
    f.mix(s.compAttack);
    f.mix(s.compRelease);
    f.mix(s.compKnee);
}
}  // namespace detail

// What shapes the signal that reaches the correction gain (gate, leveler, compressor, channel count).
inline uint64_t preStageHash(const Settings& s, int channels) noexcept {
    detail::Fnv f;
    detail::mixPreStage(f, s, channels);
    return f.h;
}

// Everything that changes what the analysis would measure. The fine offset, the auto-gain switch,
// bypass and the GR warning level are applied or judged afterwards and are not part of it.
inline uint64_t analysisHash(const Settings& s, int channels) noexcept {
    detail::Fnv f;
    detail::mixPreStage(f, s, channels);
    f.mix(s.limiterOn);
    f.mix(s.ceiling);
    f.mix(s.limiterRelease);
    f.mix(s.target);
    f.mix(s.dualMono);
    return f.h;
}
}  // namespace vd
