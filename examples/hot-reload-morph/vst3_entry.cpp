// Hot-Reload Morph — VST3 entry point.
#include "morph_shell.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID HotReloadMorphUID(0x50554C50, 0x4D4F5250, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(HotReloadMorphUID, "Pulp Hot-Reload Morph", Steinberg::Vst::PlugType::kFx,
                 "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_morph_shell)
