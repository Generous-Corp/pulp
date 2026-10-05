#pragma once

// Sparkle 2 auto-update hook for standalone apps.
//
// The SDK never links Sparkle. An app opts in at build time with
// `pulp_add_sparkle()` (tools/cmake/PulpSparkle.cmake), which embeds a pinned
// Sparkle.framework in the app's Contents/Frameworks, links it, and writes
// SUFeedURL / SUPublicEDKey into the app's Info.plist. At run time the
// standalone host looks for Sparkle's `SPUStandardUpdaterController` class in
// that embedded framework through the Objective-C runtime; finding it is what
// turns the "Check for Updates…" menu item on. A plug-in bundle never reaches
// this code: it lives in pulp-standalone, which only standalone executables
// link, and the probe ignores a Sparkle that the hosting process (a DAW) may
// have loaded from its own bundle.
//
// The standalone installs the chosen backend as the process-wide
// AppUpdateService (pulp/format/app_updates.hpp), which is what the app menu,
// the Settings panel's Updates tab and a JS editor's bridge messages read.

#include <pulp/format/app_updates.hpp>
#include <pulp/view/window_host.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace pulp::format::detail {

/// What the running process looks like to the updater. Filled in by
/// `probe_standalone_updater_environment()` on macOS; a plain value so the
/// policy below is testable without AppKit.
struct StandaloneUpdaterEnvironment {
    /// The main bundle's Info.plist names a non-empty SUFeedURL.
    bool bundle_declares_feed = false;
    /// SPUStandardUpdaterController resolves, from a Sparkle.framework that
    /// lives inside this app's own bundle.
    bool updater_framework_loaded = false;
    /// The standalone runs without a window (screenshots, CI, probes).
    bool headless = false;
    /// The main executable carries a Team ID (Developer ID / notarized
    /// distribution build). Development builds are ad-hoc signed.
    bool developer_id_signed = false;
    /// Value of PULP_STANDALONE_UPDATER: "" (default policy), "0"/"off"
    /// (disable entirely), "1"/"on" (start scheduled checks even in an
    /// ad-hoc build, for practising against a local feed), "stub" (offer the
    /// menu item and Settings controls backed by a stub that never contacts
    /// a feed -- for exercising the wiring in a build without Sparkle).
    std::string override_value;
};

enum class StandaloneUpdaterBackend : std::uint8_t { none, sparkle, stub };

struct StandaloneUpdaterPlan {
    /// Add "Check for Updates…" to the application menu.
    bool offer_menu_command = false;
    /// Start Sparkle at launch so its scheduled background checks run.
    /// When false the updater still starts on the first menu click.
    bool start_at_launch = false;
    /// What backs the menu item and the AppUpdateService.
    StandaloneUpdaterBackend backend = StandaloneUpdaterBackend::none;
};

/// The policy. Automatic checks start only in a Developer-ID-signed build, so
/// a development or CI launch of an app that embeds Sparkle never prompts for
/// permission or reaches the network on its own; the menu item still works
/// there because clicking it is an explicit request.
StandaloneUpdaterPlan plan_standalone_updater(const StandaloneUpdaterEnvironment& env);

/// Inspect the running process. Returns an all-false environment off macOS.
StandaloneUpdaterEnvironment probe_standalone_updater_environment(bool headless);

/// Create Sparkle's standard updater controller once (idempotent) and start
/// it. No-op when Sparkle is not embedded.
void start_standalone_updater();

/// Start the updater if needed and run a user-initiated check, which shows
/// Sparkle's own progress / "up to date" / update dialogs.
void check_for_standalone_updates();

/// The AppUpdateService for the plan's backend, or nullptr for `none`.
/// Sparkle's service reads the app's Info.plist (version, SUFeedURL,
/// PulpUpdatesReleasesURL, PulpUpdatesInstaller, SUAllowsAutomaticUpdates)
/// and Sparkle's own persisted settings.
std::shared_ptr<AppUpdateService> make_standalone_update_service(const StandaloneUpdaterPlan& plan);

/// A service that never contacts a feed: a check only records its time and
/// the automatic-check preference lives in memory. Portable; used for the
/// "stub" backend and by tests.
std::shared_ptr<AppUpdateService> make_stub_update_service(std::string app_name,
                                                           std::string version);

/// Add "Check for Updates…" to the application menu, directly under "About
/// <App>", when the plan offers it. The command calls check_for_app_updates().
void add_standalone_updater_menu_command(view::WindowOptions& options,
                                         const StandaloneUpdaterPlan& plan);

} // namespace pulp::format::detail
