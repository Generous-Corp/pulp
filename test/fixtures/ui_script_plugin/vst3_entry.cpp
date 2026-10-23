#include "ui_script_gain.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID UiScriptGainUID(0x50554C50, 0x55695363, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(UiScriptGainUID, "Pulp UI Script Fixture", Steinberg::Vst::PlugType::kFx, "Pulp",
                 "1.0.0", "https://github.com/Generous-Corp/pulp",
                 pulp::test_fixtures::create_ui_script_gain)
