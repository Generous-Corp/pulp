// tweak_store.hpp — Phase 0b in-memory tweak table.
//
// Scope (planning/2026-05-18-inspector-direct-manipulation-roadmap.md):
//   Phase 0b lands ONLY the data layer — protocol method + in-memory
//   table + bridge handler stub. Disk persistence (`pulp-tweaks.json`)
//   is Phase 1. Direct-manipulation gesture capture is Phase 0b PR-B.
//   Property-panel dot indicators are Phase 0b PR-C.
//
// Schema mirrors @pulp/import-ir/src/tweaks.ts TweaksFile (see
// packages/pulp-import-ir/src/tweaks.ts:138):
//
//   tweaks: Record<anchor, Record<dottedPath, value>>
//   bypassed?: Record<anchor, true | string[]>
//
// Per Codex Phase 0b review: bypass MUST be a SIBLING overlay, not
// `applied:false` mixed into the dotted-path tweak map. That preserves
// v1 backwards compatibility when Phase 1 starts reading existing
// tweaks files from disk.

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
};

}  // namespace pulp::inspect
