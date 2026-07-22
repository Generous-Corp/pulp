// VST3 entry point for the Moonbase Activation example.
#include "moonbase_activation_plugin.hpp"

#include <pulp/format/vst3_entry.hpp>

// Stable unique id — never change once shipped.
static const Steinberg::FUID MoonbaseActivationUID(0x4D4F4F4E, 0x42415345, 0x41435456, 0x00000001);

PULP_VST3_PLUGIN(MoonbaseActivationUID, "Moonbase Activation",
                 Steinberg::Vst::PlugType::kFx, "GenerousCorp", "1.0.0",
                 "https://github.com/Generous-Corp/pulp",
                 moonbase_pulp::create_moonbase_activation_plugin)
