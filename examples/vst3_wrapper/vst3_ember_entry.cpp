// The SDK's platform main (dllmain.cpp/macmain.cpp/linuxmain.cpp) is added by
// smtg_add_vst3plugin. sdk also supplies InitModule/DeinitModule. This TU is
// intentionally the Ember wrapper's platform hook point and keeps DLL entry
// concerns separate from the factory and processor implementation.
#include "public.sdk/source/main/moduleinit.h"

namespace {
Steinberg::ModuleInitializer emberVst3ModuleInit([] {});
Steinberg::ModuleTerminator emberVst3ModuleExit([] {});
} // namespace
