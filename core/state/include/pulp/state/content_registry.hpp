#pragma once

/// @file content_registry.hpp
/// Discovery for installed data-only Pulp content packs.

#include <pulp/state/preset_manager.hpp>

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
load_content_capability_manifest(const std::filesystem::path& path,
                                 std::string* error = nullptr);

/// Serialize a capability manifest for embedding in plugin bundle resources.
std::string content_capability_manifest_to_json(const ContentCapabilityManifest& manifest);

/// Metadata for one installed content pack.
struct ContentPackInfo {
    std::string id;
    std::string name;
    std::string version;
    std::filesystem::path root;
    std::vector<std::string> kinds;
    std::vector<std::string> capabilities;
    std::vector<std::filesystem::path> presets;
    std::vector<std::filesystem::path> themes;
    std::vector<std::filesystem::path> samples;
    std::vector<std::filesystem::path> wavetables;
};

enum class ContentReloadPolicy {
    hot_reload,
    manual_rescan,
    restart_required
};

struct ContentInstallPolicy {
    std::string kind;
    ContentReloadPolicy policy = ContentReloadPolicy::restart_required;
};

struct ContentInstallPreview {
    bool ok = false;
    ContentPackInfo pack;
    ContentCapabilityManifest plugin;
    std::filesystem::path source;
    std::filesystem::path install_root;
    std::vector<ContentInstallPolicy> policies;
    std::vector<std::string> issues;
};

struct ContentInstallResult {
    bool ok = false;
    ContentInstallPreview preview;
    std::vector<std::string> issues;
};

/// Installed content lookup. Content packs are data-only and live under the
/// same user-data root used by `pulp content install`.
class ContentRegistry {
public:
    explicit ContentRegistry(std::filesystem::path data_root = platform_data_root());

    static std::filesystem::path platform_data_root();
    static std::filesystem::path content_root_for_data_root(const std::filesystem::path& data_root);

    const std::filesystem::path& data_root() const { return data_root_; }
    std::filesystem::path content_root() const;

    /// Return all installed packs for a plugin id, without capability filtering.
    std::vector<ContentPackInfo> packs_for_plugin(const std::string& plugin_id) const;

    /// Return installed packs whose capabilities/kinds match the plugin manifest.
    std::vector<ContentPackInfo> packs_for_plugin(const ContentCapabilityManifest& manifest) const;

    /// Return content-pack presets as read-only factory/expansion presets.
    std::vector<PresetInfo> presets_for_plugin(const ContentCapabilityManifest& manifest) const;

private:
    std::filesystem::path data_root_;
};

/// Validate a local content-pack directory or .pulpcontent archive against a
/// trusted plugin runtime manifest without copying any files. This is the
/// runtime-side backend for in-app "Install Content Pack..." and drag/drop
/// review flows.
ContentInstallPreview preview_content_pack_install(
    const std::filesystem::path& input,
    const ContentCapabilityManifest& plugin,
    const std::filesystem::path& data_root = ContentRegistry::platform_data_root());

/// Install a previously previewable data-only content pack. `approved` must be
/// true so UI code cannot accidentally turn preview/drop into mutation.
ContentInstallResult install_content_pack(
    const std::filesystem::path& input,
    const ContentCapabilityManifest& plugin,
    const std::filesystem::path& data_root = ContentRegistry::platform_data_root(),
    bool approved = false);

const char* to_string(ContentReloadPolicy policy);

} // namespace pulp::state
