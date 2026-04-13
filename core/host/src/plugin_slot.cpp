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
#if PULP_HOST_HAS_AU
std::unique_ptr<PluginSlot> load_au_plugin(const PluginInfo& info);
#endif
#if PULP_HOST_HAS_VST3
std::unique_ptr<PluginSlot> load_vst3_plugin(const PluginInfo& info);
#endif
#if PULP_HOST_HAS_LV2
std::unique_ptr<PluginSlot> load_lv2_plugin(const PluginInfo& info);
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
        case PluginFormat::AudioUnit:
        case PluginFormat::AudioUnitV3:
#if PULP_HOST_HAS_AU
            return load_au_plugin(info);
#else
            runtime::log_warn("PluginSlot::load: AU loader only available on macOS");
            return nullptr;
#endif
        case PluginFormat::VST3:
#if PULP_HOST_HAS_VST3
            return load_vst3_plugin(info);
#else
            runtime::log_warn("PluginSlot::load: VST3 loader not compiled in");
            return nullptr;
#endif
        case PluginFormat::LV2:
#if PULP_HOST_HAS_LV2
            return load_lv2_plugin(info);
#else
            runtime::log_warn("PluginSlot::load: LV2 loader not compiled in");
            return nullptr;
#endif
    }
    return nullptr;
}

} // namespace pulp::host
