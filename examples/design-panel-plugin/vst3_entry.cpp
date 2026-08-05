// PulpDesignPanel VST3 entry point
// Uses PULP_VST3_PLUGIN() macro for minimal boilerplate

#include "design_panel_plugin.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change
static const Steinberg::FUID PulpDesignPanelUID(0x50554C50, 0x47414900, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpDesignPanelUID, "PulpDesignPanel", Steinberg::Vst::PlugType::kFx,
                  "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                  pulp::examples::create_design_panel)
