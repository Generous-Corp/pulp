// VST3 entry point for Forge Modular.
//
// The registration strings come from the identity manifest via CMake, the same
// as its three siblings; the runtime descriptor() derives them from the brand
// seam. VST3's FUID needs compile-time literals, so a rename touches
// brand.hpp AND these.
#include "forge/modular_shell.hpp"

#include <pulp/format/vst3_entry.hpp>

#ifndef FORGE_PLUGIN_VERSION
#error "Forge Modular VST3 version must come from the CMake project version"
#endif
#ifndef FORGE_PLUGIN_NAME
#error "Forge Modular VST3 name must come from the identity manifest"
#endif
#ifndef FORGE_PLUGIN_MANUFACTURER
#error "Forge Modular VST3 manufacturer must come from the identity manifest"
#endif

// Unique VST3 identity — generated once, never changed across versions.
// ('Forg','eMod' as stable 32-bit words, distinct from FX/Instrument/MIDI.)
static const Steinberg::FUID ForgeModularUID(0x466F7267, 0x654D6F64, 0x00000001,
                                             0x00000001);

PULP_VST3_PLUGIN(ForgeModularUID, FORGE_PLUGIN_NAME,
                 Steinberg::Vst::PlugType::kFx, FORGE_PLUGIN_MANUFACTURER,
                 FORGE_PLUGIN_VERSION, "", forge_modular::create_forge_modular)
