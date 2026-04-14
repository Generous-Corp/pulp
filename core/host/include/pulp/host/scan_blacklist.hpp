#pragma once

// Scan blacklist (workstream 03 slice 3.3a) — companion to HostScanCache.
//
// When a plugin crashes the scanner, the out-of-process scan worker
// (slice 3.3b, lands separately) records the plugin path + its (mtime,
// size) pair here. Subsequent scans skip blacklisted entries entirely
// so one bad plugin can't loop the entire scan on every startup.
//
// Format is a newline-delimited text file — one entry per line — so a
// user can hand-edit if they want to retry a blacklisted plugin. Lines
// starting with `#` are comments.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::host {

struct BlacklistEntry {
    int64_t mtime = 0;
    int64_t size = 0;
    std::string reason;   ///< free-text explanation (e.g. "SIGSEGV", "timeout")
};

class ScanBlacklist {
public:
    /// Return the blacklist entry for `path` if one exists AND matches
    /// the current file stamp. A rebuilt / upgraded plugin (different
    /// mtime/size) is NOT considered blacklisted — users get a fresh
    /// scan attempt on the new version.
    std::optional<BlacklistEntry> get(const std::string& path) const;

    /// Record that scanning `path` failed. Stores the current file
    /// stamp so a later rebuild invalidates the entry automatically.
    void blacklist(const std::string& path, std::string reason);

    /// Unconditionally remove `path` from the blacklist. Users can
    /// retry a plugin by calling this.
    void clear(const std::string& path) { entries_.erase(path); }

    std::size_t size() const { return entries_.size(); }
    bool is_blacklisted(const std::string& path) const {
        return get(path).has_value();
    }

    /// Serialize to the plain-text format described at the top.
    std::string to_text() const;

    /// Load from the plain-text format. Returns false on I/O error;
    /// malformed lines are silently skipped (so a corrupted file
    /// downgrades to "start fresh" rather than blocking the host).
    bool from_text(const std::string& text);

    bool save_to(const std::string& file_path) const;
    bool load_from(const std::string& file_path);

    const std::unordered_map<std::string, BlacklistEntry>& entries() const {
        return entries_;
    }

private:
    std::unordered_map<std::string, BlacklistEntry> entries_;
};

} // namespace pulp::host
