#include "processor.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID kPulpSdkSmokeUid(
    0x12345678, 0x12345679, 0x1234567A, 0x1234567B);

PULP_VST3_PLUGIN(
    kPulpSdkSmokeUid,
    "PulpSDKSmokeProbe",
    Steinberg::Vst::PlugType::kFx,
    "Pulp",
    "0.1.0",
    "",
    pulp::sdk_smoke::create_processor)
