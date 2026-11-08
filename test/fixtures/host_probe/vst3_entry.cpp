#include "host_probe.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID HostProbeUID(0x50554C50, 0x48737450, 0x726F6265, 0x00000001);

PULP_VST3_PLUGIN(HostProbeUID, "Pulp Host Probe", Steinberg::Vst::PlugType::kFx, "Pulp", "1.0.0",
                 "https://github.com/Generous-Corp/pulp", pulp::test_fixtures::create_host_probe)
