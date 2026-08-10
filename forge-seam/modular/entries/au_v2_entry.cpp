// AU v2 entry point for Forge Modular.
//
// The class name is load-bearing: AUSDK_COMPONENT_ENTRY derives the exported
// factory symbol from it (ForgeModularAU -> ForgeModularAUFactory), and the AU
// packaging writes `${target}AUFactory` into the Info.plist factoryFunction +
// export list — so the class MUST be named `<cmake target>AU`.
#include "forge/modular_shell.hpp"

#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(ForgeModularAU, forge_modular::create_forge_modular)
