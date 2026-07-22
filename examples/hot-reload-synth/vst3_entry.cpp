// Hot-Reload Synth — VST3 instrument entry point.
#include "hot_reload_synth_shell.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change.
static const Steinberg::FUID HotReloadSynthUID(0x50554C50, 0x48525359, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(HotReloadSynthUID, "Pulp Hot-Reload Synth",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                 pulp::examples::create_hot_reload_synth)
