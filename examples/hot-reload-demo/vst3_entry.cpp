// Hot-Reload Demo — VST3 entry point.
#include "hot_reload_shell.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change.
static const Steinberg::FUID HotReloadDemoUID(0x50554C50, 0x48524C44, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(HotReloadDemoUID, "Pulp Hot-Reload Demo", Steinberg::Vst::PlugType::kFx,
                 "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_hot_reload_shell)
