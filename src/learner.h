#pragma once
// Playback learning. While the track plays, the audio thread hands the signal that reaches the
// correction gain (after gate, leveler and compressor) to a background thread through a lock-free FIFO.
// The background thread measures it (BS.1770 blocks, true peak) and keeps it; when the learning ends it
// searches the gain with the plug-in's own limiter code (analyzer.h) and publishes the result.
//
// Threads: the UI thread calls arm/finish/cancel/release/status; the audio thread calls capturing() and
// block() only (atomics and one FIFO write, no allocation, no lock); the worker thread does the rest.
#include "analyzer.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vd {
// Things that make a learning pass unreliable (shown as warnings; bit flags).
enum LearnFlag : uint32_t {
    LfBackJump = 1,        // the playback position went back (rewind, loop)
    LfForwardJump = 2,     // the playback position skipped forward (fast forward, locate)
    LfDropped = 4,         // audio did not fit into the FIFO and was lost
    LfSettingsChanged = 8, // gate, leveler or compressor settings changed while learning
    LfRateChanged = 16,    // the sample rate changed
    LfNoTransport = 32,    // the host gave no position information: jumps cannot be detected
    LfTruncated = 64,      // longer than the memory limit: the rest was not kept
    LfStartedMidway = 128  // learning began away from the start of the project
};

class Learner {
public:
    enum State : int { Idle, Armed, Capturing, Computing, Done, Failed };
    struct Status {
        State state = Idle;
        double capturedSeconds = 0, progress = 0, startSeconds = 0;
        uint32_t flags = 0;
        uint64_t droppedFrames = 0;
        std::string message;  // reason when Failed
    };
    struct Result {
        AnalysisResult analysis;
        uint32_t flags = 0;
        uint64_t settingsHash = 0;
        int channels = 2;
        double fs = 48000, capturedSeconds = 0;
    };
    using SettingsProvider = std::function<Settings()>;

    Learner() = default;
    ~Learner() { release(); }
    Learner(const Learner&) = delete;
    Learner& operator=(const Learner&) = delete;

    // ---- UI thread ----
    // Allocates the FIFO (fifoFrames of two channels, rounded up to a power of two) and starts waiting for audio.
    // provider() is called on the worker thread when the learning ends to read the settings then in force.
    bool arm(SettingsProvider provider, uint32_t fifoFrames = 1u << 20);
    void finish();                              // Armed/Capturing -> Computing
    void cancel();                              // back to Idle, result discarded
    void fail(const std::string& message);      // Failed (e.g. the sample rate changed)
    void release();                             // stop the worker and free the FIFO (the plug-in is not processing)
    // Done -> Computing again over the kept recording, with the settings then in force (a new target or limiter setting).
    // Refused when nothing is kept or the gate/leveler/compressor no longer match the recording (preHash).
    bool retarget(uint64_t preHash);
    State state() const noexcept { return State(state_.load()); }
    Status status() const;
    bool result(Result& out) const;             // true when a result (ok or not) is available
    void setAutoStop(bool on) noexcept { autoStop_.store(on); }
    // Called on the worker thread when a session ends by itself in Done or Failed (not after cancel/fail/release). Set it before arm().
    void setCompletion(std::function<void()> f) {
        std::lock_guard<std::mutex> lock(mutex_);
        completion_ = std::move(f);
    }

    // ---- audio thread ----
    bool capturing() const noexcept {
        const int s = state_.load(std::memory_order_acquire);
        return s == Armed || s == Capturing;
    }
    // One processed block. `pre` (interleaved, frames x channels) is the signal before the correction gain; it is
    // ignored when !playing. position is the host's project time in samples when haveTime.
    // mayWait is for the host's offline (export) mode only: when the FIFO is full, wait (at most 2 s) for the worker instead of losing audio.
    void block(const float* pre, uint32_t frames, int channels, double fs, bool playing, bool haveTime, int64_t position, uint64_t preHash,
               bool mayWait = false) noexcept;

    // For tests and diagnostics
    uint32_t fifoCapacityFrames() const noexcept { return capacityFrames_; }

private:
    void workerMain();
    void completed();
    void setState(State s) noexcept { state_.store(int(s), std::memory_order_release); }
    bool write(const float* data, uint32_t frames, int channels) noexcept;
    size_t read(float* out, size_t maxFloats) noexcept;

    std::atomic<int> state_{Idle};
    std::atomic<uint32_t> flags_{0};
    std::atomic<uint64_t> dropped_{0}, capturedFrames_{0};
    std::atomic<int> channels_{2};
    std::atomic<double> fs_{48000}, progress_{0}, startSeconds_{0};
    std::atomic<bool> autoStop_{false}, autoFinish_{false}, inBlock_{false}, stop_{false}, workerIdle_{false};
    std::atomic<bool> fresh_{false}, kept_{false};  // fresh_: the worker drops the previous recording; kept_: a Done session's recording is still there
    std::atomic<uint64_t> startHash_{0};

    // FIFO (single producer: audio thread, single consumer: worker)
    std::unique_ptr<float[]> fifo_;
    uint32_t capacityFrames_ = 0;
    size_t capacityFloats_ = 0, mask_ = 0;
    std::atomic<uint64_t> writePos_{0}, readPos_{0};

    // audio-thread-only tracking
    bool wasPlaying_ = false, havePosition_ = false, started_ = false;
    int64_t lastPosition_ = 0, lastFrames_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    SettingsProvider provider_;
    std::function<void()> completion_;
    Result result_;
    bool haveResult_ = false;
    std::string message_;
};
}  // namespace vd
