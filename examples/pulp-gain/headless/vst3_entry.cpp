// PulpGainHeadless VST3 entry point

#include "pulp_gain_headless.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID: stable across versions; never change it.
static const Steinberg::FUID PulpGainHeadlessUID(0x50554C50, 0x47414900, 0x00000002, 0x00000001);

PULP_VST3_PLUGIN(PulpGainHeadlessUID, "PulpGainHeadless", Steinberg::Vst::PlugType::kFx, "Pulp",
                 "1.0.0", "https://github.com/Generous-Corp/pulp",
                 pulp::examples::create_pulp_gain_headless)
