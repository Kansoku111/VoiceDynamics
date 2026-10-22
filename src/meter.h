#pragma once
// Processor -> controller transport (lock-free, fixed size) and the controller's side of the editor.
#include "learner.h"
#include "result.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace vd {
constexpr char MeterRequest[] = "VoiceDynamics.MeterRequest.1", MeterReply[] = "VoiceDynamics.MeterReply.1";
constexpr char CommandMessage[] = "VoiceDynamics.Command.1";    // int "command" (Command)
constexpr char AnalysisMessage[] = "VoiceDynamics.Analysis.1";  // binary "record" (Packed), string "name"
enum Command : int { CmdLearnStart = 1, CmdLearnFinish = 2, CmdLearnCancel = 3, CmdClearResult = 4, CmdResetMeter = 5 };

enum MeterField : int {
    MGeneration, MRate, MChannels, MLatency, MBypass, MOffline,
    MInputPeak, MOutputPeak,                    // sample peaks since the previous frame, linear
    MMomentary, MShortTerm, MIntegrated, MOutTruePeak,  // output loudness (LUFS) and the output true peak (dBTP) since reset
    MGateDb, MLevelerDb, MCompDb, MLimiterDb,   // gain changes in dB over the interval (gate/comp/limiter: the deepest, <= 0)
    MGainDb,                                    // correction gain in force (automatic + fine offset)
    MLearnState, MLearnSeconds, MLearnProgress, MLearnStart, MLearnFlags, MLearnDropped,
    MRecValid, MRecSource, MRecGain, MRecPre, MRecPred, MRecTp, MRecMaxGr, MRecDuration, MRecWarnings, MRecFlags, MRecUnreached, MRecStale,
    MeterSize
};
using MeterFrame = std::array<double, MeterSize>;
constexpr double NoLevel = -200.0;

inline bool validMeter(const MeterFrame& f) noexcept {
    for (double v : f)
        if (!std::isfinite(v) || std::abs(v) > 1e12) return false;
    if (f[MGeneration] < 1 || f[MRate] < 8000 || f[MRate] > 768000 || (f[MChannels] != 1 && f[MChannels] != 2)) return false;
    if (f[MInputPeak] < 0 || f[MOutputPeak] < 0 || f[MLearnState] < 0 || f[MLearnState] > double(Learner::Failed) || f[MLearnProgress] < 0 || f[MLearnProgress] > 1) return false;
    return true;
}

// Single publisher, lock-free. A reader never accepts values crossing a publication.
class Mailbox {
    std::atomic<uint64_t> sequence_{0};
    std::array<std::atomic<double>, MeterSize> values_{};

public:
    Mailbox() noexcept {
        for (auto& v : values_) v.store(0);
    }
    void publish(const MeterFrame& f) noexcept {
        sequence_.fetch_add(1);
        for (size_t i = 0; i < f.size(); ++i) values_[i].store(f[i]);
        sequence_.fetch_add(1);
    }
    bool read(MeterFrame& f) const noexcept {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto before = sequence_.load();
            if (before == 0 || (before & 1)) continue;
            MeterFrame copy{};
            for (size_t i = 0; i < copy.size(); ++i) copy[i] = values_[i].load();
            if (before == sequence_.load()) {
                f = copy;
                return true;
            }
        }
        return false;
    }
};
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free, "Meter transport must not lock audio");

struct FileStatus {
    bool running = false, done = false, ok = false;
    double progress = 0;
    std::u16string message;  // error or a short summary
    std::u16string name;     // file name being or last analysed
};

// What the editor needs from the controller besides parameters.
class EditorLink {
public:
    virtual bool pollMeter(MeterFrame&) = 0;
    virtual std::u16string note() = 0;       // the processor's last explanation (why learning failed)
    virtual void command(Command) = 0;
    virtual bool analyzeFile(const std::wstring& path) = 0;  // auxiliary: analyse a WAV instead of playing the track
    virtual FileStatus fileStatus() = 0;
    virtual void cancelFile() = 0;

protected:
    ~EditorLink() = default;
};
}  // namespace vd
