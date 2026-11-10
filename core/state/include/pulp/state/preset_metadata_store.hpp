#pragma once

/// @file preset_metadata_store.hpp
/// Persistent user metadata for presets: favourites, display-name overrides and
/// tag edits, kept apart from the preset files so read-only sources (factory
/// presets, content packs) can carry them and so a star never rewrites a file.
///
/// One JSON file serves every instance and format of a plug-in on the machine.
/// Writes read the file again under a lock and merge per record (newest edit
/// wins), so two instances editing at once do not lose each other's changes.
/// "Newest" is causal: an edit is stamped after the file is merged, with a
/// stamp above every version it replaces, so it wins even when two edits share
/// a clock tick or one machine's clock runs behind.
/// Not thread-safe by itself; PresetLibrary serialises access.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pulp::state {

/// One preset's user edits. `key` identifies the preset (see PresetLibrary).
struct PresetRecord {
    std::string key;
    std::string digest; ///< Content digest at the last edit; used to re-link a moved preset
    bool favourite = false;
    std::string display_name; ///< Empty = no override
    std::vector<std::string> tags_add;
    std::vector<std::string> tags_remove;
    /// Stamp of the last edit: Unix milliseconds, raised when needed to stay
    /// above the version it replaced (so it can run slightly ahead of the clock).
    std::int64_t updated_ms = 0;
    bool deleted = false; ///< Tombstone so a removal survives a merge

    /// True when the record carries no user edit.
    bool empty() const {
        return !favourite && display_name.empty() && tags_add.empty() && tags_remove.empty();
    }
};

class PresetMetadataStore {
  public:
    /// An empty path keeps everything in memory (nothing is persisted).
    explicit PresetMetadataStore(std::filesystem::path file = {});

    /// Read the file. A missing file is an empty store and succeeds; an
    /// unreadable or corrupt one fails and leaves the store empty.
    bool load();

    /// Re-read the file if it changed on disk since the last load or commit.
    /// Returns true when the in-memory records changed.
    bool reload_if_changed();

    const std::filesystem::path& file() const {
        return file_;
    }
    bool persistent() const {
        return !file_.empty();
    }
    /// Wall-clock source for edit stamps, in Unix milliseconds. Defaults to the
    /// system clock; tests set a fixed clock to force edits into one tick. It
    /// also dates tombstone compaction. Set it before the store is shared.
    using Clock = std::function<std::int64_t()>;
    void set_clock(Clock clock) {
        clock_ = std::move(clock);
    }

    /// Incremented on every write to the file.
    std::uint64_t revision() const {
        return revision_;
    }

    /// The live (non-tombstone) record for a key, or nullptr.
    const PresetRecord* find(const std::string& key) const;
    std::vector<PresetRecord> records() const;

    /// Create or update the record for `key`, stamp it, and write the file.
    /// `digest` is stored when non-empty. Returns false if the write failed
    /// (the in-memory change is kept).
    bool set_favourite(const std::string& key, bool favourite, const std::string& digest);
    bool set_display_name(const std::string& key, const std::string& name,
                          const std::string& digest);
    bool add_tag(const std::string& key, const std::string& tag, const std::string& digest);
    bool remove_tag(const std::string& key, const std::string& tag, const std::string& digest);

    /// The program map (see ProgramMap) rides in the same file as one JSON
    /// document, replaced as a whole: the newest write wins. Empty = none.
    const std::string& program_map_json() const {
        return program_map_json_;
    }
    bool set_program_map_json(const std::string& json);

    /// Move a record to a new key (a preset that was renamed or moved). Returns
    /// false, changing nothing, if the old record is gone or the new key already
    /// carries edits once the file is merged.
    bool rekey(const std::string& old_key, const std::string& new_key);
    /// Drop a record (a tombstone is written so the removal merges). Returns
    /// false if the record is already gone once the file is merged.
    bool erase(const std::string& key);

  private:
    template <typename Edit>
    bool edit(const std::string& key, const std::string& digest, Edit&& apply);
    template <typename Apply> bool commit(Apply&& apply);
    std::int64_t clock_now() const;
    std::int64_t next_stamp(std::int64_t after) const;
    struct Disk {
        std::map<std::string, PresetRecord> records;
        std::string program_map_json;
        std::int64_t program_map_updated_ms = 0;
    };
    bool read_file(Disk& out) const;
    bool write_file();
    void merge_disk(const Disk& disk);

    std::filesystem::path file_;
    std::map<std::string, PresetRecord> records_; ///< Includes tombstones
    std::string program_map_json_;
    std::int64_t program_map_updated_ms_ = 0;
    Clock clock_;
    std::uint64_t revision_ = 0;
    std::filesystem::file_time_type seen_mtime_{};
    std::uintmax_t seen_size_ = 0;
    bool seen_file_ = false;
};

} // namespace pulp::state
