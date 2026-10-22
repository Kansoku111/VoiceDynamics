#pragma once
// Sample-accurate automation lanes (pattern shared with the other plug-ins in this repository).
#include "parameters.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

namespace vd {
// VST3 queues describe piecewise-linear curves. The implicit prior point is at -1.
// Discrete parameters retain their value until the supplied transition sample.
struct AutomationLane {
    Steinberg::Vst::IParamValueQueue* queue=nullptr;
    int cursor=0, previousOffset=-1, nextOffset=0, samples=0;
    double previousValue=0,nextValue=0;
    bool discrete=false,hasNext=false;
    void advance() noexcept {
        hasNext=false;
        while(queue && cursor<queue->getPointCount()) {
            Steinberg::int32 offset=0; double value=0;
            if(queue->getPoint(cursor++,offset,value)!=Steinberg::kResultOk) continue;
            if(!std::isfinite(value)||offset<0||offset>=samples||offset<previousOffset) continue;
            nextOffset=offset; nextValue=normalized(value); hasNext=true; break;
        }
    }
    void begin(Steinberg::Vst::IParamValueQueue* q,double current,bool stepped,int size) noexcept {
        queue=q; cursor=0; previousOffset=-1; previousValue=current; discrete=stepped; samples=size; advance();
    }
    double at(int sample) noexcept {
        while(hasNext&&nextOffset<=sample) { previousOffset=nextOffset; previousValue=nextValue; advance(); }
        if(!hasNext||discrete) return previousValue;
        const double fraction=double(sample-previousOffset)/(nextOffset-previousOffset);
        return previousValue+(nextValue-previousValue)*fraction;
    }
};
}

