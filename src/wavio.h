#pragma once
// WAV reading for the analysis (non-real-time only) and a small writer for tools and tests.
// PCM 8/16/24/32 bit, IEEE float 32/64 bit, WAVE_FORMAT_EXTENSIBLE. RF64 and compressed formats are refused.
#include "pcm.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vd {
struct WavInfo {
    double rate = 0;
    int channels = 0, bits = 0, format = 0;
    uint64_t frames = 0, dataOffset = 0;
};
bool wavInfo(const std::filesystem::path& path, WavInfo& info, std::string& error);
// Reads the whole file as interleaved float with `busChannels` channels (1 or 2):
// same count copies, mono -> stereo duplicates, stereo -> mono averages, more channels use the first two.
// progress(fraction) returns false to cancel.
bool wavRead(const std::filesystem::path& path, int busChannels, WavInfo& info, ChunkedPcm& pcm, std::string& error,
             const std::function<bool(double)>& progress = {});
// 32-bit float WAV (tests and tools).
bool wavWriteFloat(const std::filesystem::path& path, double rate, int channels, const std::vector<float>& interleaved, std::string& error);
// 16-bit PCM WAV with the given bit depth (16/24) from float, for tests.
bool wavWritePcm(const std::filesystem::path& path, double rate, int channels, int bits, const std::vector<float>& interleaved, std::string& error);
}  // namespace vd
