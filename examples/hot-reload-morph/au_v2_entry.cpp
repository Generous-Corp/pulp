// Hot-Reload Morph — AU v2 entry (aufx effect). Reloading the logic swaps BOTH
// the DSP and the editor (create_view forwarding).
#include "morph_shell.hpp"
#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(HotReloadMorphAU, pulp::examples::create_morph_shell)
