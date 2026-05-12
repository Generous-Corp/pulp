// SPDX-License-Identifier: MIT
//
// pulp #1814 — version-pinned SDK tarball filename helpers used by
// `pulp install` / `pulp cache fetch skia`.
//
// Lives in a standalone header (free of pulp::runtime / pulp::platform
// deps) so the unit test can compile this surface without dragging the
// whole CLI link-graph in (mirrors the version_diag / fetchcontent_cache
// pattern in tools/cli/).

#pragma once

#include <string>

namespace pulp::cli {

/// Versioned SDK tarball filename for the `~/.pulp/cache/` download cache.
/// Returns `pulp-sdk-v<version>-<platform>.tar.gz`.
///
/// Pinning the version into the filename is the structural fix for the
/// stale-cache bug: a cached tarball from a prior release cannot
/// silently shadow a fresh download because the filename lookup misses,
/// not a version field that has to be checked separately. See
/// https://github.com/danielraffel/pulp/issues/1814.
std::string sdk_tarball_filename(const std::string& version,
                                  const std::string& platform);

/// Pre-#1814 unversioned filename. Used only for best-effort cleanup of
/// stale tarballs from older CLI installs and for back-compat unit
/// tests; never produced by the post-#1814 code path.
std::string legacy_unversioned_sdk_tarball_filename(const std::string& platform);

}  // namespace pulp::cli
