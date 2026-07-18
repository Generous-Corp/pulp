// SPDX-License-Identifier: MIT
//
// cli_fs_util.hpp — filesystem-safety helpers shared by the CLI's
// archive-handling commands (kit, package, content).
//
// These are the containment checks that stand between an untrusted archive
// entry and the filesystem, so they live in one place with a pinned contract
// rather than being re-derived per command file.
#pragma once

#include <filesystem>

namespace pulp::cli::fsutil {

namespace fs = std::filesystem;

// True when `path` is lexically at or below `root`, compared component by
// component. Both arguments are `lexically_normal`-ed first; neither is made
// absolute, so this answers a purely lexical question and the caller owns
// resolution.
//
// The contract this implies, and which callers depend on:
//   - The caller MUST pass both arguments in the same frame of reference —
//     both absolute, or both relative to the same base. A relative path and an
//     absolute path never share a prefix, so a mixed pair always returns false.
//   - Because normalization is lexical, a symlink inside `root` that points
//     outside it still reads as "within". A caller guarding extraction against
//     a symlinked escape must resolve (`weakly_canonical`) or reject symlinks
//     itself before calling.
//   - `root` at or below itself is within (an empty `root` matches anything).
bool path_is_within(const fs::path& path, const fs::path& root);

// True when `rel` is safe to join onto an extraction root: non-empty,
// relative, and free of `.` / `..` / empty components. Rejects the traversal
// and absolute-path forms an archive entry name can smuggle in.
//
// This is a name-shape check only — see `path_is_within` for the containment
// check callers pair it with once the entry is joined onto a real root.
bool safe_archive_rel(const fs::path& rel);

// True when `path` names one of the CLI's package archive containers.
bool is_package_archive_path(const fs::path& path);

// A fresh, process-unique directory under the system temp dir for unpacking an
// archive into. Unique per call; the caller owns cleanup.
fs::path temporary_archive_root();

}  // namespace pulp::cli::fsutil
