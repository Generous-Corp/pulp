#pragma once

// FileSearchPath — ordered list of directories to scan when resolving a
// file by relative name. Mirrors the JUCE class of the same name; see the
// [Reference Framework Gap Analysis](planning/2026-05-24-reference-framework-gap-analysis.md)
// row for FileSearchPath.
//
// Typical use: a plugin needs to locate a "preset.xml" or a font file that
// might live in any of several user-/installer-/bundle-supplied roots, and
// wants the lookup order to be controllable (and persistable into a
// settings file). FileSearchPath holds the ordered roots, finds the first
// match, optionally finds all matches across all roots, and can serialize
// to / from the platform's PATH-style separator (`:` on POSIX, `;` on
// Windows) so it round-trips through `PropertiesFile` cleanly.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::runtime {

class FileSearchPath {
public:
    /// Construct an empty search path.
    FileSearchPath() = default;

    /// Construct from a PATH-style serialized string. Same semantics as
    /// `from_string()` — empty/duplicate entries are dropped.
    explicit FileSearchPath(std::string_view serialized) {
        from_string(serialized);
    }

    /// The platform-native PATH separator: `:` on POSIX, `;` on Windows.
    static constexpr char separator() noexcept {
#if defined(_WIN32) || defined(_WIN64)
        return ';';
#else
        return ':';
#endif
    }

    /// Number of directories in the search path.
    std::size_t size() const noexcept { return paths_.size(); }

    /// True when the search path contains no directories.
    bool empty() const noexcept { return paths_.empty(); }

    /// Access the path at index `i` (no bounds check beyond size()).
    const std::filesystem::path& operator[](std::size_t i) const {
        return paths_[i];
    }

    /// Append `dir` to the end of the search list. No-op if `dir` is
    /// already present (preserves the existing position).
    void add(const std::filesystem::path& dir) {
        if (dir.empty()) return;
        for (const auto& existing : paths_) {
            if (existing == dir) return;
        }
        paths_.push_back(dir);
    }

    /// Insert `dir` at the front so it is searched before everything else.
    /// If `dir` is already present elsewhere in the list it is moved to
    /// the front (no duplicates).
    void add_at_front(const std::filesystem::path& dir) {
        if (dir.empty()) return;
        for (auto it = paths_.begin(); it != paths_.end(); ++it) {
            if (*it == dir) {
                paths_.erase(it);
                break;
            }
        }
        paths_.insert(paths_.begin(), dir);
    }

    /// Remove all directories.
    void clear() noexcept { paths_.clear(); }

    /// Remove the directory at index `i`. Out-of-range index is a no-op.
    void remove(std::size_t i) {
        if (i < paths_.size()) {
            paths_.erase(paths_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    /// Read-only view of the directories in search order.
    const std::vector<std::filesystem::path>& paths() const noexcept {
        return paths_;
    }

    /// Search the path for `filename` and return the first match, or
    /// `nullopt` if no directory contains it. `filename` is appended as a
    /// relative path to each directory in order; the first hit that
    /// `std::filesystem::exists()` accepts wins.
    std::optional<std::filesystem::path> find(
        const std::filesystem::path& filename) const {
        if (filename.empty()) return std::nullopt;
        std::error_code ec;
        for (const auto& dir : paths_) {
            auto candidate = dir / filename;
            if (std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }
        return std::nullopt;
    }

    /// Search the path for `filename` and return ALL matches across every
    /// directory, in search order. Useful for overlays where later
    /// directories shadow earlier ones and the caller wants to see both.
    std::vector<std::filesystem::path> find_all(
        const std::filesystem::path& filename) const {
        std::vector<std::filesystem::path> hits;
        if (filename.empty()) return hits;
        std::error_code ec;
        for (const auto& dir : paths_) {
            auto candidate = dir / filename;
            if (std::filesystem::exists(candidate, ec)) {
                hits.push_back(candidate);
            }
        }
        return hits;
    }

    /// Serialize the search path to a PATH-style string using `separator()`.
    /// Round-trips through `from_string()`.
    std::string to_string() const {
        std::string out;
        const char sep = separator();
        bool first = true;
        for (const auto& p : paths_) {
            if (!first) out.push_back(sep);
            out += p.string();
            first = false;
        }
        return out;
    }

    /// Replace the contents of the search path with the entries parsed
    /// from `serialized`. Splits on the platform PATH separator; empty
    /// segments and duplicates are dropped. Existing entries are cleared
    /// first.
    void from_string(std::string_view serialized) {
        paths_.clear();
        const char sep = separator();
        std::size_t start = 0;
        while (start <= serialized.size()) {
            std::size_t end = serialized.find(sep, start);
            if (end == std::string_view::npos) end = serialized.size();
            if (end > start) {
                auto piece = serialized.substr(start, end - start);
                add(std::filesystem::path(std::string(piece)));
            }
            if (end == serialized.size()) break;
            start = end + 1;
        }
    }

    bool operator==(const FileSearchPath& other) const noexcept {
        return paths_ == other.paths_;
    }
    bool operator!=(const FileSearchPath& other) const noexcept {
        return !(*this == other);
    }

private:
    std::vector<std::filesystem::path> paths_;
};

}  // namespace pulp::runtime
