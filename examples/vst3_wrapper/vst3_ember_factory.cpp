#include "vst3_ember_processor.h"

#include "public.sdk/source/main/pluginfactory.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF("Ember", "https://github.com/", "")

DEF_CLASS2(INLINE_UID_FROM_FUID(EmberVst3::kProcessorUID),
           PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           "Ember Gain",
           0, // combined processor/controller; not distributable
           PlugType::kFx,
           "1.0.0",
           kVstVersionString,
           EmberVst3::EmberProcessor::createInstance)

END_FACTORY
