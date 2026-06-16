#pragma once

/// @file preset_manager.hpp
/// Built-in preset management — save/load/delete/rename/import.
/// No more rolling your own PresetManager for every plugin.

#include <pulp/state/store.hpp>
#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <optional>

namespace pulp::state {

struct ContentCapabilityManifest;

/// Metadata for a single preset file.
struct PresetInfo {
    std::string name;                  ///< Display name (filename without extension)
    std::filesystem::path path;        ///< Full path to the preset file
    bool is_factory = false;           ///< True if this is a bundled factory preset
    std::string folder;                ///< Relative folder path (empty for root)
    std::string author;                ///< Optional author name
    std::string description;           ///< Optional description
    std::vector<std::string> tags;     ///< Optional category tags
};

/// Built-in preset manager for Pulp plugins.
///
/// Handles the full preset lifecycle: save, load, delete, rename, import,
/// factory vs user separation, folder scanning, and current-preset tracking.
///
/// Preset files are JSON with the plugin's serialized state plus metadata.
///
/// Storage locations (platform-standard):
/// - macOS: ~/Library/Audio/Presets/<manufacturer>/<plugin_name>/
/// - Windows: %APPDATA%/<manufacturer>/<plugin_name>/Presets/
/// - Linux: ~/.config/<manufacturer>/<plugin_name>/presets/
///
/// Factory presets live in the plugin bundle's Resources/Presets/ directory.
///
/// @code
/// PresetManager presets(store, "MyCompany", "MyPlugin");
/// presets.save("Init");
/// presets.load(presets.user_presets()[0]);
/// @endcode
class PresetManager {
public:
    /// Construct with a StateStore and plugin identity for path resolution.
    PresetManager(StateStore& store, const std::string& manufacturer,
                  const std::string& plugin_name);

    // ── Save/Load ────────────────────────────────────────────────────────

    /// Save current state as a named preset in the user presets folder.
    /// @param name Preset name (used as filename, .json appended)
    /// @param folder Optional subfolder within user presets directory
    /// @return True on success.
    bool save(const std::string& name, const std::string& folder = "");

    /// Load a preset from a PresetInfo (sets all parameter values).
    /// @return True on success.
    bool load(const PresetInfo& preset);

    /// Load a preset by file path.
    bool load(const std::filesystem::path& path);

    /// Delete a user preset file. Factory presets cannot be deleted.
    bool delete_preset(const PresetInfo& preset);

    /// Rename a user preset. Factory presets cannot be renamed.
    bool rename(const PresetInfo& preset, const std::string& new_name);

    /// Import a preset file from an external location into user presets.
    std::optional<PresetInfo> import_file(const std::filesystem::path& source);

    // ── Discovery ────────────────────────────────────────────────────────

    /// Get all presets (factory + user), sorted alphabetically.
    std::vector<PresetInfo> all_presets() const;

    /// Get factory presets only.
    std::vector<PresetInfo> factory_presets() const;

    /// Get user presets only.
    std::vector<PresetInfo> user_presets() const;

    /// Get installed content-pack presets matching this plugin's content
    /// capabilities. These are read-only expansion/factory presets.
    std::vector<PresetInfo> content_presets() const;

    /// Opt into installed content-pack discovery for this plugin id.
    void set_content_plugin_id(std::string plugin_id);

    /// Limit installed content packs to matching capabilities such as
    /// `content.presets.v1`.
    void set_content_capabilities(std::vector<std::string> capabilities);

    /// Override the content data root. Intended for tests and explicit
    /// sandboxing; normal plugins use the platform user-data root.
    void set_content_data_root(std::filesystem::path data_root);

    /// Opt into content discovery from a parsed `pulp.plugin-runtime.v1`
    /// manifest embedded in the plugin/app bundle.
    void set_content_manifest(const ContentCapabilityManifest& manifest);

    /// Rescan preset directories (call after external changes).
    void refresh();

    // ── Current preset tracking ──────────────────────────────────────────

    /// The currently loaded preset (empty if unsaved/modified).
    const std::string& current_preset_name() const { return current_name_; }
    void set_current_preset_name(const std::string& name) { current_name_ = name; }

    /// True if parameters have changed since the last load/save.
    bool has_unsaved_changes() const { return unsaved_changes_; }
    void mark_as_changed() { unsaved_changes_ = true; }
    void mark_as_saved() { unsaved_changes_ = false; }

    // ── Navigation ───────────────────────────────────────────────────────

    /// Load the next preset in the list. Wraps around.
    bool load_next();
    /// Load the previous preset in the list. Wraps around.
    bool load_previous();

    // ── Callbacks ────────────────────────────────────────────────────────

    /// Called after a preset is loaded.
    std::function<void(const PresetInfo&)> on_preset_loaded;

    /// Called after the preset list changes (save/delete/rename/import).
    std::function<void()> on_list_changed;

    // ── Paths ────────────────────────────────────────────────────────────

    /// The user presets directory.
    std::filesystem::path user_presets_dir() const { return user_dir_; }

    /// The factory presets directory (inside the plugin bundle).
    std::filesystem::path factory_presets_dir() const { return factory_dir_; }

private:
    StateStore& store_;
    std::string manufacturer_;
    std::string plugin_name_;
    std::filesystem::path user_dir_;
    std::filesystem::path factory_dir_;
    std::filesystem::path content_data_root_;
    std::string content_plugin_id_;
    std::vector<std::string> content_capabilities_;
    std::vector<std::string> content_kinds_;

    std::string current_name_;
    bool unsaved_changes_ = false;

    std::vector<PresetInfo> cached_presets_;
    bool cache_valid_ = false;

    void scan_directory(const std::filesystem::path& dir, bool is_factory,
                        std::vector<PresetInfo>& out) const;
    void ensure_user_dir();
    static std::filesystem::path platform_presets_root();
};

} // namespace pulp::state
