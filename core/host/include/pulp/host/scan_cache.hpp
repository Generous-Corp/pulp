#pragma once

// Persistent plugin scan cache — workstream 03 slice 3.1.
//
// Stores PluginInfo results keyed by (path, mtime, size). On the second scan
// of an unchanged plugin file, the cache returns the stored PluginInfo
// without re-inspecting the plugin binary. Entries auto-invalidate when the
// underlying file changes.
//
// Format: JSON file. Schema versioned so future changes can force a flush.
// Usage:
//   HostScanCache cache;
//   cache.load_from("/path/to/cache.json");          // best-effort; silent on miss
//   if (auto info = cache.get("/path/to/plugin.vst3")) { ... }
//   else { PluginInfo fresh = ...; cache.put("/path/to/plugin.vst3", fresh); }
//   cache.save_to("/path/to/cache.json");

#include <pulp/host/scanner.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace pulp::host {

struct ScanCacheEntry {
    int64_t mtime = 0;   // seconds since epoch; 0 means "unknown" (never matches)
    int64_t size = 0;    // bytes
    PluginInfo info;
};

class HostScanCache {
public:
    /// Schema version; entries loaded with a different version are discarded.
    static constexpr int kSchemaVersion = 1;

    /// Return a cached entry if one exists whose (mtime, size) matches the
    /// file on disk today. Returns std::nullopt on any mismatch or if the
    /// file is not readable.
    std::optional<PluginInfo> get(const std::string& path) const;

    /// Overwrite (or insert) an entry for `path`. Records the current file
    /// mtime + size from the filesystem.
    void put(const std::string& path, const PluginInfo& info);

    /// Remove the entry for `path` (noop if absent).
    void erase(const std::string& path);

    /// Total number of entries currently in memory.
    std::size_t size() const { return entries_.size(); }

    /// Drop every entry.
    void clear() { entries_.clear(); }

    /// Serialize to a JSON file. Creates parent directories if needed.
    /// Returns false on I/O failure.
    bool save_to(const std::string& file_path) const;

    /// Load a JSON file produced by save_to(). Returns false if the file is
    /// missing, unreadable, or has a different schema version (cache is
    /// then left in its current state — callers can rebuild). Does NOT
    /// validate per-entry mtime/size against the filesystem; that happens
    /// inside get().
    bool load_from(const std::string& file_path);

    // ── Serialization helpers exposed for unit tests ──────────────────────
    std::string to_json() const;
    bool from_json(const std::string& json);

    // ── Raw access for tests / diagnostics ────────────────────────────────
    const std::unordered_map<std::string, ScanCacheEntry>& entries() const {
        return entries_;
    }

private:
    std::unordered_map<std::string, ScanCacheEntry> entries_;
};

} // namespace pulp::host
