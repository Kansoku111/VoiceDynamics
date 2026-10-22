#include "wavio.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace vd {
namespace {
uint32_t u32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t u16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }
}  // namespace

bool wavInfo(const std::filesystem::path& path, WavInfo& info, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "ファイルを開けません。";
        return false;
    }
    f.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(f.tellg());
    f.seekg(0);
    unsigned char head[12];
    if (!f.read(reinterpret_cast<char*>(head), 12) || std::memcmp(head + 8, "WAVE", 4) || (std::memcmp(head, "RIFF", 4) && std::memcmp(head, "RF64", 4))) {
        error = "WAVファイルではありません。";
        return false;
    }
    if (!std::memcmp(head, "RF64", 4)) {
        error = "RF64形式のWAVには対応していません。";
        return false;
    }
    WavInfo r;
    bool haveFormat = false, haveData = false;
    uint64_t position = 12, dataBytes = 0;
    while (position + 8 <= size) {
        unsigned char chunk[8];
        f.seekg(std::streamoff(position));
        if (!f.read(reinterpret_cast<char*>(chunk), 8)) break;
        uint64_t length = u32(chunk + 4);
        if (!std::memcmp(chunk, "fmt ", 4)) {
            std::vector<unsigned char> body(size_t(std::min<uint64_t>(length, 64)));
            if (body.size() < 16 || !f.read(reinterpret_cast<char*>(body.data()), std::streamsize(body.size()))) {
                error = "fmtチャンクが壊れています。";
                return false;
            }
            r.format = u16(body.data());
            r.channels = u16(body.data() + 2);
            r.rate = u32(body.data() + 4);
            r.bits = u16(body.data() + 14);
            if (r.format == 0xFFFE && body.size() >= 26) r.format = u16(body.data() + 24);
            haveFormat = true;
        } else if (!std::memcmp(chunk, "data", 4)) {
            r.dataOffset = position + 8;
            if (length == 0 || length == 0xFFFFFFFFull || r.dataOffset + length > size) length = size - r.dataOffset;
            dataBytes = length;
            haveData = true;
            if (haveFormat) break;
        }
        position += 8 + length + (length & 1);
    }
    if (!haveFormat || !haveData) {
        error = "fmtまたはdataチャンクが見つかりません。";
        return false;
    }
    const bool pcm = r.format == 1 && (r.bits == 8 || r.bits == 16 || r.bits == 24 || r.bits == 32);
    const bool real = r.format == 3 && (r.bits == 32 || r.bits == 64);
    if (!pcm && !real) {
        error = "対応していない形式です（PCM 8/16/24/32 bit、float 32/64 bitに対応）。";
        return false;
    }
    if (r.channels < 1 || r.channels > 64 || !(r.rate >= 8000 && r.rate <= 384000)) {
        error = "チャンネル数またはサンプルレートが範囲外です。";
        return false;
    }
    r.frames = dataBytes / (uint64_t(r.bits / 8) * uint64_t(r.channels));
    info = r;
    return true;
}

bool wavRead(const std::filesystem::path& path, int busChannels, WavInfo& info, ChunkedPcm& pcm, std::string& error,
             const std::function<bool(double)>& progress) {
    pcm.clear();
    if (!wavInfo(path, info, error)) return false;
    if (info.frames == 0) {
        error = "音声データが空です。";
        return false;
    }
    busChannels = std::clamp(busChannels, 1, 2);
    if (info.frames * uint64_t(busChannels) * 4 > ChunkedPcm::MaxBytes) {
        error = "素材が長すぎます（約2時間まで）。";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    f.seekg(std::streamoff(info.dataOffset));
    pcm.configure(busChannels);
    std::vector<float> staged((size_t(1) << 15) * size_t(busChannels));
    const int width = info.bits / 8, n = info.channels;
    const size_t chunkFrames = 1 << 15;
    std::vector<unsigned char> raw(chunkFrames * size_t(n) * size_t(width));
    uint64_t done = 0;
    while (done < info.frames) {
        const size_t frames = size_t(std::min<uint64_t>(chunkFrames, info.frames - done));
        if (!f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(frames * size_t(n) * size_t(width)))) {
            error = "ファイルの読み込みに失敗しました。";
            pcm.clear();
            return false;
        }
        for (size_t i = 0; i < frames; ++i) {
            double first = 0, second = 0;
            for (int c = 0; c < std::min(n, 2); ++c) {
                const unsigned char* p = &raw[(i * size_t(n) + size_t(c)) * size_t(width)];
                double v = 0;
                if (info.format == 3 && info.bits == 32) {
                    float x;
                    std::memcpy(&x, p, 4);
                    v = x;
                } else if (info.format == 3) {
                    std::memcpy(&v, p, 8);
                } else if (info.bits == 8) {
                    v = (int(p[0]) - 128) / 128.0;
                } else if (info.bits == 16) {
                    v = int16_t(u16(p)) / 32768.0;
                } else if (info.bits == 24) {
                    v = (int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8) / 8388608.0;
                } else {
                    v = int32_t(u32(p)) / 2147483648.0;
                }
                if (!std::isfinite(v)) v = 0;
                (c == 0 ? first : second) = v;
            }
            float* out = &staged[i * size_t(busChannels)];
            if (busChannels == 1) {
                out[0] = float(n >= 2 ? (first + second) * 0.5 : first);
            } else {
                out[0] = float(first);
                out[1] = float(n >= 2 ? second : first);
            }
        }
        if (!pcm.append(staged.data(), frames)) {
            error = "メモリーが足りません。";
            pcm.clear();
            return false;
        }
        done += frames;
        if (progress && !progress(double(done) / double(info.frames))) {
            error = "中止しました。";
            pcm.clear();
            return false;
        }
    }
    return true;
}

namespace {
bool writeHeader(std::ofstream& f, double rate, int channels, int bits, int format, uint64_t dataBytes) {
    auto put32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    put32(uint32_t(36 + dataBytes));
    f.write("WAVEfmt ", 8);
    put32(16);
    put16(uint16_t(format));
    put16(uint16_t(channels));
    put32(uint32_t(rate));
    put32(uint32_t(rate) * uint32_t(channels) * uint32_t(bits / 8));
    put16(uint16_t(channels * bits / 8));
    put16(uint16_t(bits));
    f.write("data", 4);
    put32(uint32_t(dataBytes));
    return bool(f);
}
}  // namespace

bool wavWriteFloat(const std::filesystem::path& path, double rate, int channels, const std::vector<float>& s, std::string& error) {
    std::ofstream f(path, std::ios::binary);
    if (!f || !writeHeader(f, rate, channels, 32, 3, s.size() * 4)) {
        error = "書き込めません。";
        return false;
    }
    f.write(reinterpret_cast<const char*>(s.data()), std::streamsize(s.size() * 4));
    return bool(f);
}

bool wavWritePcm(const std::filesystem::path& path, double rate, int channels, int bits, const std::vector<float>& s, std::string& error) {
    std::ofstream f(path, std::ios::binary);
    if (!f || (bits != 16 && bits != 24) || !writeHeader(f, rate, channels, bits, 1, s.size() * size_t(bits / 8))) {
        error = "書き込めません。";
        return false;
    }
    for (float v : s) {
        const double x = std::clamp(double(v), -1.0, 1.0);
        if (bits == 16) {
            const int16_t q = int16_t(std::lround(x * 32767.0));
            f.write(reinterpret_cast<const char*>(&q), 2);
        } else {
            const int32_t q = int32_t(std::lround(x * 8388607.0));
            const unsigned char b[3]{uint8_t(q & 255), uint8_t((q >> 8) & 255), uint8_t((q >> 16) & 255)};
            f.write(reinterpret_cast<const char*>(b), 3);
        }
    }
    return bool(f);
}
}  // namespace vd
