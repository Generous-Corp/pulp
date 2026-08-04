#pragma once

// What is installed on this machine, and which copy of it is live.
//
// Forge Modular runs a Python generator that exists in up to four places at
// once: an explicit override, the copy under Application Support, the copy
// sealed inside the application bundle, and a developer's checkout. Picking
// between them by "whichever is in Application Support" made the installer
// unable to update what it installs -- a toolchain written by an older release
// shadowed every fix a newer one shipped, and because the shadowed script
// merely printed its usage and exited 2, the app reported nothing at all.
//
// So the choice is made from a VERSION STAMP written at package time, and the
// facts behind it are gathered into one report the user can read, because the
// live toolchain path is the single thing that would have made that failure
// obvious in seconds.

#include <cstddef>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace forge_modular {

// ── version stamps ───────────────────────────────────────────────────────────

/// Order two stamps: negative when `a` is older, 0 when equal, positive when
/// newer. Dotted integers, compared field by field, missing fields reading as
/// zero. An empty or unparseable stamp is the OLDEST thing there is -- that is
/// what makes "no stamp cannot outrank a release" fall out rather than being a
/// special case bolted on.
int compare_stamps(const std::string& a, const std::string& b);

/// Where a toolchain records which release laid it down.
///
/// A file rather than an mtime: a copy rewrites mtimes, and every path this
/// code takes is a copy. The first line is the version; the second, when
/// present, is when it was packaged.
std::string stamp_file_name();

/// Parse a stamp file's contents into (version, packaged-at). Either may be
/// empty.
std::pair<std::string, std::string> parse_stamp(const std::string& text);

// ── choosing a toolchain ─────────────────────────────────────────────────────

/// One place the generator might be.
struct ToolchainCandidate {
    std::string path;
    bool usable = false;    ///< patch.py is actually there
    std::string stamp;      ///< the version that laid it down, or ""
};

/// Which one runs, and why -- the "why" is user-facing, on the details row.
struct ToolchainPick {
    std::string path;
    std::string stamp;
    std::string reason;
};

/// Standing order: an explicit override, then the newer of the installed copy
/// and the bundle's own, then a checkout.
///
/// The bundle wins ONLY when it is strictly newer. Equal stamps leave the
/// installed copy in charge, which is what keeps hand-editing that directory
/// working for development: editing a file does not change the stamp beside it.
ToolchainPick choose_toolchain(const ToolchainCandidate& override_dir,
                               const ToolchainCandidate& installed,
                               const ToolchainCandidate& bundled,
                               const ToolchainCandidate& checkout);

// ── the details report ───────────────────────────────────────────────────────

/// Everything needed to answer "which build is this, and what is it running".
struct AppDetails {
    std::string app_version;        ///< CFBundleShortVersionString
    std::string packaged_at;        ///< when the toolchain stamp was written
    std::string toolchain_path;     ///< THE LIVE ONE. The field that matters.
    std::string toolchain_stamp;
    std::string toolchain_reason;
    std::size_t index_plugins = 0;
    std::size_t index_modules = 0;
    std::time_t index_written = 0;  ///< 0 when there is no index
    std::string sdk_path;           ///< "" when the Rack SDK is not here
    bool signed_in = false;         ///< a VCV library token was found
};

/// "4 days ago", "just now", "never built". Takes `now` so a test can age a
/// file without waiting four days for it.
std::string describe_age(std::time_t written, std::time_t now);

/// The report, as label/value pairs in the order they should be read. Pure, so
/// the wording is asserted without a window.
std::vector<std::pair<std::string, std::string>> details_rows(
    const AppDetails& d, std::time_t now);

/// The same report as one block of text, for the clipboard. A Label cannot be
/// selected with the mouse, so copying it whole is the affordance that makes
/// the report reportable.
std::string details_text(const AppDetails& d, std::time_t now);

}  // namespace forge_modular
