#pragma once

// Plugin registry — connects format adapters to processor factories
// Each plugin binary registers its factory at static init time.
// Format adapters (VST3, AU, CLAP) query the registry to create processors.

#include <pulp/format/processor.hpp>

namespace pulp::format {

// The global processor factory. Set once per plugin binary.
// Thread-safe: set at static init, read at runtime.
ProcessorFactory& registered_factory();

// Call this from your plugin's entry point (or use PULP_REGISTER_PLUGIN macro)
void register_plugin(ProcessorFactory factory);

} // namespace pulp::format

// Macro for plugin developers — place in ONE .cpp file per plugin
#define PULP_REGISTER_PLUGIN(factory_fn) \
    namespace { \
        struct PulpPluginRegistrar { \
            PulpPluginRegistrar() { ::pulp::format::register_plugin(factory_fn); } \
        } _pulp_registrar; \
    }
