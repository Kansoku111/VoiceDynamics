#pragma once
// Gain search for the target LUFS-I. It runs the plug-in's own limiter code over the signal that
// reaches the correction gain (gate/leveler/compressor output) and measures what the plug-in will
// really output, so the prediction and the output agree. Non-real-time (worker threads only).
//
// Two ways to obtain that signal:
//  - playback learning: the plug-in captures it while the track plays (learner.h), then calls solveGain();
//  - a WAV file: analyzeFile() runs the same gate/leveler/compressor code over the file first.
#include "parameters.h"
#include "pcm.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vd {
enum Warning : uint32_t {
    WarnUnreached = 1,   // the target could not be reached within 0.1 LU
    WarnHighGr = 2,      // reserved (the editor judges this against the live setting)
    WarnTruePeak = 4,    // the output true peak is above the ceiling
    WarnShort = 8,       // too short for a stable Integrated value
    WarnLimiterOff = 16  // limiter is off and the peaks pass the ceiling
};
constexpr double ToleranceLu = 0.1;   // required agreement between prediction and target
constexpr double ShortSeconds = 3.0;

struct AnalysisResult {
    bool ok = false, cancelled = false, unreached = false;
    std::string error;                // Japanese message when !ok
    double gainDb = 0;                // the automatic correction gain (without the fine offset)
    double preLufs = -200;            // LUFS-I after gate/leveler/compressor, before the gain
    double prePeakDb = -200;          // true peak at the same point
    double predictedLufs = -200;      // LUFS-I of the plug-in output with gainDb
    double truePeakDb = -200;         // true peak of that output
    double maxGrDb = 0;               // deepest limiter gain reduction, positive dB
    double durationSec = 0;
    double errorLu = 0;               // predictedLufs - target
    uint32_t warnings = 0;
    int evaluations = 0;              // limiter passes that were needed (0 = the limiter never engages)
};

using Progress = std::function<bool(double)>;  // fraction 0..1; return false to cancel

// What the gate/leveler/compressor part of a signal measures (the 400 ms block powers and the true peak).
struct PreMeasurement {
    std::vector<double> blocks;
    double peak = 0;  // linear
};

// `pre`: the signal after the compressor, before the gain, aligned so that frame 0 is the first captured frame.
AnalysisResult solveGain(const Settings& settings, int channels, double fs, const ChunkedPcm& pre, const PreMeasurement& measured, const Progress& progress = {});

// Runs gate/leveler/compressor over `pcm` (overwritten with their output) and then solveGain().
AnalysisResult analyzeFile(const Settings& settings, int channels, double fs, ChunkedPcm& pcm, const Progress& progress = {});
}  // namespace vd
