// Format entry points.
//
// Each format needs its own exported symbol -- clap_entry, GetPluginFactory,
// an AU component entry -- and none of them can be shared, so they live here
// together rather than in three near-identical files. Everything they wrap is
// the same Shell.
//
// The identifiers below are permanent once shipped: a DAW keys saved sessions
// off them, so changing one orphans every project that used the plugin.

#include "forge_modular/shell.hpp"

#if defined(PULP_FORGE_MODULAR_CLAP)
#include <pulp/format/clap_entry.hpp>
PULP_CLAP_PLUGIN(forge_modular::create_shell)

#elif defined(PULP_FORGE_MODULAR_VST3)
#include <pulp/format/vst3_entry.hpp>
// Generated once, never changed across versions: 'Forg','eMod' as stable words.
static const Steinberg::FUID ForgeModularUID(0x466F7267, 0x654D6F64,
                                             0x00000001, 0x00000001);
PULP_VST3_PLUGIN(ForgeModularUID, "Forge Modular",
                 Steinberg::Vst::PlugType::kFx,
                 "Generous", "0.1.0", "",
                 forge_modular::create_shell)

#elif defined(PULP_FORGE_MODULAR_AU)
#include <pulp/format/au_v2_entry.hpp>
// The class name must equal the CMake target exactly: the AU entry generates
// <ClassName>Factory and the bundle looks for <target>AUFactory, so the class
// must be named <target>AU -- ForgeModularAppAU, not ForgeModularApp. Near
// enough is
// a link error, and the message names a symbol that appears nowhere in source.
PULP_AU_PLUGIN(ForgeModularAppAU, forge_modular::create_shell)

#endif
