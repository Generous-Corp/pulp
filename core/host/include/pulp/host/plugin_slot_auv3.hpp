#pragma once

// Loading an Audio Unit through AUAudioUnit (macOS).
//
// PluginSlot::load() sends PluginFormat::AudioUnitV3 here with the default
// options, which run the plugin out of process: the system hosts it in its
// Audio Unit hosting service, and a crash there cannot take the host down. The
// same path loads AU v2 components, which the system wraps as AUAudioUnit.
// Call load_auv3_plugin() directly to choose in-process loading or a different
// instantiation timeout.

#include <pulp/host/plugin_slot.hpp>

#include <chrono>
#include <memory>

namespace pulp::host {

struct AuV3LoadOptions {
    /// Instantiate with kAudioComponentInstantiation_LoadOutOfProcess. In
    /// process is faster to render and lets the host share memory with the
    /// plugin, at the cost of sharing its crashes.
    bool out_of_process = true;
    /// How long to wait for the system to finish instantiating the plugin.
    std::chrono::milliseconds instantiate_timeout{10000};
};

/// Load the Audio Unit named by `info.unique_id` ("TYPE:SUBT:MANU" four-char
/// codes, as the scanner writes them). Null when no such component is
/// registered, instantiation fails, or it does not finish within the timeout.
/// Available only when PULP_HOST_HAS_AU is defined (macOS).
std::unique_ptr<PluginSlot> load_auv3_plugin(const PluginInfo& info,
                                             const AuV3LoadOptions& options = {});

} // namespace pulp::host
