#pragma once

/// @file content_capability_manifest.hpp
/// The capabilities a plugin declares before consuming installed content
/// packs, and the `pulp.plugin-runtime.v1` JSON form a plugin bundle embeds.
/// pulp-state owns the value and its wire format; reading installed content
/// packs is pulp-content's job (content_registry.hpp), so code that only needs
/// to read or write a bundle's manifest does not depend on the content-pack
/// reader.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

/// Parse the plugin-side content capability manifest embedded in a built
/// plugin/app bundle. Schema:
/// `{ "schema":"pulp.plugin-runtime.v1", "pluginId":"...",
///    "content": { "capabilities":[...], "kinds":[...],
///                 "reload": { "hotReloadKinds":[...],
///                             "manualRescanKinds":[...] } } }`
std::optional<ContentCapabilityManifest>
parse_content_capability_manifest(std::string_view json, std::string* error = nullptr);

/// Load and parse a plugin content capability manifest from disk.
std::optional<ContentCapabilityManifest>
load_content_capability_manifest(const std::filesystem::path& path, std::string* error = nullptr);

/// Serialize a capability manifest for embedding in plugin bundle resources.
std::string content_capability_manifest_to_json(const ContentCapabilityManifest& manifest);

} // namespace pulp::state
