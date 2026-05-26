#pragma once

// PluginHostType — detect which DAW/host is running the plugin.
// Identifies the host from the process name at runtime.

#include <string>
#include <string_view>

namespace pulp::format {

enum class HostType {
    Unknown,
    LogicPro,
    GarageBand,
    AbletonLive,
    Reaper,
    ProTools,
    Cubase,
    Nuendo,
    Wavelab,
    StudioOne,
    FLStudio,
    Bitwig,
    Maschine,
    AudacityTenacity,
    Ardour,
    Standalone,  // Pulp standalone host
    Other
};

/// Detect the current plugin host from the running process.
/// Call once during plugin initialization and cache the result.
HostType detect_host_type();

/// Classify a host from an executable path or process name.
HostType host_type_from_process_name(std::string_view process_name);

/// Get a human-readable name for the host type.
std::string host_type_name(HostType type);

/// Whether the host supports specific features (heuristic).
bool host_supports_resize(HostType type);
bool host_supports_sidechain(HostType type);

}  // namespace pulp::format
