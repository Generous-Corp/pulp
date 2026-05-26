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
    Mixbus32C,   // Harrison Mixbus 32C — Ardour derivative with separate quirk profile
    Standalone,  // Pulp standalone host
    Other
};

/// Detect the current plugin host from the running process.
/// Call once during plugin initialization and cache the result.
///
/// On Apple platforms this prefers the AU v3 wrapper-reported host
/// identifier (see `host_type_from_auv3_wrapper`) before falling back
/// to the executable-path heuristic — AU v3 plug-ins live in a
/// sandboxed `.appex` whose own process name is the wrapper service,
/// not the host. DAW-quirks row 22 (item 5.11 / 3.1).
HostType detect_host_type();

/// Classify a host from an executable path or process name.
HostType host_type_from_process_name(std::string_view process_name);

/// Classify the AU v3 wrapper-reported host identifier (row 22).
///
/// AU v3 plug-ins run in a sandboxed extension; the host executable
/// path is not visible in the bundle's address space. Apple's
/// `AUHostingService` (and the older XPC wrappers Logic Pro / MainStage /
/// GarageBand spawn) make the *bundle identifier* of the wrapping host
/// available through `[NSProcessInfo processInfo].processName` (the
/// wrapper executable name, prefixed with the host bundle id) or the
/// `auHostIdentifier` user-info key on some wrappers. This helper
/// normalises any such identifier to a `HostType`.
///
/// The function is platform-independent so it can be unit-tested without
/// having to run inside a real AU extension. The Apple-only
/// `detect_host_type()` calls this with the wrapper-reported string
/// before falling back to the executable-path heuristic.
///
/// Returns `HostType::Unknown` when the identifier is empty or does not
/// match any known wrapper. Examples that resolve to known hosts:
///   • "com.apple.logic10"                → `HostType::LogicPro`
///   • "com.apple.garageband10"           → `HostType::GarageBand`
///   • "com.apple.mainstage"              → `HostType::LogicPro` (Logic/MainStage
///                                          share the same Pro Audio quirk family)
///   • "AUHostingServiceXPC_arrow"        → `HostType::LogicPro` (Logic 10.5+
///                                          XPC wrapper service name)
///   • "AUHostingService"                 → `HostType::Unknown` (generic — host
///                                          identity needs to come from another
///                                          channel; caller falls back)
///   • "com.kymatica.AUM"                 → `HostType::Unknown` (iOS host;
///                                          first known wrapper to graduate goes
///                                          here when it does)
HostType host_type_from_auv3_wrapper(std::string_view wrapper_identifier);

/// Apple-only: query the AU v3 wrapper-reported host identifier from
/// `NSProcessInfo` and any environment variables Apple's host services
/// set. Returns an empty string on non-Apple platforms or when no
/// wrapper context is detected. Exposed for tests + `detect_host_type`.
std::string current_auv3_wrapper_identifier();

/// Get a human-readable name for the host type.
std::string host_type_name(HostType type);

/// Whether the host supports specific features (heuristic).
bool host_supports_resize(HostType type);
bool host_supports_sidechain(HostType type);

}  // namespace pulp::format
