// CLAP entry point for Forge Modular.
//
// Identity derives from descriptor() at module load: the plugin id is the
// descriptor's bundle_id (com.generous.forge.modular — stable, never change
// it) and the feature list follows the descriptor's category.
#include "forge/modular_shell.hpp"

#include <pulp/format/clap_entry.hpp>

PULP_CLAP_PLUGIN(forge_modular::create_forge_modular)
