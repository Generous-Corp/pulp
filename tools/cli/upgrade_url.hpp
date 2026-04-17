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

#include <string>

namespace pulp::cli {

struct UpgradeUrlPair {
    std::string asset;
    std::string url;
};

inline UpgradeUrlPair pulp_upgrade_url_for(const std::string& version,
                                           const std::string& platform,
                                           const std::string& arch) {
    const std::string ext = (platform == "windows") ? "zip" : "tar.gz";
    const std::string asset = "pulp-" + platform + "-" + arch + "." + ext;
    const std::string url =
        "https://github.com/danielraffel/pulp/releases/download/v"
        + version + "/" + asset;
    return {asset, url};
}

}  // namespace pulp::cli
