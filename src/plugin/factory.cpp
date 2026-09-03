// The VST3 module entry point: class factory with a single combined
// component/controller class. Including pluginfactory.cpp (compiled into
// nedit_vst3_sdk) supplies the per-platform module glue
// (DllMain / bundleEntry / module constructors) automatically.

#include "plugin/NeditProcessor.h"

#include "public.sdk/source/main/pluginfactory.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"

BEGIN_FACTORY_DEF ("Nedit Project",                                   // vendor
                   "https://github.com/nedrush/NeditVST",             // upstream project
                   "nedit-project@localhost")                         // contact

//------------------------------------------------------------------------
// Nedit -- generative sample slicer. Combined component+controller, so
// classFlags is 0 (single-component effects cannot be distributed).
DEF_CLASS2 (INLINE_UID (0x9547773A, 0x0C7D4168, 0x80EB880D, 0xABB4697F),
            PClassInfo::kManyInstances,
             kVstAudioEffectClass,
            "NeditRemix",
            0,
            "Instrument|Sampler",
            "0.1.0",
            kVstVersionString,
            nedit::plugin::NeditProcessor::createInstance)

END_FACTORY
