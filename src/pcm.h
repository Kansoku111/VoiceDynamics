#pragma once
// Interleaved float audio kept in fixed-size chunks, so a long recording never needs one huge block
// of contiguous memory. Not for the audio thread (appending allocates).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vd {
class ChunkedPcm {
public:
    static constexpr size_t ChunkBits = 18, ChunkFrames = size_t(1) << ChunkBits, Mask = ChunkFrames - 1;
    // Largest total size the analysis accepts (about 2 hours of 48 kHz stereo).
    static constexpr uint64_t MaxBytes = 3ull << 30;

    void configure(int channels) {
        clear();
        channels_ = channels < 1 ? 1 : channels > 2 ? 2 : channels;
    }
    void clear() {
        chunks_.clear();
        frames_ = 0;
    }
    int channels() const noexcept { return channels_; }
    size_t frames() const noexcept { return frames_; }
    const float* frame(size_t i) const noexcept { return &chunks_[i >> ChunkBits][(i & Mask) * size_t(channels_)]; }
    float* frame(size_t i) noexcept { return &chunks_[i >> ChunkBits][(i & Mask) * size_t(channels_)]; }
    // Returns false (and stores nothing) when the size limit would be exceeded or memory runs out.
    bool append(const float* interleaved, size_t frames) {
        if ((uint64_t(frames_) + frames) * uint64_t(channels_) * 4 > MaxBytes) return false;
        try {
            size_t done = 0;
            while (done < frames) {
                if ((frames_ & Mask) == 0 && (frames_ >> ChunkBits) >= chunks_.size())
                    chunks_.emplace_back(new float[ChunkFrames * size_t(channels_)]);
                const size_t room = ChunkFrames - (frames_ & Mask), n = frames - done < room ? frames - done : room;
                float* target = &chunks_[frames_ >> ChunkBits][(frames_ & Mask) * size_t(channels_)];
                for (size_t k = 0; k < n * size_t(channels_); ++k) target[k] = interleaved[done * size_t(channels_) + k];
                frames_ += n;
                done += n;
            }
        } catch (...) {
            return false;
        }
        return true;
    }
    bool resize(size_t frames) {  // zero-filled
        clear();
        std::vector<float> zeros(ChunkFrames * size_t(channels_), 0.f);
        for (size_t done = 0; done < frames;) {
            const size_t n = frames - done < ChunkFrames ? frames - done : ChunkFrames;
            if (!append(zeros.data(), n)) return false;
            done += n;
        }
        return true;
    }

private:
    std::vector<std::unique_ptr<float[]>> chunks_;
    size_t frames_ = 0;
    int channels_ = 2;
};
}  // namespace vd
