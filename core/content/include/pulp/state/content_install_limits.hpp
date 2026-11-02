#pragma once

/// @file content_install_limits.hpp
/// Decompressed-size budget a content-pack archive must fit before anything in
/// it is inflated. A `.pulpcontent` archive declares each entry's uncompressed
/// size in its central directory, so the whole pack can be admitted or refused
/// from metadata alone, before a single byte lands on the heap or on disk.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::state {

/// Caps on what an archive may inflate to. Callers that stage packs into a
/// constrained location override the defaults; the CLI exposes the total as
/// `--max-pack-bytes`.
struct ContentInstallLimits {
    std::uint64_t max_total_bytes = std::uint64_t{4} << 30; ///< Sum of every entry's declared size.
    std::uint64_t max_entry_bytes = std::uint64_t{1} << 30; ///< Any single entry's declared size.
    /// Number of file entries. The hash check cross-references every archived
    /// payload against the declared list, so this cap bounds that work as the
    /// byte caps bound inflation.
    std::uint64_t max_entries = 65536;
};

/// Lower both byte caps to at most `max_bytes` (a user-facing `--max-pack-bytes`
/// value). A value above a default never raises that default: the flag only
/// tightens. The entry-count cap is untouched.
inline ContentInstallLimits clamp_content_install_bytes(ContentInstallLimits limits,
                                                        std::uint64_t max_bytes) {
    limits.max_total_bytes = std::min(limits.max_total_bytes, max_bytes);
    limits.max_entry_bytes = std::min(limits.max_entry_bytes, max_bytes);
    return limits;
}

/// Running admission of archive entries against a ContentInstallLimits. The
/// runtime installer and the CLI share it so both refuse the same pack with the
/// same message, naming the entry that crossed the line.
class ContentSizeBudget {
  public:
    explicit ContentSizeBudget(ContentInstallLimits limits = {}) : limits_(limits) {}

    /// Admit `bytes` for `entry_name`. Returns an empty string when the entry
    /// fits, otherwise an issue naming the entry and the cap it exceeded. A
    /// refused entry is not counted, so the caller can stop at the first one.
    std::string admit(std::string_view entry_name, std::uint64_t bytes) {
        if (entries_ >= limits_.max_entries) {
            return "archive-size: entry `" + std::string(entry_name) + "` is entry " +
                   std::to_string(entries_ + 1) + ", above the entry-count cap of " +
                   std::to_string(limits_.max_entries) + " entries";
        }
        if (bytes > limits_.max_entry_bytes) {
            return "archive-size: entry `" + std::string(entry_name) + "` declares " +
                   std::to_string(bytes) + " bytes, above the per-entry cap of " +
                   std::to_string(limits_.max_entry_bytes) + " bytes";
        }
        if (bytes > limits_.max_total_bytes - total_) {
            return "archive-size: entry `" + std::string(entry_name) +
                   "` would bring the pack to " + std::to_string(total_ + bytes) +
                   " bytes, above the total cap of " + std::to_string(limits_.max_total_bytes) +
                   " bytes";
        }
        total_ += bytes;
        ++entries_;
        return {};
    }

    std::uint64_t admitted_bytes() const {
        return total_;
    }
    std::uint64_t admitted_entries() const {
        return entries_;
    }
    const ContentInstallLimits& limits() const {
        return limits_;
    }

  private:
    ContentInstallLimits limits_;
    std::uint64_t total_ = 0;
    std::uint64_t entries_ = 0;
};

} // namespace pulp::state
