#pragma once
#include "parameters.h"
#include "result.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"

// Component state format (little endian):
//   u32 magic 'VDYN', u32 version, u32 count, count x { u32 id, f64 value }
// IDs below ParamCount are parameters (normalized values; the mappings in parameters.h are frozen with the IDs).
// IDs from ResultBase are the learned result (result.h pack() order, plain values): ResultBase + 0 is the
// "a result exists" flag. Readers ignore unknown IDs and reject non-finite or out-of-range values, duplicate
// IDs and truncated data, and then leave the current state untouched.
// Version history: 1 = VoiceDynamics 0.1.0.
namespace vd {
constexpr Steinberg::uint32 StateMagic = 0x4E594456, StateVersion = 1, ResultBase = 1000;

struct State {
    Values values = defaults();
    ResultRecord record;
};

inline bool readState(Steinberg::IBStream* stream, State& state) {
    using namespace Steinberg;
    if (!stream) return false;
    IBStreamer s(stream, kLittleEndian);
    uint32 magic = 0, version = 0, count = 0;
    if (!s.readInt32u(magic) || !s.readInt32u(version) || !s.readInt32u(count)) return false;
    if (magic != StateMagic || version < 1 || count > 4096) return false;
    State result;
    std::array<bool, ParamCount> seen{};
    Packed packed{};
    std::array<bool, RecordFields> seenRecord{};
    for (uint32 i = 0; i < count; ++i) {
        uint32 id = 0;
        double value = 0;
        if (!s.readInt32u(id) || !s.readDouble(value) || !std::isfinite(value)) return false;
        if (id < ParamCount) {
            if (value < 0 || value > 1 || seen[id]) return false;
            seen[id] = true;
            result.values[id] = value;
        } else if (id >= ResultBase && id < ResultBase + RecordFields) {
            if (seenRecord[id - ResultBase]) return false;
            seenRecord[id - ResultBase] = true;
            packed[id - ResultBase] = value;
        }
    }
    if (seenRecord[0]) {
        for (int k = 1; k < RecordFields && packed[0] != 0; ++k)
            if (!seenRecord[size_t(k)]) return false;  // a result with missing fields is damaged
        if (!unpack(packed, result.record)) return false;
    }
    state = result;
    return true;
}

template <class Getter>
Steinberg::tresult writeState(Steinberg::IBStream* stream, Getter normalizedValue, const ResultRecord& record) {
    using namespace Steinberg;
    if (!stream) return kInvalidArgument;
    IBStreamer s(stream, kLittleEndian);
    const uint32 resultCount = record.valid ? RecordFields : 1;
    if (!s.writeInt32u(StateMagic) || !s.writeInt32u(StateVersion) || !s.writeInt32u(ParamCount + resultCount)) return kResultFalse;
    for (uint32 i = 0; i < ParamCount; ++i)
        if (!s.writeInt32u(i) || !s.writeDouble(normalized(normalizedValue(i)))) return kResultFalse;
    const Packed packed = pack(record);
    for (uint32 k = 0; k < resultCount; ++k)
        if (!s.writeInt32u(ResultBase + k) || !s.writeDouble(packed[k])) return kResultFalse;
    return kResultOk;
}
}  // namespace vd
