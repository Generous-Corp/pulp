// MidiLearnDemo VST3 entry point

#include "midi_learn_demo.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID MidiLearnDemoUID(0x50554C50, 0x4D4C4400, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(MidiLearnDemoUID, "MidiLearnDemo", Steinberg::Vst::PlugType::kFx, "Pulp", "1.0.0",
                 "https://github.com/Generous-Corp/pulp",
                 pulp::examples::create_midi_learn_demo_vst3)
