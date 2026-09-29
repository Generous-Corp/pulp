#include "tail_edge_probe.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID TailEdgeProbeUID(0x50554C50, 0x54514E51, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(TailEdgeProbeUID, "Pulp Test (dev) Plugin", Steinberg::Vst::PlugType::kFx,
                 "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                 pulp::test_fixtures::create_tail_edge_probe)
