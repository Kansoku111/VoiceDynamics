#pragma once
// Minimal VST3 host pieces shared by the independent host and editor tests
// (pattern shared with the other plug-ins in this repository).
#include "meter.h"
#include "parameters.h"
#include "result.h"
#include "rt_guard.h"
#include "state.h"
#include "base/source/fobject.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"
#include <windows.h>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr double Tau = 6.283185307179586;
inline void check(bool b, const std::string& label) {
    if (!b) throw std::runtime_error(label);
}

// Records edit gestures; optionally forwards performed edits to the processor like a host.
class Handler : public FObject, public IComponentHandler {
    std::array<bool, vd::ParamCount> open_{};

public:
    int restarts = 0, began = 0, ended = 0, performed = 0;
    bool valid = true;
    std::function<void(ParamID, ParamValue)> forward;
    tresult PLUGIN_API beginEdit(ParamID id) override {
        valid &= id < vd::ParamCount && !open_[id];
        if (id < vd::ParamCount) open_[id] = true;
        ++began;
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override {
        valid &= id < vd::ParamCount && open_[id] && std::isfinite(v) && v >= 0 && v <= 1;
        ++performed;
        if (forward) forward(id, v);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID id) override {
        valid &= id < vd::ParamCount && open_[id];
        if (id < vd::ParamCount) open_[id] = false;
        ++ended;
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32) override {
        ++restarts;
        return kResultOk;
    }
    OBJ_METHODS(Handler, FObject)
    DEFINE_INTERFACES
    DEF_INTERFACE(IComponentHandler)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};

// Sits between the processor and the controller: records the processor's replies and forwards them.
class Tee : public FObject, public IConnectionPoint {
public:
    IConnectionPoint* forward = nullptr;
    vd::MeterFrame frame{};
    std::u16string note;
    bool haveFrame = false;
    int replies = 0;
    tresult PLUGIN_API connect(IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API disconnect(IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API notify(IMessage* message) override {
        if (message && message->getMessageID() && !std::strcmp(message->getMessageID(), vd::MeterReply)) {
            const void* data = nullptr;
            uint32 size = 0;
            if (message->getAttributes()->getBinary("frame", data, size) == kResultOk && size == sizeof(frame)) {
                std::memcpy(frame.data(), data, size);
                haveFrame = true;
                ++replies;
            }
            TChar text[256]{};
            note.clear();
            if (message->getAttributes()->getString("note", text, sizeof(text)) == kResultOk)
                for (int i = 0; i < 255 && text[i]; ++i) note.push_back(char16_t(text[i]));
        }
        return forward ? forward->notify(message) : kResultOk;
    }
    OBJ_METHODS(Tee, FObject)
    DEFINE_INTERFACES
    DEF_INTERFACE(IConnectionPoint)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};

struct Instance {
    HostApplication app;
    IComponent* comp = nullptr;
    IAudioProcessor* proc = nullptr;
    IEditController* ctrl = nullptr;
    IConnectionPoint *pc = nullptr, *cc = nullptr;
    IPtr<Tee> tee;
    int channels = 2, format = kSample64;
    double rate = 48000;
    bool active = false;
    explicit Instance(IPluginFactory* factory) {
        FUID id(0xD8FD0041, 0xF8DA4404, 0xB9B0FA12, 0xC46BB1B5);
        check(factory->createInstance(id, IComponent::iid, reinterpret_cast<void**>(&comp)) == kResultOk, "create component");
        check(comp->initialize(&app) == kResultOk, "initialize");
        check(comp->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&proc)) == kResultOk, "processor interface");
        TUID cid;
        comp->getControllerClassId(cid);
        check(FUID::fromTUID(cid) == FUID(0x9FC05A42, 0x3BDE43C4, 0x99D49A3F, 0xE81CECA8), "controller class id");
        check(factory->createInstance(cid, IEditController::iid, reinterpret_cast<void**>(&ctrl)) == kResultOk, "create controller");
        check(ctrl->initialize(&app) == kResultOk, "initialize controller");
        check(comp->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&pc)) == kResultOk, "processor point");
        check(ctrl->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&cc)) == kResultOk, "controller point");
        tee = owned(new Tee);
        tee->forward = cc;
        pc->connect(tee);
        cc->connect(pc);
    }
    // Host-style parameter change: sent to the processor in a zero-length flush and to the controller.
    void parameter(uint32 id, double v) {
        ParameterChanges changes(1);
        int32 index = 0;
        changes.addParameterData(id, index)->addPoint(0, v, index);
        ProcessData d{};
        d.symbolicSampleSize = format;
        d.inputParameterChanges = &changes;
        check(proc->process(d) == kResultOk, "flush parameter");
        ctrl->setParamNormalized(id, v);
    }
    void plain(uint32 id, double p) { parameter(id, vd::norm(id, p)); }
    // A message from the controller side to the processor (as the editor sends them).
    void command(vd::Command c) {
        IPtr<HostMessage> msg = owned(new HostMessage);
        msg->setMessageID(vd::CommandMessage);
        msg->getAttributes()->setInt("command", c);
        check(pc->notify(msg) == kResultOk, "processor accepts the command");
    }
    // Asks the processor for a meter frame the way the controller does; the reply arrives through the tee.
    bool meter(vd::MeterFrame& f, std::u16string* note = nullptr) {
        IPtr<HostMessage> msg = owned(new HostMessage);
        msg->setMessageID(vd::MeterRequest);
        if (pc->notify(msg) != kResultOk || !tee->haveFrame) return false;
        f = tee->frame;
        if (note) *note = tee->note;
        return true;
    }
    // Waits until the learner reaches one of two states (Done/Failed by default).
    vd::MeterFrame waitLearn(double timeoutSeconds = 60, int a = vd::Learner::Done, int b = vd::Learner::Failed) {
        const auto start = std::chrono::steady_clock::now();
        vd::MeterFrame f{};
        for (;;) {
            check(meter(f), "meter frame");
            const int s = int(f[vd::MLearnState]);
            if (s == a || s == b) return f;
            check(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < timeoutSeconds, "timed out waiting for the learner, state " + std::to_string(s));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    bool layout(int mainCh) {
        SpeakerArrangement ins[]{mainCh == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo};
        SpeakerArrangement outs[]{ins[0]};
        return proc->setBusArrangements(ins, 1, outs, 1) == kResultOk;
    }
    void start(double sr = 48000, int fmt = kSample64, int mainCh = 2, int maxBlock = 4096, int mode = kRealtime) {
        rate = sr;
        format = fmt;
        channels = mainCh;
        check(layout(mainCh), "layout");
        comp->activateBus(kAudio, kInput, 0, true);
        comp->activateBus(kAudio, kOutput, 0, true);
        ProcessSetup setup{mode, fmt, maxBlock, sr};
        check(proc->setupProcessing(setup) == kResultOk, "setup");
        check(comp->setActive(true) == kResultOk, "activate");
        check(proc->setProcessing(true) == kResultOk, "start processing");
        active = true;
    }
    void stop() {
        if (!active) return;
        proc->setProcessing(false);
        comp->setActive(false);
        active = false;
    }
    ~Instance() {
        stop();
        pc->disconnect(cc);
        cc->disconnect(pc);
        pc->release();
        cc->release();
        ctrl->setComponentHandler(nullptr);
        ctrl->terminate();
        ctrl->release();
        proc->release();
        comp->terminate();
        comp->release();
    }
    void saveState(MemoryStream& s) {
        check(comp->getState(&s) == kResultOk, "getState");
        s.seek(0, IBStream::kIBSeekSet, nullptr);
    }
    void loadState(MemoryStream& s) {
        s.seek(0, IBStream::kIBSeekSet, nullptr);
        check(comp->setState(&s) == kResultOk, "setState");
        s.seek(0, IBStream::kIBSeekSet, nullptr);
        check(ctrl->setComponentState(&s) == kResultOk, "setComponentState");
    }
};

// What the host tells the plug-in about the transport.
struct Transport {
    bool context = true, playing = true, offline = false;
    int64 start = 0;
    std::vector<std::pair<int64, int64>> jumps;  // (frame index in the render, change of the position from then on)
};
struct Play {
    std::vector<int> schedule{512};  // block lengths, used cyclically
    bool inPlace = false, guard = false;
    int tailFrames = 0;  // silent frames appended after the input
    Transport transport;
    uint32 automationId = vd::ParamCount;  // an automated parameter (value from automation())
    std::function<double(int64)> automation;
    double* processSeconds = nullptr;  // accumulates the time spent inside process()
};

// Plays interleaved material (a.channels channels) through the plug-in and returns the interleaved output.
template <class T>
std::vector<double> play(Instance& a, const std::vector<float>& input, const Play& o) {
    const size_t ch = size_t(a.channels), frames = input.size() / ch + size_t(o.tailFrames);
    std::vector<double> result(frames * ch);
    constexpr int Max = 8192;
    std::vector<T> in0(Max), in1(Max), out0(Max), out1(Max);
    T* in[]{in0.data(), in1.data()};
    T* out[]{o.inPlace ? in0.data() : out0.data(), o.inPlace ? in1.data() : out1.data()};
    AudioBusBuffers inBus{}, outBus{};
    inBus.numChannels = outBus.numChannels = a.channels;
    if constexpr (sizeof(T) == 4) {
        inBus.channelBuffers32 = in;
        outBus.channelBuffers32 = out;
    } else {
        inBus.channelBuffers64 = in;
        outBus.channelBuffers64 = out;
    }
    size_t pos = 0;
    int block = 0;
    while (pos < frames) {
        const int n = int(std::min<size_t>(frames - pos, size_t(o.schedule[size_t(block) % o.schedule.size()])));
        for (int i = 0; i < n; ++i)
            for (size_t c = 0; c < ch; ++c) {
                const size_t k = (pos + size_t(i)) * ch + c;
                (c == 0 ? in0 : in1)[size_t(i)] = T(k < input.size() ? input[k] : 0.f);
            }
        ParameterChanges changes(1);
        if (o.automation) {
            int32 index = 0;
            changes.addParameterData(o.automationId, index)->addPoint(n - 1, o.automation(int64(pos) + n - 1), index);
        }
        int64 shift = 0;
        for (const auto& j : o.transport.jumps)
            if (int64(pos) >= j.first) shift += j.second;
        ProcessContext context{};
        context.state = o.transport.playing ? ProcessContext::kPlaying : 0;
        context.sampleRate = a.rate;
        context.projectTimeSamples = o.transport.start + int64(pos) + shift;
        ProcessData d{};
        d.inputParameterChanges = o.automation ? &changes : nullptr;
        d.processMode = o.transport.offline ? kOffline : kRealtime;
        d.symbolicSampleSize = a.format;
        d.numSamples = n;
        d.numInputs = 1;
        d.numOutputs = 1;
        d.inputs = &inBus;
        d.outputs = &outBus;
        d.processContext = o.transport.context ? &context : nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        if (o.guard) {
            rtguard::Armed armed;
            check(a.proc->process(d) == kResultOk, "process");
        } else {
            check(a.proc->process(d) == kResultOk, "process");
        }
        if (o.processSeconds) *o.processSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (int i = 0; i < n; ++i)
            for (size_t c = 0; c < ch; ++c) {
                const double v = double((c == 0 ? out0 : out1)[size_t(i)]);
                if (o.inPlace) {
                    const double w = double((c == 0 ? in0 : in1)[size_t(i)]);
                    result[(pos + size_t(i)) * ch + c] = w;
                    check(std::isfinite(w), "finite output");
                } else {
                    result[(pos + size_t(i)) * ch + c] = v;
                    check(std::isfinite(v), "finite output");
                }
            }
        pos += size_t(n);
        ++block;
    }
    return result;
}

inline double maxDifference(const std::vector<double>& a, const std::vector<double>& b) {
    check(a.size() == b.size(), "length");
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}

// Two bytes of every state value, for building crafted states.
struct StateBuilder {
    MemoryStream s;
    void u32(uint32 v) { s.write(&v, 4, nullptr); }
    void f64(double v) { s.write(&v, 8, nullptr); }
    void header(uint32 magic, uint32 version, uint32 count) {
        u32(magic);
        u32(version);
        u32(count);
    }
    void entry(uint32 id, double v) {
        u32(id);
        f64(v);
    }
    MemoryStream& stream() {
        s.seek(0, IBStream::kIBSeekSet, nullptr);
        return s;
    }
};
