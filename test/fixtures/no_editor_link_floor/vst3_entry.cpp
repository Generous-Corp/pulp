#include "no_editor_gain.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID NoEditorGainUID(0x50554C50, 0x4E6F4564, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(NoEditorGainUID, "Pulp No Editor Link Floor", Steinberg::Vst::PlugType::kFx,
                 "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                 pulp::test_fixtures::create_no_editor_gain)
