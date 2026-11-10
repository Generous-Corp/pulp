#pragma once

/// @file preset_library.hpp
/// A searchable view over a PresetManager's factory, user and content-pack
/// presets, with favourites, tag edits and display-name overrides that survive
/// content updates.
///
/// Every preset has a stable key (see `make_preset_key`), independent of how
/// the preset is displayed. User edits live in a PresetMetadataStore keyed by
/// that key, so they apply to read-only sources and never rewrite a preset
/// file. The library holds an in-memory index; `query()` never touches disk.
///
/// Threading: all members are safe to call from any thread (an internal mutex
/// serialises them), but none are real-time safe. The audio thread must not
/// call into the library.

#include <pulp/state/preset_manager.hpp>
#include <pulp/state/preset_metadata_store.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// Bumped when the library's public surface changes incompatibly.
#define PULP_PRESET_LIBRARY_API_VERSION 1

namespace pulp::state {

enum class PresetSource { factory, user, content };

/// `factory:<path>`, `user:<path>` or `content:<pack>:<path>`, where `<path>`
/// is the preset's path below its source root, `/`-separated, without the
/// extension. The key survives updates that change a preset's values; it does
/// not survive a rename or move (the library re-links those by digest).
std::string make_preset_key(PresetSource source, std::string_view relative_path,
                            std::string_view pack_id = {});

/// `sha256:<hex>` over the preset's parameter map (names sorted, values at a
/// fixed precision), so metadata and file layout do not change it. A file that
/// is not strict JSON falls back to `raw:<hex>` over its bytes. Empty when the
/// file cannot be read.
std::string preset_digest(const std::filesystem::path& file);

struct PresetSummary {
    std::string key;
    std::string name;         ///< File stem
    std::string display_name; ///< Name shown to the user (override applied)
    PresetSource source = PresetSource::user;
    std::string folder;
    std::vector<std::string> tags; ///< Baseline tags plus edits
    bool favourite = false;
    std::string author;
    bool is_factory = false;
    std::filesystem::path path;
};

enum class PresetSort { name, favourites_first, source };

struct PresetQuery {
    /// Whitespace-separated words; every word must match the name, folder,
    /// tags, author or description, case-insensitively.
    std::string text;
    std::vector<std::string> tags_all; ///< Every listed tag must be present
    std::optional<bool> favourite;
    std::optional<PresetSource> source;
    PresetSort sort = PresetSort::name;
    std::size_t offset = 0;
    std::size_t limit = 0; ///< 0 = no limit
};

struct PresetPage {
    std::vector<PresetSummary> items;
    std::size_t total = 0;        ///< Matches before offset and limit
    std::uint64_t generation = 0; ///< Changes whenever the index or edits change
};

/// What the library can do right now, so a UI can hide what it cannot.
struct PresetLibraryCapabilities {
    bool favourites = true;
    bool tags = true;
    bool search = true;
    bool program_map = false;
    /// False when edits cannot be written (no storage path, or it is not
    /// writable); edits then last for the session only.
    bool persistence = false;
};

class PresetLibrary {
  public:
    /// `overlay_file` holds the user edits; empty keeps them in memory.
    PresetLibrary(PresetManager& manager, std::filesystem::path overlay_file);

    /// Where a plug-in's edits live by default: the per-user application data
    /// folder, under the manufacturer and plug-in names. Never inside the
    /// preset folders (the preset scanner would list the file as a preset).
    /// Empty when the platform location is unknown.
    static std::filesystem::path default_overlay_path(const std::string& manufacturer,
                                                      const std::string& plugin_name);

    /// Rescan the preset folders, pick up edits another instance wrote, and
    /// re-link edits whose preset was renamed or moved.
    void refresh();

    PresetPage query(const PresetQuery& query) const;
    std::optional<PresetSummary> find(const std::string& key) const;
    /// The key a preset has in this library, or empty if it is not indexed.
    std::string key_for(const std::filesystem::path& preset_file) const;

    bool set_favourite(const std::string& key, bool favourite);
    /// An empty name clears the override.
    bool set_display_name(const std::string& key, const std::string& name);
    bool add_tag(const std::string& key, const std::string& tag);
    bool remove_tag(const std::string& key, const std::string& tag);

    /// Edits whose preset is no longer present and could not be re-linked.
    /// They are kept so the edits return with the preset.
    std::size_t orphan_count() const;
    void clear_orphans();

    std::uint64_t generation() const;
    PresetLibraryCapabilities capabilities() const;

    /// Called after an edit or a refresh that changed the index. May run on
    /// any thread that called into the library; do not call back into the
    /// library from it.
    std::function<void()> on_changed;

  private:
    struct Entry {
        std::string key;
        PresetSource source = PresetSource::user;
        PresetInfo info;
    };

    void rebuild_locked();
    void relink_locked();
    PresetSummary summarize_locked(const Entry& entry) const;
    const Entry* find_entry_locked(const std::string& key) const;
    std::string digest_of_locked(const Entry& entry) const;
    bool edit_locked(const std::string& key, const std::function<bool(const std::string&)>& apply);
    void notify(bool changed);

    PresetManager& manager_;
    PresetMetadataStore store_;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> by_key_;
    std::uint64_t generation_ = 1;
    bool persist_ok_ = true;
};

} // namespace pulp::state
