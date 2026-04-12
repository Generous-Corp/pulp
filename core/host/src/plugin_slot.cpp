// PluginSlot::load() — format dispatcher.
//
// Routes the load request to the format-specific loader. Each loader lives
// in its own translation unit (plugin_slot_clap.cpp, plugin_slot_vst3.cpp,
// …) and is compiled in only when the relevant SDK is available, guarded
// by PULP_HOST_HAS_<FORMAT> compile definitions in core/host/CMakeLists.txt.

#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/log.hpp>

namespace pulp::host {

#if PULP_HOST_HAS_CLAP
std::unique_ptr<PluginSlot> load_clap_plugin(const PluginInfo& info);
#endif

std::unique_ptr<PluginSlot> PluginSlot::load(const PluginInfo& info) {
    switch (info.format) {
        case PluginFormat::CLAP:
#if PULP_HOST_HAS_CLAP
            return load_clap_plugin(info);
#else
            runtime::log_warn("PluginSlot::load: CLAP loader not compiled in");
            return nullptr;
#endif
        case PluginFormat::VST3:
        case PluginFormat::AudioUnit:
        case PluginFormat::AudioUnitV3:
        case PluginFormat::LV2:
            runtime::log_warn("PluginSlot::load: loader for this format not yet implemented ('{}')",
                              info.path);
            return nullptr;
    }
    return nullptr;
}

} // namespace pulp::host
