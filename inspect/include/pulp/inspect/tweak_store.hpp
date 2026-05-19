// tweak_store.hpp — Phase 0b in-memory tweak table + Phase 1 disk persistence.
//
// Scope (planning/2026-05-18-inspector-direct-manipulation-roadmap.md):
//   Phase 0b landed the data layer — protocol method + in-memory table
//   + bridge handler stub.
//   Phase 1 adds `pulp-tweaks.json` read/write so edits survive process
//   restart. Direct-manipulation gesture capture is Phase 0b PR-B;
//   property-panel dot indicators are Phase 0b PR-C.
//
// On-disk schema (mirrors @pulp/import-ir/src/tweaks.ts TweaksFile with
// the Phase 1 bypass + version additions):
//
//   {
//     "$schema": "pulp-tweaks://v1",
//     "version": 1,
//     "tweaks":   Record<anchor, Record<dottedPath, value>>,
//     "bypassed": Record<anchor, true | string[]>,
//     "sources":  Record<anchor, Record<dottedPath, string>>   // optional
//   }
//
// Per Codex Phase 0b review: bypass IS a SIBLING overlay, not
// `applied:false` mixed into the dotted-path tweak map. That preserves
// v1 backwards compatibility — old files with only `tweaks` still load.
//
// Atomic-write contract: save_to_disk() writes to `<path>.tmp` then
// renames over the target so a crash mid-write never leaves a
// half-flushed file at the canonical path.

#pragma once

#include <choc/containers/choc_Value.h>

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pulp::inspect {

/// In-memory record of inspector direct-manipulation edits ("tweaks"),
/// keyed by stable_anchor_id (Phase 0a) + dotted property path
/// (e.g. "paint.backgroundColor", "layout.padding").
///
/// Thread-safety: external mutex; all public methods take the same
/// internal mutex so the inspector can safely receive concurrent
/// applyTweak / listTweaks calls from multiple connected clients.
class TweakStore {
public:
    /// A single tweak record returned by list_tweaks().
    struct Record {
        std::string anchor_id;
        std::string property_path;  // dotted, e.g. "paint.backgroundColor"
        choc::value::Value value;
        std::string source;         // optional adapter / origin tag (free-form)
    };

    /// Bypass overlay value — matches the TS `true | string[]` shape.
    /// `bool=true` bypasses every tweak under an anchor; a non-empty
    /// vector bypasses only the listed dotted paths.
    using BypassValue = std::variant<bool, std::vector<std::string>>;

    TweakStore() = default;

    // ── Mutation ────────────────────────────────────────────────────

    /// Record an edit. Overwrites any prior value at the same
    /// (anchor_id, property_path). Returns the post-write tweak count.
    std::size_t apply_tweak(std::string_view anchor_id,
                            std::string_view property_path,
                            choc::value::Value value,
                            std::string_view source = {});

    /// Remove a single (anchor_id, property_path) entry. Returns true
    /// if something was removed.
    bool remove_tweak(std::string_view anchor_id,
                      std::string_view property_path);

    /// Remove all entries for an anchor. Returns the number of entries
    /// removed.
    std::size_t remove_anchor(std::string_view anchor_id);

    /// Clear the entire table (tweaks + bypass overlay).
    void clear();

    /// Set the bypass state for an anchor.
    /// - `true` bypasses every tweak under the anchor.
    /// - A non-empty vector bypasses only those dotted paths.
    /// - An empty vector or `false` clears the bypass entirely.
    void set_bypass(std::string_view anchor_id, BypassValue value);

    /// Convenience: clear the bypass for an anchor (equivalent to
    /// set_bypass(id, false)).
    void clear_bypass(std::string_view anchor_id);

    // ── Inspection ──────────────────────────────────────────────────

    /// Total number of (anchor, property) entries.
    std::size_t count() const;

    /// Return every record. Order is insertion-stable within an anchor
    /// and unspecified across anchors.
    std::vector<Record> list_tweaks() const;

    /// Look up a specific (anchor_id, property_path). Returns the value
    /// if set, std::nullopt otherwise.
    std::optional<choc::value::Value>
    lookup(std::string_view anchor_id, std::string_view property_path) const;

    /// Whether a specific (anchor_id, property_path) is currently
    /// bypassed (either whole-anchor or path-specific).
    bool is_bypassed(std::string_view anchor_id,
                     std::string_view property_path) const;

    /// Current bypass state for an anchor. Returns std::nullopt if the
    /// anchor has no bypass entry; otherwise the live BypassValue.
    std::optional<BypassValue>
    bypass_for(std::string_view anchor_id) const;

    // ── Phase 1: disk persistence ───────────────────────────────────

    /// On-disk schema version. Bumped if/when we ever break the format.
    static constexpr int kSchemaVersion = 1;

    /// Outcome of a disk operation. `ok` is the only success path; the
    /// rest carry a human-readable error message.
    struct DiskResult {
        bool ok = false;
        std::string error;
        std::size_t tweak_count = 0;
        std::size_t bypass_count = 0;
        std::string path;       // resolved path actually used
    };

    /// Replace the in-memory state with the contents of `path`. Returns
    /// `ok=false` (without touching internal state) if the file is
    /// missing, malformed, or has an unsupported schema version.
    ///
    /// If `path` is empty, resolves via default_tweaks_path().
    DiskResult load_from_disk(std::string_view path = {});

    /// Atomically write the current in-memory state to `path`. Writes
    /// to `<path>.tmp` first then renames; on success the .tmp file no
    /// longer exists at the canonical path.
    ///
    /// If `path` is empty, resolves via default_tweaks_path().
    DiskResult save_to_disk(std::string_view path = {}) const;

    /// Enable / disable post-mutation auto-save. When enabled, every
    /// successful apply_tweak / remove_tweak / remove_anchor / clear /
    /// set_bypass / clear_bypass call flushes the table to disk at
    /// `path` (or default_tweaks_path() if empty). Default OFF so unit
    /// tests don't write to disk by accident.
    ///
    /// Set `enabled=false` to disable. The `path` is ignored when
    /// disabling.
    void set_auto_save(bool enabled, std::string_view path = {});

    /// Whether auto-save is currently armed.
    bool auto_save_enabled() const;

    /// The path auto-save flushes to (empty if auto-save is off and no
    /// explicit path has ever been set).
    std::string auto_save_path() const;

    /// Resolve the default `pulp-tweaks.json` location:
    ///   1. `$PULP_TWEAKS_FILE` env var if set (verbatim — caller's
    ///      responsibility to make sense of it).
    ///   2. Walk up from cwd looking for `package.json`; if found, use
    ///      `<project_root>/pulp-tweaks.json`.
    ///   3. Otherwise `<cwd>/pulp-tweaks.json`.
    static std::string default_tweaks_path();

    /// Serialize the current state to a JSON string in the on-disk
    /// schema. Public so callers (e.g. the protocol layer's saveTweaks
    /// response) can preview without writing.
    std::string to_json() const;

    /// Replace state from an in-memory JSON string. Same error
    /// semantics as load_from_disk(). Public so tests can round-trip
    /// without touching the filesystem.
    DiskResult from_json(std::string_view json);

private:
    mutable std::mutex mtx_;
    // Outer key: anchor_id. Inner key: dotted property path.
    // Inner value: the most-recent assigned value + source.
    struct Entry {
        choc::value::Value value;
        std::string source;
    };
    std::unordered_map<std::string,
                       std::unordered_map<std::string, Entry>> tweaks_;
    std::unordered_map<std::string, BypassValue> bypassed_;

    // Auto-save state. When `auto_save_` is true, every successful
    // mutation re-flushes the table to `auto_save_path_` after the
    // mutation completes. Both fields are guarded by `mtx_`.
    bool auto_save_ = false;
    std::string auto_save_path_;

    // Helpers (assume mtx_ held by caller unless noted).
    std::string to_json_locked() const;
    DiskResult from_json_locked(std::string_view json);
    DiskResult save_locked(std::string_view path) const;
    // Auto-save trigger. Called by every mutating public method AFTER
    // it releases its lock; takes its own lock briefly to read auto-
    // save state. Returns silently if auto-save is off.
    void maybe_auto_save_unlocked() const;
};

}  // namespace pulp::inspect
