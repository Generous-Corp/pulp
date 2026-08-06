// PulpDesignArp VST3 entry point
// Uses PULP_VST3_PLUGIN() macro for minimal boilerplate

#include "design_panel_midi_effect.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change
static const Steinberg::FUID PulpDesignArpUID(0x50554C50, 0x41525000, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpDesignArpUID, "PulpDesignArp", Steinberg::Vst::PlugType::kInstrument,
                  "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                  pulp::examples::create_design_panel_midi_effect)
