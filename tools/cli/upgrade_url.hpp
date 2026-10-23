// upgrade_url.hpp — build the `pulp upgrade` release-asset name + URL
// for a given version + target triple.
//
// Header-only so tests can pin the URL shape without pulling cmd_misc's
// transitive CLI deps. The convention must stay in sync with
// .github/workflows/release-cli.yml — the #352 regression was that the
// CLI baked the version into the filename and every upgrade 404'd on
// the download step.
//
// Convention:
//   Asset  = "pulp-<platform>-<arch>.<ext>"  (no version in the filename)
//   URL    = ".../releases/download/v<version>/<asset>"
//   ext    = "zip" for windows, "tar.gz" otherwise
//   arch   = "arm64" | "x64"  (NOT "x86_64" — release assets use x64)

#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace pulp::cli {

// True when `version` is exactly the `M.m.p` a release tag carries: three
// dot-separated runs of digits and nothing else. Anything looser (a leading
// `v`, a pre-release suffix, whitespace, shell metacharacters) is refused
// before the version reaches a URL, a path, or a shell command.
inline bool pulp_release_version_is_well_formed(std::string_view version) {
    int components = 0;
    std::size_t run = 0;
    for (unsigned char c : version) {
        if (std::isdigit(c)) {
            ++run;
        } else if (c == '.' && run > 0) {
            ++components;
            run = 0;
        } else {
            return false;
        }
    }
    return run > 0 && components == 2;
}

struct UpgradeUrlPair {
    std::string asset;
    std::string url;
};

// Download URL of any asset on the release tagged `v<version>`; the
// SHA256SUMS manifest and the SDK archives share this shape.
inline std::string pulp_release_asset_url(const std::string& version, const std::string& asset) {
    return "https://github.com/Generous-Corp/pulp/releases/download/v" + version + "/" + asset;
}

inline UpgradeUrlPair pulp_upgrade_url_for(const std::string& version,
                                           const std::string& platform,
                                           const std::string& arch) {
    const std::string ext = (platform == "windows") ? "zip" : "tar.gz";
    const std::string asset = "pulp-" + platform + "-" + arch + "." + ext;
    return {asset, pulp_release_asset_url(version, asset)};
}

}  // namespace pulp::cli
