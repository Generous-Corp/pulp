// SPDX-License-Identifier: MIT
//
// pulp #1814 — version-pinned SDK tarball filename helpers used by
// `pulp install` / `pulp cache fetch skia`.
//
// Pulled out of cli_common.cpp into its own translation unit so the
// matching unit-test target can compile + link these two functions
// standalone (mirrors the version_diag / fetchcontent_cache pattern
// elsewhere in tools/cli/).

#include "sdk_cache_paths.hpp"

#include <string>

namespace pulp::cli {

std::string sdk_tarball_filename(const std::string& version,
                                  const std::string& platform) {
    // Pinning the version into the filename is the structural fix for
    // pulp #1814 — a cached v0.85 tarball cannot shadow a v0.92
    // download because the lookup misses on the filename, not on a
    // version field that has to be checked separately. Format:
    //   pulp-sdk-v0.92.0-darwin-arm64.tar.gz
    return "pulp-sdk-v" + version + "-" + platform + ".tar.gz";
}

std::string legacy_unversioned_sdk_tarball_filename(const std::string& platform) {
    // The pre-#1814 filename. Used only for best-effort cleanup of
    // stale tarballs from older CLI installs and for back-compat
    // unit tests; never produced by the post-#1814 code path.
    return "pulp-sdk-" + platform + ".tar.gz";
}

}  // namespace pulp::cli
