// VST3 processor and controller of VoiceDynamics. Structure after the other plug-ins of this repository (Glaze).
#include "analyzer.h"
#include "automation.h"
#include "chain.h"
#include "editor.h"
#include "learner.h"
#include "loudness.h"
#include "meter.h"
#include "state.h"
#include "version.h"
#include "wavio.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <xmmintrin.h>
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
// New identifiers issued for VoiceDynamics on 2026-10-22. Never reuse elsewhere.
const FUID processorId(0xD8FD0041, 0xF8DA4404, 0xB9B0FA12, 0xC46BB1B5);
const FUID controllerId(0x9FC05A42, 0x3BDE43C4, 0x99D49A3F, 0xE81CECA8);

constexpr uint32 LearnFifoFrames = 1u << 21;

void copyText(const char16_t* text, String128 out) {
    int i = 0;
    for (; text[i] && i < 127; ++i) out[i] = text[i];
    out[i] = 0;
}

// Maps a normalized value with the frozen vd::plain/norm curves; Japanese texts.
class VdParameter final : public Parameter {
public:
    explicit VdParameter(uint32 id)
        : Parameter(vd::specs[id].name, id, vd::specs[id].unit, vd::norm(id, vd::specs[id].def), vd::specs[id].steps,
                    (vd::specs[id].automatable ? ParameterInfo::kCanAutomate : 0) | (vd::specs[id].scale == vd::Scale::List ? ParameterInfo::kIsList : 0) |
                        (id == vd::Bypass ? ParameterInfo::kIsBypass : 0)) {}
    ParamValue toPlain(ParamValue v) const SMTG_OVERRIDE { return vd::plain(getInfo().id, v); }
    ParamValue toNormalized(ParamValue p) const SMTG_OVERRIDE { return vd::norm(getInfo().id, p); }
    void toString(ParamValue v, String128 out) const SMTG_OVERRIDE {
        const uint32 id = getInfo().id;
        const auto& spec = vd::specs[id];
        const double p = vd::plain(id, v);
        if (spec.scale == vd::Scale::Toggle) {
            copyText(p >= .5 ? u"オン" : u"オフ", out);
            return;
        }
        if (spec.scale == vd::Scale::List) {
            copyText(vd::monoNames[std::clamp(int(p), 0, 1)], out);
            return;
        }
        if (id == vd::GateRange && p <= vd::GateRangeMute + .05) {
            copyText(u"ミュート", out);
            return;
        }
        if (id == vd::GateHpf && p <= vd::GateHpfOff + .05) {
            copyText(u"オフ", out);
            return;
        }
        char text[64];
        const bool fine = (id == vd::GateAttack || id == vd::CompAttack) && p < 10;
        const bool whole = id == vd::GateHold || id == vd::GateRelease || id == vd::CompRelease || id == vd::LimiterRelease || id == vd::GateHpf || id == vd::CompAttack;
        std::snprintf(text, sizeof(text), fine ? "%.1f" : whole ? "%.0f" : "%.1f", p);
        int i = 0;
        for (; text[i] && i < 127; ++i) out[i] = char16(text[i]);
        out[i] = 0;
    }
    bool fromString(const TChar* text, ParamValue& v) const SMTG_OVERRIDE {
        if (!text) return false;
        const uint32 id = getInfo().id;
        const auto& spec = vd::specs[id];
        const std::u16string_view view(reinterpret_cast<const char16_t*>(text));
        if (spec.scale == vd::Scale::Toggle) {
            if (view == u"オン" || view == u"on" || view == u"1") return v = 1, true;
            if (view == u"オフ" || view == u"off" || view == u"0") return v = 0, true;
            return false;
        }
        if (spec.scale == vd::Scale::List) {
            for (int i = 0; i <= spec.steps; ++i)
                if (view == vd::monoNames[i]) return v = vd::norm(id, i), true;
        }
        if (id == vd::GateRange && view == u"ミュート") return v = 0, true;
        if (id == vd::GateHpf && view == u"オフ") return v = 0, true;
        char narrow[64]{};
        for (int i = 0; i < 63 && text[i]; ++i) narrow[i] = text[i] < 128 ? char(text[i]) : '?';
        char* end = nullptr;
        const double p = std::strtod(narrow, &end);
        if (end == narrow || !std::isfinite(p) || p < spec.lo || p > spec.hi) return false;
        if (spec.scale == vd::Scale::List && p != std::floor(p)) return false;
        v = vd::norm(id, p);
        return true;
    }
};

// Flush denormals to zero while the processor runs, and restore the host's mode afterwards.
struct DenormalGuard {
    unsigned int saved = _mm_getcsr();
    DenormalGuard() noexcept { _mm_setcsr(saved | 0x8040); }
    ~DenormalGuard() { _mm_setcsr(saved); }
};

std::u16string widen(const std::string& utf8) {
    std::u16string out;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = (unsigned char)utf8[i];
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) cp = c & 7, extra = 3;
        else if (c >= 0xE0) cp = c & 15, extra = 2;
        else if (c >= 0xC0) cp = c & 31, extra = 1;
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) cp = (cp << 6) | ((unsigned char)utf8[i] & 63);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(char16_t(0xD800 + (cp >> 10)));
            out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(char16_t(cp));
        }
    }
    return out;
}

class Processor final : public AudioEffect {
    vd::Chain chain_;
    vd::Learner learner_;
    vd::LoudnessMeter outMeter_;
    vd::TruePeakDetector outTp_;
    double outTpMax_ = 0;  // linear, since the last reset
    std::array<std::atomic<double>, vd::ParamCount> saved_{};
    vd::Values current_{};  // audio thread's copy of saved_
    vd::Settings settings_;
    uint64_t preHash_ = 0;
    std::atomic<double> autoGainDb_{0};
    double appliedAutoGain_ = 0;
    std::atomic<int> channels_{0}, latency_{0};
    std::atomic<bool> resetMeter_{false};
    std::mutex recordMutex_;  // never taken by the audio thread
    vd::ResultRecord record_;
    std::u16string note_;
    uint64_t retargetHash_ = 0;  // UI thread: the stale settings waiting to be solved again
    std::chrono::steady_clock::time_point retargetSince_{};
    vd::Mailbox meter_;
    std::vector<float> preBuf_;
    bool ready_ = false, offline_ = false, forceApply_ = true;
    int meterSamples_ = 0;
    uint64 generation_ = 0;

    int mainChannels() { return SpeakerArr::getChannelCount(getAudioOutput(0)->getArrangement()); }
    void store(uint32 id, double v) noexcept { saved_[id].store(vd::normalized(v), std::memory_order_relaxed); }
    vd::Values savedValues() const noexcept {
        vd::Values v{};
        for (uint32 i = 0; i < vd::ParamCount; ++i) v[i] = saved_[i].load(std::memory_order_relaxed);
        return v;
    }
    vd::Settings savedSettings() const noexcept { return vd::settingsFrom(savedValues()); }
    // Audio thread: take over what the controller or a state load changed.
    void sync() noexcept {
        bool dirty = forceApply_;
        for (uint32 i = 0; i < vd::ParamCount; ++i) {
            const double v = saved_[i].load(std::memory_order_relaxed);
            if (v != current_[i]) {
                current_[i] = v;
                dirty = true;
            }
        }
        const double gain = autoGainDb_.load(std::memory_order_relaxed);
        if (gain != appliedAutoGain_) {
            appliedAutoGain_ = gain;
            dirty = true;
        }
        if (dirty) apply();
        forceApply_ = false;
    }
    void apply() noexcept {
        settings_ = vd::settingsFrom(current_);
        preHash_ = vd::preStageHash(settings_, chain_.channels());
        chain_.set(settings_, appliedAutoGain_);
        learner_.setAutoStop(settings_.learnAutoStop);
    }
    bool prepare() {
        offline_ = processSetup.processMode == kOffline;
        const int ch = mainChannels();
        if (ch < 1 || ch > 2) return false;
        chain_.prepare(processSetup.sampleRate, ch);
        outMeter_.prepare(processSetup.sampleRate, ch, false);
        outTp_.prepare(processSetup.sampleRate, ch);
        outTpMax_ = 0;
        preBuf_.assign(size_t(std::max<int32>(processSetup.maxSamplesPerBlock, 512)) * size_t(ch), 0.f);
        channels_.store(ch);
        latency_.store(chain_.latency());
        current_ = savedValues();
        appliedAutoGain_ = autoGainDb_.load();
        apply();
        chain_.reset();
        forceApply_ = false;
        ready_ = true;
        meterSamples_ = 0;
        publishMeter();
        return true;
    }
    void applyRecord(const vd::ResultRecord& r) {
        {
            std::lock_guard<std::mutex> lock(recordMutex_);
            record_ = r;
            note_.clear();
        }
        autoGainDb_.store(r.valid ? r.gainDb : 0.0);
    }
    // Worker thread of the learner: the session ended in Done or Failed.
    void learnEnded() {
        vd::Learner::Result r;
        if (!learner_.result(r)) return;
        if (r.analysis.ok) {
            applyRecord(vd::recordFrom(r.analysis, vd::SourceLearn, r.settingsHash, r.channels, r.fs, r.flags));
        } else {
            std::lock_guard<std::mutex> lock(recordMutex_);
            note_ = widen(r.analysis.error);
        }
    }
    void startLearning() {
        learner_.setCompletion([this] { learnEnded(); });
        {
            std::lock_guard<std::mutex> lock(recordMutex_);
            note_.clear();
        }
        learner_.arm([this] { return savedSettings(); }, LearnFifoFrames);
    }

public:
    Processor() {
        setControllerClass(controllerId);
        const auto v = vd::defaults();
        for (uint32 i = 0; i < vd::ParamCount; ++i) store(i, v[i]);
        current_ = v;
        vd::Chain scratch;  // the latency before the host has set anything up
        scratch.prepare(48000, 2);
        latency_.store(scratch.latency());
    }
    ~Processor() override { learner_.release(); }
    static FUnknown* create(void*) { return static_cast<IAudioProcessor*>(new Processor); }
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const auto r = AudioEffect::initialize(context);
        if (r != kResultOk) return r;
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 ni, SpeakerArrangement* out, int32 no) override {
        if (!in || !out || ni != 1 || no != 1 || in[0] != out[0] || (in[0] != SpeakerArr::kStereo && in[0] != SpeakerArr::kMono)) return kResultFalse;
        getAudioInput(0)->setArrangement(in[0]);
        getAudioOutput(0)->setArrangement(out[0]);
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 || size == kSample64 ? kResultTrue : kResultFalse; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& s) override {
        if (s.maxSamplesPerBlock < 1 || canProcessSampleSize(s.symbolicSampleSize) != kResultTrue || !std::isfinite(s.sampleRate) || s.sampleRate < 22050 ||
            s.sampleRate > 384000)
            return kResultFalse;
        const auto r = AudioEffect::setupProcessing(s);
        if (r != kResultOk) return r;
        return prepare() ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API setActive(TBool on) override {
        if (on && !prepare()) return kResultFalse;
        if (!on) {
            // Free the learning memory unless a session is running (the host may toggle activity around an export).
            const auto s = learner_.state();
            if (s != vd::Learner::Armed && s != vd::Learner::Capturing && s != vd::Learner::Computing) learner_.release();
        }
        return AudioEffect::setActive(on);
    }
    tresult PLUGIN_API setProcessing(TBool on) override {
        if (on) {
            sync();
            chain_.reset();
            meterSamples_ = 0;
        } else if (learner_.state() == vd::Learner::Capturing && savedSettings().learnAutoStop) {
            learner_.finish();  // playback stopped: end the learning (the option 停止で学習を終える)
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getLatencySamples() override { return uint32(latency_.load()); }
    uint32 PLUGIN_API getTailSamples() override { return uint32(latency_.load()); }

    void fillLearn(vd::MeterFrame& f, std::u16string& note) {
        const auto st = learner_.status();
        f[vd::MLearnState] = double(st.state);
        f[vd::MLearnSeconds] = st.capturedSeconds;
        f[vd::MLearnProgress] = std::clamp(st.progress, 0., 1.);
        f[vd::MLearnStart] = st.startSeconds;
        f[vd::MLearnFlags] = double(st.flags);
        f[vd::MLearnDropped] = double(st.droppedFrames);
        vd::ResultRecord r;
        {
            std::lock_guard<std::mutex> lock(recordMutex_);
            r = record_;
            note = note_;
        }
        f[vd::MRecValid] = r.valid;
        if (r.valid) {
            f[vd::MRecSource] = r.source;
            f[vd::MRecGain] = r.gainDb;
            f[vd::MRecPre] = r.preLufs;
            f[vd::MRecPred] = r.predictedLufs;
            f[vd::MRecTp] = r.truePeakDb;
            f[vd::MRecMaxGr] = r.maxGrDb;
            f[vd::MRecDuration] = r.durationSec;
            f[vd::MRecWarnings] = r.warnings;
            f[vd::MRecFlags] = r.learnFlags;
            f[vd::MRecUnreached] = r.unreached;
            const int ch = channels_.load() ? channels_.load() : r.channels;
            const auto settings = savedSettings();
            const uint64_t hash = vd::analysisHash(settings, ch);
            f[vd::MRecStale] = hash != r.hash;
            // A new target or limiter setting: solve again over the kept recording once the value has rested for a moment.
            const auto now = std::chrono::steady_clock::now();
            if (hash == r.hash || r.source != vd::SourceLearn) retargetHash_ = 0;
            else if (hash != retargetHash_) {
                retargetHash_ = hash;
                retargetSince_ = now;
            } else if (now - retargetSince_ >= std::chrono::milliseconds(400) && learner_.retarget(vd::preStageHash(settings, ch))) {
                f[vd::MLearnState] = double(vd::Learner::Computing);
                f[vd::MLearnProgress] = 0;
            }
        }
    }
    tresult PLUGIN_API notify(IMessage* message) override {
        if (!message || !message->getMessageID()) return kInvalidArgument;
        const char* id = message->getMessageID();
        if (!std::strcmp(id, vd::CommandMessage)) {
            int64 command = 0;
            if (!message->getAttributes() || message->getAttributes()->getInt("command", command) != kResultOk) return kInvalidArgument;
            switch (command) {
            case vd::CmdLearnStart: startLearning(); return kResultOk;
            case vd::CmdLearnFinish: learner_.finish(); return kResultOk;
            case vd::CmdLearnCancel: learner_.cancel(); return kResultOk;
            case vd::CmdClearResult:
                learner_.cancel();
                applyRecord(vd::ResultRecord{});
                return kResultOk;
            case vd::CmdResetMeter: resetMeter_.store(true); return kResultOk;
            default: return kInvalidArgument;
            }
        }
        if (!std::strcmp(id, vd::AnalysisMessage)) {
            const void* data = nullptr;
            uint32 size = 0;
            vd::Packed packed{};
            vd::ResultRecord r;
            if (!message->getAttributes() || message->getAttributes()->getBinary("record", data, size) != kResultOk || !data || size != sizeof(packed)) return kInvalidArgument;
            std::memcpy(packed.data(), data, size);
            if (!vd::unpack(packed, r) || !r.valid || r.source != vd::SourceFile) return kInvalidArgument;
            if (learner_.state() == vd::Learner::Failed || learner_.state() == vd::Learner::Done) learner_.cancel();  // the new result replaces the old session's message
            applyRecord(r);
            return kResultOk;
        }
        if (std::strcmp(id, vd::MeterRequest)) return AudioEffect::notify(message);
        vd::MeterFrame f{};
        if (!meter_.read(f)) return kResultFalse;
        std::u16string note;
        fillLearn(f, note);
        if (auto reply = owned(allocateMessage())) {
            reply->setMessageID(vd::MeterReply);
            reply->getAttributes()->setBinary("frame", f.data(), uint32(sizeof(f)));
            reply->getAttributes()->setString("note", reinterpret_cast<const TChar*>(note.c_str()));
            return sendMessage(reply);
        }
        return kResultFalse;
    }
    void publishMeter() noexcept {
        vd::MeterFrame f{};
        const bool dual = settings_.dualMono && chain_.channels() == 1;
        auto lufs = [&](double v) { return v <= vd::NoLevel + 1 ? vd::NoLevel : v + (dual ? 3.0103 : 0.0); };
        f[vd::MGeneration] = double(++generation_);
        f[vd::MRate] = chain_.sampleRate();
        f[vd::MChannels] = chain_.channels();
        f[vd::MLatency] = chain_.latency();
        f[vd::MBypass] = settings_.bypass;
        f[vd::MOffline] = offline_;
        auto& st = chain_.stats();
        f[vd::MInputPeak] = st.inputPeak;
        f[vd::MOutputPeak] = st.outputPeak;
        f[vd::MMomentary] = lufs(outMeter_.momentary());
        f[vd::MShortTerm] = lufs(outMeter_.shortTerm());
        f[vd::MIntegrated] = outMeter_.hasIntegrated() ? lufs(outMeter_.integrated()) : vd::NoLevel;
        f[vd::MOutTruePeak] = outTpMax_ > 1e-9 ? vd::linToDb(outTpMax_) : vd::NoLevel;
        f[vd::MGateDb] = vd::linToDb(st.gateGainMin);
        f[vd::MLevelerDb] = std::abs(st.levelerDbMin) > std::abs(st.levelerDbMax) ? st.levelerDbMin : st.levelerDbMax;
        f[vd::MCompDb] = st.compDbMin;
        f[vd::MLimiterDb] = st.limiterDbMin;
        f[vd::MGainDb] = chain_.gainDb();
        st.clear();
        meter_.publish(f);
    }
    template <class T>
    tresult render(ProcessData& d) {
        if (d.numOutputs < 1 || !d.outputs) return kResultOk;
        const int ch = mainChannels();
        if (d.outputs[0].numChannels != ch || ch != chain_.channels()) return kResultFalse;
        auto output = [&](int c) -> T* {
            if constexpr (sizeof(T) == 4) return d.outputs[0].channelBuffers32 ? d.outputs[0].channelBuffers32[c] : nullptr;
            else return d.outputs[0].channelBuffers64 ? d.outputs[0].channelBuffers64[c] : nullptr;
        };
        for (int c = 0; c < ch; ++c)
            if (!output(c)) return kResultFalse;
        const bool haveInput = d.inputs && d.numInputs > 0 && d.inputs[0].numChannels >= 1 && d.inputs[0].numChannels <= 2;
        auto sample = [&](int c, int n) -> double {
            if (!haveInput) return 0;
            auto& b = d.inputs[0];
            c = std::min(c, b.numChannels - 1);
            if (b.silenceFlags & (uint64(1) << c)) return 0;
            if constexpr (sizeof(T) == 4) return b.channelBuffers32 && b.channelBuffers32[c] ? b.channelBuffers32[c][n] : 0;
            else return b.channelBuffers64 && b.channelBuffers64[c] ? b.channelBuffers64[c][n] : 0;
        };
        std::array<vd::AutomationLane, vd::ParamCount> lanes{};
        std::array<uint32, vd::ParamCount> active{};
        int activeCount = 0;
        if (d.inputParameterChanges)
            for (int q = 0; q < d.inputParameterChanges->getParameterCount(); ++q) {
                auto* queue = d.inputParameterChanges->getParameterData(q);
                if (!queue) continue;
                const auto id = queue->getParameterId();
                if (id >= vd::ParamCount || lanes[id].queue) continue;
                lanes[id].begin(queue, current_[id], vd::specs[id].steps > 0, d.numSamples);
                active[activeCount++] = id;
            }
        // Host transport: capture only while playing (always in an export), with the project position for jump detection.
        const bool exporting = d.processMode == kOffline;
        const bool haveContext = d.processContext != nullptr;
        const bool playing = exporting || !haveContext || (d.processContext->state & ProcessContext::kPlaying) != 0;
        const bool haveTime = haveContext;
        const int64 startPosition = haveContext ? d.processContext->projectTimeSamples : 0;
        const bool capture = learner_.capturing();
        const size_t cap = preBuf_.size() / size_t(ch);
        size_t filled = 0;
        int64 chunkStart = startPosition;
        const uint64 hash = preHash_;
        uint64 silence = (uint64(1) << ch) - 1;
        const double fs = chain_.sampleRate();
        auto flush = [&] {
            learner_.block(preBuf_.data(), uint32(filled), ch, fs, playing, haveTime, chunkStart, hash, exporting);
            chunkStart += int64(filled);
            filled = 0;
        };
        // Reading all inputs before writing any output keeps in-place buffers safe.
        for (int n = 0; n < d.numSamples; ++n) {
            bool changed = false;
            for (int i = 0; i < activeCount; ++i) {
                const auto id = active[i];
                const double v = lanes[id].at(n);
                if (v != current_[id]) {
                    current_[id] = v;
                    store(id, v);
                    changed = true;
                }
            }
            if (changed) apply();
            const double x[2]{sample(0, n), sample(1, n)};
            double y[2]{}, p[2]{};
            chain_.tick(x, y, capture ? p : nullptr);
            for (int c = 0; c < ch; ++c) {
                output(c)[n] = T(y[c]);
                if (y[c] != 0) silence &= ~(uint64(1) << c);
            }
            outMeter_.push(y);
            outTpMax_ = std::max(outTpMax_, outTp_.push(y, outTpMax_));
            if (capture) {
                for (int c = 0; c < ch; ++c) preBuf_[filled * size_t(ch) + size_t(c)] = float(p[c]);
                if (++filled == cap) flush();
            }
        }
        if (capture && (filled > 0 || d.numSamples == 0)) flush();
        meterSamples_ += d.numSamples;
        if (meterSamples_ >= processSetup.sampleRate / 30) {
            if (resetMeter_.exchange(false)) {
                outMeter_.reset();
                outTpMax_ = 0;
            }
            publishMeter();
            meterSamples_ = 0;
        }
        d.outputs[0].silenceFlags = silence;
        return kResultOk;
    }
    tresult PLUGIN_API process(ProcessData& d) override {
        if (d.numSamples < 0) return kInvalidArgument;
        if (d.numSamples == 0) {
            // Parameter flush: keep the last value of each queue.
            if (d.inputParameterChanges)
                for (int i = 0; i < d.inputParameterChanges->getParameterCount(); ++i) {
                    auto* q = d.inputParameterChanges->getParameterData(i);
                    if (!q || q->getParameterId() >= vd::ParamCount || q->getPointCount() == 0) continue;
                    int32 offset = 0;
                    double v = 0;
                    if (q->getPoint(q->getPointCount() - 1, offset, v) == kResultTrue && std::isfinite(v)) store(q->getParameterId(), v);
                }
            return kResultOk;
        }
        if (!ready_) return kResultFalse;
        DenormalGuard guard;
        sync();
        if (d.symbolicSampleSize == kSample32) return render<float>(d);
        if (d.symbolicSampleSize == kSample64) return render<double>(d);
        return kResultFalse;
    }
    tresult PLUGIN_API getState(IBStream* stream) override {
        vd::ResultRecord r;
        {
            std::lock_guard<std::mutex> lock(recordMutex_);
            r = record_;
        }
        return vd::writeState(stream, [&](uint32 i) { return saved_[i].load(std::memory_order_relaxed); }, r);
    }
    tresult PLUGIN_API setState(IBStream* stream) override {
        vd::State s;
        if (!vd::readState(stream, s)) return kResultFalse;
        for (uint32 i = 0; i < vd::ParamCount; ++i) store(i, s.values[i]);
        applyRecord(s.record);
        return kResultOk;
    }
};

class Controller final : public EditController, public vd::EditorLink {
    vd::Mailbox meter_;
    vd::EditorPreferences editorPreferences_;
    std::u16string note_;
    double rate_ = 0;
    int channels_ = 0;
    // File analysis (auxiliary): a thread reads and analyses; the UI thread sends the result.
    std::mutex fileMutex_;
    std::thread fileThread_;
    std::atomic<bool> cancelFile_{false};
    vd::FileStatus file_;
    vd::ResultRecord filePending_;
    bool havePending_ = false;

    void send(vd::Command c) {
        if (auto message = owned(allocateMessage())) {
            message->setMessageID(vd::CommandMessage);
            message->getAttributes()->setInt("command", c);
            sendMessage(message);
        }
    }
    // UI thread: forward a finished file analysis to the processor and reap the thread.
    void pump() {
        vd::ResultRecord pending;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(fileMutex_);
            have = havePending_;
            pending = filePending_;
            havePending_ = false;
        }
        if (have)
            if (auto message = owned(allocateMessage())) {
                const vd::Packed packed = vd::pack(pending);
                message->setMessageID(vd::AnalysisMessage);
                message->getAttributes()->setBinary("record", packed.data(), uint32(sizeof(packed)));
                sendMessage(message);
            }
        bool reap = false;
        {
            std::lock_guard<std::mutex> lock(fileMutex_);
            reap = !file_.running && fileThread_.joinable();
        }
        if (reap) fileThread_.join();
    }
    // The message of an earlier file analysis is not shown again (a running analysis is kept).
    void forgetFile() {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (!file_.running) file_ = vd::FileStatus{};
    }
    void stopFile() {
        cancelFile_.store(true);
        if (fileThread_.joinable()) fileThread_.join();
    }

public:
    ~Controller() override { stopFile(); }
    static FUnknown* create(void*) { return static_cast<IEditController*>(new Controller); }
    bool pollMeter(vd::MeterFrame& f) override {
        pump();
        sendMessageID(vd::MeterRequest);
        return meter_.read(f);
    }
    std::u16string note() override { return note_; }
    void command(vd::Command c) override {
        if (c == vd::CmdLearnStart || c == vd::CmdClearResult) forgetFile();
        send(c);
    }
    vd::FileStatus fileStatus() override {
        pump();
        std::lock_guard<std::mutex> lock(fileMutex_);
        return file_;
    }
    void cancelFile() override { cancelFile_.store(true); }
    bool analyzeFile(const std::wstring& path) override {
        pump();
        {
            std::lock_guard<std::mutex> lock(fileMutex_);
            if (file_.running) return false;
        }
        if (fileThread_.joinable()) fileThread_.join();
        vd::Values values{};
        for (uint32 i = 0; i < vd::ParamCount; ++i) values[i] = getParamNormalized(i);
        const vd::Settings settings = vd::settingsFrom(values);
        const int channels = channels_ ? channels_ : 2;
        const double pluginRate = rate_;
        const std::filesystem::path p(path);
        {
            std::lock_guard<std::mutex> lock(fileMutex_);
            file_ = vd::FileStatus{};
            file_.running = true;
            const auto name = p.filename().u16string();
            file_.name = name;
        }
        cancelFile_.store(false);
        fileThread_ = std::thread([this, settings, channels, pluginRate, p] {
            vd::FileStatus result;
            result.name = p.filename().u16string();
            vd::ResultRecord record;
            auto fail = [&](const std::string& m) {
                result.ok = false;
                result.message = widen(m);
            };
            try {
                vd::WavInfo info;
                vd::ChunkedPcm pcm;
                std::string error;
                auto progress = [this](double fraction) {
                    std::lock_guard<std::mutex> lock(fileMutex_);
                    file_.progress = fraction * 0.3;
                    return !cancelFile_.load();
                };
                if (!vd::wavRead(p, channels, info, pcm, error, progress)) {
                    fail(error);
                } else if (pluginRate > 0 && std::abs(info.rate - pluginRate) > 0.5) {
                    char text[200];
                    std::snprintf(text, sizeof(text), "WAVのサンプルレート（%.0f Hz）がプロジェクト（%.0f Hz）と違います。同じレートで書き出したWAVを使ってください。", info.rate, pluginRate);
                    fail(text);
                } else {
                    auto analysisProgress = [this](double fraction) {
                        std::lock_guard<std::mutex> lock(fileMutex_);
                        file_.progress = 0.3 + fraction * 0.7;
                        return !cancelFile_.load();
                    };
                    const auto r = vd::analyzeFile(settings, channels, info.rate, pcm, analysisProgress);
                    if (r.cancelled) {
                        fail("取り消しました。");
                    } else if (!r.ok) {
                        fail(r.error);
                    } else {
                        record = vd::recordFrom(r, vd::SourceFile, vd::analysisHash(settings, channels), channels, info.rate, 0);
                        result.ok = true;
                        char text[160];
                        std::snprintf(text, sizeof(text), "補正ゲイン %+.2f dB（予測 %.2f LUFS）", r.gainDb, r.predictedLufs);
                        result.message = widen(text);
                    }
                }
            } catch (const std::exception&) {
                fail("WAVの解析中にメモリ不足などの問題が起きました。");
            } catch (...) {
                fail("WAVの解析中に問題が起きました。");
            }
            std::lock_guard<std::mutex> lock(fileMutex_);
            result.progress = 1;
            result.done = true;
            file_ = result;
            if (result.ok) {
                filePending_ = record;
                havePending_ = true;
            }
            file_.running = false;
        });
        return true;
    }
    tresult PLUGIN_API notify(IMessage* message) override {
        if (!message || !message->getMessageID()) return kInvalidArgument;
        if (std::strcmp(message->getMessageID(), vd::MeterReply)) return EditController::notify(message);
        const void* data = nullptr;
        uint32 size = 0;
        if (!message->getAttributes() || message->getAttributes()->getBinary("frame", data, size) != kResultOk || !data || size != sizeof(vd::MeterFrame)) return kResultFalse;
        vd::MeterFrame f{};
        std::memcpy(f.data(), data, size);
        if (!vd::validMeter(f)) return kResultFalse;
        TChar text[256]{};
        if (message->getAttributes()->getString("note", text, sizeof(text)) == kResultOk) {
            std::u16string s;
            for (int i = 0; i < 255 && text[i]; ++i) s.push_back(char16_t(text[i]));
            note_ = s;
        }
        rate_ = f[vd::MRate];
        channels_ = int(f[vd::MChannels]);
        meter_.publish(f);
        return kResultOk;
    }
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const auto r = EditController::initialize(context);
        if (r != kResultOk) return r;
        for (uint32 i = 0; i < vd::ParamCount; ++i) parameters.addParameter(new VdParameter(i));
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override {
        forgetFile();
        return name && !std::strcmp(name, ViewType::kEditor) ? createVoiceDynamicsEditor(this, this, &editorPreferences_) : nullptr;
    }
    tresult PLUGIN_API setComponentState(IBStream* stream) override {
        vd::State s;
        if (!vd::readState(stream, s)) return kResultFalse;
        for (uint32 i = 0; i < vd::ParamCount; ++i) setParamNormalized(i, s.values[i]);
        return kResultOk;
    }
    // All sound settings live in the component state; the controller has none.
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
};
}  // namespace

bool InitModule() { return true; }
bool DeinitModule() {
    unregisterVoiceDynamicsEditor();
    return true;
}
BEGIN_FACTORY_DEF(VD_VENDOR, VD_URL, "")
DEF_CLASS2(INLINE_UID_FROM_FUID(processorId), PClassInfo::kManyInstances, kVstAudioEffectClass, VD_PRODUCT, Vst::kDistributable, "Fx|Dynamics", VD_VERSION,
           kVstVersionString, Processor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(controllerId), PClassInfo::kManyInstances, kVstComponentControllerClass, VD_PRODUCT " Controller", 0, "", VD_VERSION, kVstVersionString,
           Controller::create)
END_FACTORY
