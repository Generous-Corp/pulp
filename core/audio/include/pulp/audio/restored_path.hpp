#pragma once

/// @file restored_path.hpp
/// Confinement for file paths that arrive through restored state. A host
/// session or preset is authored by whoever saved it, so a path inside it is
/// untrusted input: restoring it verbatim would make the plugin open any file
/// the author named. State blobs should carry content or root-relative
/// references, never absolute paths; this helper is the gate for the legacy
/// blobs that still do.

#include <pulp/runtime/safe_relative_path.hpp>

#include <filesystem>
#include <optional>
#include <system_error>

namespace pulp::audio {

/// Resolve a path restored from state against the directory the plugin is
/// allowed to read from. Symlinks along the existing prefix are followed, so a
/// link that points out of `root` is refused like any other escape. A relative
/// `path` is taken relative to `root`; an absolute one must already lie inside
/// it. Returns the canonical path to read, or nullopt when it would leave
/// `root`, when either argument is empty, or when `root` cannot be resolved.
inline std::optional<std::filesystem::path>
confine_restored_path(const std::filesystem::path& path, const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    if (path.empty() || root.empty())
        return std::nullopt;
    std::error_code ec;
    const fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec || canonical_root.empty())
        return std::nullopt;
    const fs::path joined = path.is_absolute() ? path : canonical_root / path;
    const fs::path canonical = fs::weakly_canonical(joined, ec);
    if (ec || canonical.empty())
        return std::nullopt;
    if (!pulp::runtime::is_within_directory(canonical_root, canonical))
        return std::nullopt;
    return canonical;
}

} // namespace pulp::audio
