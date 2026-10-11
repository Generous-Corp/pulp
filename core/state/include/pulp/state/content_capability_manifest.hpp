#pragma once

/// @file content_capability_manifest.hpp
/// The capabilities a plugin declares before consuming installed content
/// packs. A plain value type: pulp-state carries it (PresetManager records one)
/// without depending on the content-pack reader in pulp-content.

#include <string>
#include <vector>

namespace pulp::state {

/// Capabilities a plugin declares before consuming installed content packs.
struct ContentCapabilityManifest {
    std::string schema = "pulp.plugin-runtime.v1";
    std::string plugin_id;
    std::vector<std::string> capabilities;
    std::vector<std::string> content_kinds;
    std::vector<std::string> hot_reload_kinds;
    std::vector<std::string> manual_rescan_kinds;
};

} // namespace pulp::state
