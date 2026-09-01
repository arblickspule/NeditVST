// The VST3 module entry point: class factory with a single combined
// component/controller class. Including pluginfactory.cpp (compiled into
// nedit_vst3_sdk) supplies the per-platform module glue
// (DllMain / bundleEntry / module constructors) automatically.

#include "plugin/NeditProcessor.h"

#include "public.sdk/source/main/pluginfactory.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"

BEGIN_FACTORY_DEF ("arblickspule-collab",                              // vendor
                   "https://github.com/arblickspule/NeditVST",        // fork project
                   "nedit-project@localhost")                         // contact

//------------------------------------------------------------------------
// NeditVST-CollabV2 -- generative sample slicer. Combined component+controller,
// so classFlags is 0 (single-component effects cannot be distributed).
DEF_CLASS2 (INLINE_UID (0x9DCAB3BE, 0x7259C555, 0x17774997, 0xECA6E5F6),
            PClassInfo::kManyInstances,
            kVstAudioEffectClass,
            "NeditVST-CollabV2",
            0,
            "Instrument|Sampler",
            "0.1.0",
            kVstVersionString,
            nedit::plugin::NeditProcessor::createInstance)

END_FACTORY
