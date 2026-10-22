#pragma once
// The learned result that a song keeps: the automatic correction gain and what it was measured with.
// It is saved in the component state, sent between processor and controller, and judged for staleness.
#include "analyzer.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace vd {
enum ResultSource : int { SourceNone = 0, SourceLearn = 1, SourceFile = 2 };

struct ResultRecord {
    bool valid = false;
    int source = SourceNone;
    double gainDb = 0;           // automatic correction gain, without the fine offset
    double preLufs = -200;       // LUFS-I before the gain (after gate/leveler/compressor)
    double predictedLufs = -200; // LUFS-I of the output with that gain
    double truePeakDb = -200;
    double maxGrDb = 0;
    double durationSec = 0;
    double fs = 48000;
    int channels = 2;
    uint64_t hash = 0;           // analysisHash() of the settings it was made with
    uint32_t warnings = 0;       // Warning flags (analyzer.h)
    uint32_t learnFlags = 0;     // LearnFlag bits (learner.h); 0 for a file
    bool unreached = false;
};

constexpr int RecordFields = 15;
using Packed = std::array<double, RecordFields>;

inline Packed pack(const ResultRecord& r) noexcept {
    Packed a{};
    a[0] = r.valid ? 1 : 0;
    a[1] = r.source;
    a[2] = r.gainDb;
    a[3] = r.preLufs;
    a[4] = r.predictedLufs;
    a[5] = r.truePeakDb;
    a[6] = r.maxGrDb;
    a[7] = r.durationSec;
    a[8] = r.fs;
    a[9] = r.channels;
    a[10] = double(r.hash & 0xffffffffu);
    a[11] = double(r.hash >> 32);
    a[12] = r.warnings;
    a[13] = r.learnFlags;
    a[14] = r.unreached ? 1 : 0;
    return a;
}

inline bool integral(double v, double lo, double hi) noexcept { return std::isfinite(v) && v >= lo && v <= hi && v == std::floor(v); }
inline bool inRange(double v, double lo, double hi) noexcept { return std::isfinite(v) && v >= lo && v <= hi; }

// Rejects anything a well-formed writer never produces (non-finite, out of range, impossible combinations).
inline bool unpack(const Packed& a, ResultRecord& out) noexcept {
    if (!integral(a[0], 0, 1)) return false;
    ResultRecord r;
    if (a[0] == 0) {
        out = r;
        return true;
    }
    if (!integral(a[1], 1, 2) || !inRange(a[2], -80, 80) || !inRange(a[3], -200, 100) || !inRange(a[4], -200, 100) || !inRange(a[5], -200, 100) || !inRange(a[6], 0, 200) ||
        !inRange(a[7], 0, 1e7) || !inRange(a[8], 8000, 768000) || !integral(a[9], 1, 2) || !integral(a[10], 0, 4294967295.0) || !integral(a[11], 0, 4294967295.0) ||
        !integral(a[12], 0, 65535) || !integral(a[13], 0, 65535) || !integral(a[14], 0, 1))
        return false;
    r.valid = true;
    r.source = int(a[1]);
    r.gainDb = a[2];
    r.preLufs = a[3];
    r.predictedLufs = a[4];
    r.truePeakDb = a[5];
    r.maxGrDb = a[6];
    r.durationSec = a[7];
    r.fs = a[8];
    r.channels = int(a[9]);
    r.hash = uint64_t(a[10]) | (uint64_t(a[11]) << 32);
    r.warnings = uint32_t(a[12]);
    r.learnFlags = uint32_t(a[13]);
    r.unreached = a[14] != 0;
    out = r;
    return true;
}

inline ResultRecord recordFrom(const AnalysisResult& a, int source, uint64_t hash, int channels, double fs, uint32_t learnFlags) noexcept {
    ResultRecord r;
    r.valid = a.ok;
    r.source = source;
    r.gainDb = std::clamp(a.gainDb, -80.0, 80.0);
    r.preLufs = std::clamp(a.preLufs, -200.0, 100.0);
    r.predictedLufs = std::clamp(a.predictedLufs, -200.0, 100.0);
    r.truePeakDb = std::clamp(a.truePeakDb, -200.0, 100.0);
    r.maxGrDb = std::clamp(a.maxGrDb, 0.0, 200.0) + 0.0;  // + 0.0 turns -0 into +0
    r.durationSec = std::clamp(a.durationSec, 0.0, 1e7);
    r.fs = fs;
    r.channels = channels;
    r.hash = hash;
    r.warnings = a.warnings;
    r.learnFlags = learnFlags;
    r.unreached = a.unreached;
    return r;
}
}  // namespace vd
