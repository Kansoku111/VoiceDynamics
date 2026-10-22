#pragma once
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "meter.h"

namespace vd {
struct EditorPreferences {
    float zoom = 1.f;
};
}  // namespace vd
Steinberg::IPlugView* createVoiceDynamicsEditor(Steinberg::Vst::EditController*, vd::EditorLink*, vd::EditorPreferences*);
void unregisterVoiceDynamicsEditor();
