// PulpDesignSynth VST3 entry point
// Uses PULP_VST3_PLUGIN() macro for minimal boilerplate

#include "design_panel_instrument.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change
static const Steinberg::FUID PulpDesignSynthUID(0x50554C50, 0x53594E00, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpDesignSynthUID, "PulpDesignSynth", Steinberg::Vst::PlugType::kInstrumentSynth,
                  "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                  pulp::examples::create_design_panel_instrument)
