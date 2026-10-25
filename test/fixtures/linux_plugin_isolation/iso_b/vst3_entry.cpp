#include "../isolation_processor.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID IsolationUID(0x50554C50, 0x49736F00, 0x00000042, 0x00000001);

PULP_VST3_PLUGIN(IsolationUID, "Pulp Isolation B", Steinberg::Vst::PlugType::kFx, "Pulp", "1.0.0",
                 "https://github.com/Generous-Corp/pulp", pulp::test_fixtures::create_isolation_b)
