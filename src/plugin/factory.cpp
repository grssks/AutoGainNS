#include "public.sdk/source/main/pluginfactory.h"
#include "plugin/ids.hpp"
#include "plugin/processor.hpp"
#include "plugin/controller.hpp"

#define stringPluginName "AutoGainNS"

BEGIN_FACTORY_DEF("AutoGainNS", "", "")

DEF_CLASS2(
    INLINE_UID_FROM_FUID(autogain::kProcessorUID),
    PClassInfo::kManyInstances,
    kVstAudioEffectClass,
    stringPluginName,
    Vst::kDistributable,
    "Fx|Dynamics|Restoration",
    "1.0.0",
    kVstVersionString,
    autogain::AutoGainNSProcessor::createInstance)

DEF_CLASS2(
    INLINE_UID_FROM_FUID(autogain::kControllerUID),
    PClassInfo::kManyInstances,
    kVstComponentControllerClass,
    stringPluginName "Controller",
    0,
    "",
    "1.0.0",
    kVstVersionString,
    autogain::AutoGainNSController::createInstance)

END_FACTORY
