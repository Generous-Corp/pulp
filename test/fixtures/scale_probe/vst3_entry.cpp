#include "scale_probe.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID ScaleProbeUID(0x50554C50, 0x5363616C, 0x6550726F, 0x00000001);

PULP_VST3_PLUGIN(ScaleProbeUID, "Pulp Scale Probe", Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                 pulp::test_fixtures::create_scale_probe)
