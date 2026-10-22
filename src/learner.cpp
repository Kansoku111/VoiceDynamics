#include "learner.h"
#include "loudness.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vd {
namespace {
size_t roundUpPow2(size_t v) {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}
}  // namespace

// ---- UI thread -------------------------------------------------------------------------------------
bool Learner::arm(SettingsProvider provider, uint32_t fifoFrames) {
    const int s = state();
    if (s == Armed || s == Capturing || s == Computing) return false;
    // The worker may still be winding down a cancelled or finished session.
    for (int i = 0; i < 1000 && worker_.joinable() && !workerIdle_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (worker_.joinable() && !workerIdle_.load()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fifo_) {
        capacityFrames_ = uint32_t(roundUpPow2(std::max<size_t>(fifoFrames, 1u << 12)));
        capacityFloats_ = size_t(capacityFrames_) * 2;
        mask_ = capacityFloats_ - 1;
        fifo_.reset(new float[capacityFloats_]);
    }
    writePos_ = 0;
    readPos_ = 0;
    flags_ = 0;
    dropped_ = 0;
    capturedFrames_ = 0;
    progress_ = 0;
    startSeconds_ = 0;
    autoFinish_ = false;
    inBlock_ = false;
    started_ = false;
    wasPlaying_ = false;
    havePosition_ = false;
    haveResult_ = false;
    result_ = Result{};
    message_.clear();
    kept_ = false;
    fresh_ = true;
    provider_ = std::move(provider);
    if (!worker_.joinable()) {
        stop_ = false;
        worker_ = std::thread([this] { workerMain(); });
    }
    setState(Armed);
    wake_.notify_all();
    return true;
}

void Learner::finish() {
    for (int s : {int(Capturing), int(Armed)}) {
        int expected = s;
        if (state_.compare_exchange_strong(expected, int(Computing))) {
            wake_.notify_all();
            return;
        }
    }
}

bool Learner::retarget(uint64_t preHash) {
    if (!kept_.load() || preHash != startHash_.load()) return false;
    progress_ = 0;
    int expected = Done;
    if (!state_.compare_exchange_strong(expected, int(Computing))) return false;
    wake_.notify_all();
    return true;
}

void Learner::cancel() {
    kept_ = false;
    setState(Idle);
    wake_.notify_all();
}

void Learner::fail(const std::string& message) {
    const int s = state();
    if (s != Armed && s != Capturing && s != Computing) return;
    kept_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = message;
        result_ = Result{};
        result_.analysis.error = message;
        result_.flags = flags_.load();
        haveResult_ = true;
    }
    setState(Failed);
    wake_.notify_all();
}

void Learner::release() {
    setState(Idle);
    stop_ = true;
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    fifo_.reset();
    capacityFrames_ = 0;
    capacityFloats_ = mask_ = 0;
    writePos_ = readPos_ = 0;
}

Learner::Status Learner::status() const {
    Status st;
    st.state = state();
    st.flags = flags_.load();
    st.droppedFrames = dropped_.load();
    st.progress = progress_.load();
    st.startSeconds = startSeconds_.load();
    const double fs = fs_.load();
    st.capturedSeconds = fs > 0 ? double(capturedFrames_.load()) / fs : 0;
    if (st.state == Failed) {
        std::lock_guard<std::mutex> lock(mutex_);
        st.message = message_;
    }
    return st;
}

bool Learner::result(Result& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!haveResult_) return false;
    out = result_;
    return true;
}

// ---- audio thread ----------------------------------------------------------------------------------
bool Learner::write(const float* data, uint32_t frames, int channels) noexcept {
    const size_t n = size_t(frames) * size_t(channels);
    const uint64_t w = writePos_.load(std::memory_order_relaxed), r = readPos_.load(std::memory_order_acquire);
    if (capacityFloats_ - size_t(w - r) < n) return false;
    const size_t at = size_t(w) & mask_, first = std::min(n, capacityFloats_ - at);
    std::memcpy(&fifo_[at], data, first * sizeof(float));
    if (first < n) std::memcpy(&fifo_[0], data + first, (n - first) * sizeof(float));
    writePos_.store(w + n, std::memory_order_release);
    return true;
}

void Learner::block(const float* pre, uint32_t frames, int channels, double fs, bool playing, bool haveTime, int64_t position, uint64_t preHash, bool mayWait) noexcept {
    inBlock_.store(true);
    if (capturing()) {
        if (!playing) {
            if (wasPlaying_ && autoStop_.load(std::memory_order_relaxed) && state_.load() == Capturing) autoFinish_.store(true);
            wasPlaying_ = false;
        } else {
            int expected = Armed;
            if (state_.compare_exchange_strong(expected, Capturing)) {
                channels_.store(channels);
                fs_.store(fs);
                startHash_.store(preHash);
                if (haveTime) {
                    startSeconds_.store(double(position) / fs);
                    if (position > int64_t(fs * 0.25)) flags_.fetch_or(LfStartedMidway);
                }
            }
            if (state_.load() == Capturing) {
                if (fs != fs_.load(std::memory_order_relaxed) || channels != channels_.load(std::memory_order_relaxed)) flags_.fetch_or(LfRateChanged);
                if (preHash != startHash_.load(std::memory_order_relaxed)) flags_.fetch_or(LfSettingsChanged);
                if (haveTime) {
                    if (havePosition_) {
                        const int64_t expectedPosition = lastPosition_ + lastFrames_;
                        if (position - expectedPosition > 2) flags_.fetch_or(LfForwardJump);
                        else if (expectedPosition - position > 2) flags_.fetch_or(LfBackJump);
                    }
                    lastPosition_ = position;
                    lastFrames_ = frames;
                    havePosition_ = true;
                } else {
                    flags_.fetch_or(LfNoTransport);
                }
                wasPlaying_ = true;
                bool written = write(pre, frames, channels);
                if (!written && mayWait) {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    while (!written && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        written = write(pre, frames, channels);
                    }
                }
                if (!written) {
                    dropped_.fetch_add(frames, std::memory_order_relaxed);
                    flags_.fetch_or(LfDropped);
                }
            }
        }
    }
    inBlock_.store(false);
}

void Learner::completed() {
    std::function<void()> f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        f = completion_;
    }
    if (f) f();
}

// ---- worker thread ---------------------------------------------------------------------------------
size_t Learner::read(float* out, size_t maxFloats) noexcept {
    const uint64_t r = readPos_.load(std::memory_order_relaxed), w = writePos_.load(std::memory_order_acquire);
    const size_t n = std::min(maxFloats, size_t(w - r));
    if (!n) return 0;
    const size_t at = size_t(r) & mask_, first = std::min(n, capacityFloats_ - at);
    std::memcpy(out, &fifo_[at], first * sizeof(float));
    if (first < n) std::memcpy(out + first, &fifo_[0], (n - first) * sizeof(float));
    readPos_.store(r + n, std::memory_order_release);
    return n;
}

void Learner::workerMain() {
    ChunkedPcm pcm;
    LoudnessMeter meter;
    TruePeakDetector detector;
    PreMeasurement m;
    std::vector<float> buffer(8192);
    bool initialized = false, truncated = false, flushed = false;
    int channels = 2;
    double fs = 48000;
    uint64_t frames = 0;
    auto drop = [&] {
        pcm.clear();
        m = PreMeasurement{};
        initialized = truncated = flushed = false;
        frames = 0;
    };
    while (!stop_.load()) {
        if (fresh_.exchange(false) && initialized) drop();
        const int s = state();
        if (s != Armed && s != Capturing && s != Computing) {
            // A finished session keeps its recording so that a new target can be solved without playing again.
            if (initialized && s != Done) {
                kept_ = false;
                drop();
            }
            std::unique_lock<std::mutex> lock(mutex_);
            workerIdle_.store(true);
            wake_.wait_for(lock, std::chrono::milliseconds(50), [&] {
                const int now = state();
                return stop_.load() || now == Armed || now == Capturing || now == Computing;
            });
            workerIdle_.store(false);
            continue;
        }
        if (autoFinish_.exchange(false) && s == Capturing) finish();
        const size_t floatsPerFrame = size_t(std::clamp(channels_.load(), 1, 2));
        const size_t got = read(buffer.data(), buffer.size() - buffer.size() % floatsPerFrame);
        if (got) {
            if (!initialized) {
                channels = std::clamp(channels_.load(), 1, 2);
                fs = fs_.load();
                pcm.configure(channels);
                meter.prepare(fs, channels, false);
                meter.setBlockSink(&m.blocks);
                detector.prepare(fs, channels);
                initialized = true;
            }
            const size_t n = got / size_t(channels);
            if (!truncated) {
                if (!pcm.append(buffer.data(), n)) {
                    truncated = true;
                    flags_.fetch_or(LfTruncated);
                } else {
                    double f[2]{0, 0};
                    for (size_t i = 0; i < n; ++i) {
                        for (int c = 0; c < channels; ++c) f[c] = double(buffer[i * size_t(channels) + size_t(c)]);
                        meter.push(f);
                        m.peak = std::max(m.peak, detector.push(f, m.peak));
                    }
                    frames += n;
                    capturedFrames_.store(frames);
                }
            }
            continue;
        }
        if (s == Computing && !inBlock_.load() && readPos_.load() == writePos_.load()) {
            const double zero[2]{0, 0};
            if (initialized && !flushed) {
                for (int i = 0; i < detector.delay(); ++i) m.peak = std::max(m.peak, detector.push(zero, m.peak));
                flushed = true;
            }
            if (initialized && frames > 0) {
                const Settings settings = provider_ ? provider_() : Settings{};
                PreMeasurement measured = m;
                if (settings.dualMono && channels == 1)
                    for (auto& b : measured.blocks) b *= 2.0;  // the blocks were measured with weight 1
                AnalysisResult r = solveGain(settings, channels, fs, pcm, measured, [this](double f) {
                    progress_.store(f);
                    return state() == Computing;
                });
                int expected = Computing;
                if (!r.cancelled) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        result_ = Result{};
                        result_.analysis = r;
                        result_.flags = flags_.load();
                        result_.settingsHash = analysisHash(settings, channels);
                        result_.channels = channels;
                        result_.fs = fs;
                        result_.capturedSeconds = double(frames) / fs;
                        message_ = r.error;
                        haveResult_ = true;
                    }
                    if (state_.compare_exchange_strong(expected, r.ok ? int(Done) : int(Failed))) {
                        kept_ = r.ok;
                        completed();
                    }
                }
            } else {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    message_ = "音が入ってきませんでした。トラックを再生しながら学習してください。";
                    result_ = Result{};
                    result_.analysis.error = message_;
                    result_.flags = flags_.load();
                    haveResult_ = true;
                }
                int expected = Computing;
                if (state_.compare_exchange_strong(expected, int(Failed))) completed();
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}  // namespace vd
