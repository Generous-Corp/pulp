#pragma once

// In-app software updates, as a service the hosting process may provide.
//
// A standalone app that embeds an updater (Sparkle, via pulp_add_sparkle())
// installs an AppUpdateService at launch. Everything else -- an AU, VST3,
// CLAP or AAX plug-in inside a DAW, a test binary, a build without an
// updater -- has none installed, and every query below then reports
// `available == false`. That is the contract an editor relies on: the
// editor code is shared by every format, so it asks app_update_status() and
// shows its "Check for Updates" controls only when the answer is available.
//
// Surfaces built on this service:
//   * the standalone app menu's "Check for Updates…" item (standalone.cpp),
//   * AppUpdatesSettingsView, a drop-in Settings group for native editors
//     (pulp/format/app_updates_settings_view.hpp),
//   * add_app_update_handlers(), the EditorBridge messages a JS editor calls
//     (pulp/format/app_updates_bridge.hpp; client in @pulp/react).
//
// Extension point (not built): a plug-in may later install its own service
// that never runs an installer in the host's process -- for example one that
// reads the same appcast, reports a newer version, and whose
// check_for_updates() opens `releases_url` or asks the standalone app to
// update. The status shape already carries what such a notice needs.

#include <cstdint>
#include <memory>
#include <string>

namespace pulp::format {

/// How an update is installed, as the app declares it (pulp_add_sparkle
/// INSTALLER). Decides what the Settings note may truthfully say.
enum class AppUpdateInstaller : std::uint8_t {
    unknown,    ///< Not declared; the note says nothing about installing.
    package,    ///< A signed .pkg: quits the app, asks for an admin password.
    app_bundle, ///< A replacement .app: quits and reopens the app.
};

struct AppUpdateStatus {
    /// This process can check for, and install, updates. False in plug-ins.
    bool available = false;
    /// A user-initiated check may start now (false while one is running).
    bool can_check_now = false;
    /// Scheduled background checks are on. The user's choice, persisted by
    /// the backend (Sparkle keeps it in the app's user defaults).
    bool automatic_checks = false;
    /// The backend may download AND install an update without asking.
    bool automatic_install = false;
    /// The backend is a development stub that never contacts a feed.
    bool stub = false;
    std::string app_name;
    /// User-facing version (CFBundleShortVersionString) and build
    /// (CFBundleVersion, the number the feed is compared against).
    std::string version;
    std::string build;
    /// Unix seconds of the last completed check; 0 = never checked.
    std::int64_t last_check_unix_seconds = 0;
    /// Web page that lists releases (pulp_add_sparkle RELEASES_URL); may be empty.
    std::string releases_url;
    /// Host of the update feed, e.g. "github.com"; may be empty.
    std::string feed_host;
    AppUpdateInstaller installer = AppUpdateInstaller::unknown;
};

class AppUpdateService {
  public:
    virtual ~AppUpdateService() = default;
    [[nodiscard]] virtual AppUpdateStatus status() const = 0;
    /// Start a user-initiated check; the backend shows its own progress,
    /// "up to date" and update dialogs. Returns false if it could not start.
    virtual bool check_for_updates() = 0;
    /// Turn scheduled checks on or off. Returns false if the backend refused.
    virtual bool set_automatic_checks(bool on) = 0;
    /// Open `status().releases_url` in the user's browser. Returns false when
    /// there is none or the backend cannot open pages.
    virtual bool open_releases_page() {
        return false;
    }
};

/// Install (or, with nullptr, remove) the process-wide service. The
/// standalone host calls this; plug-ins never do.
void set_app_update_service(std::shared_ptr<AppUpdateService> service);
[[nodiscard]] std::shared_ptr<AppUpdateService> app_update_service();

/// Convenience wrappers over the installed service. With none installed:
/// status().available is false and the two actions return false.
[[nodiscard]] AppUpdateStatus app_update_status();
bool check_for_app_updates();
bool set_app_update_automatic_checks(bool on);
bool open_app_releases_page();

/// The explanatory note shown under the Settings controls, built only from
/// facts in `status`, so it stays true for each app. Empty when unavailable.
/// e.g. "Updates download from Spectr's GitHub releases. Installing an
/// update quits and reopens Spectr, briefly stopping its audio, and asks for
/// an administrator password. Updates are never installed automatically."
[[nodiscard]] std::string app_update_note(const AppUpdateStatus& status);

/// "Version 1.0.7 (1.0.7.2)" -- the build is shown only when it differs.
[[nodiscard]] std::string app_update_version_text(const AppUpdateStatus& status);

/// "Last checked: never" or "Last checked: 2026-10-04 14:03" (local time).
[[nodiscard]] std::string app_update_last_check_text(const AppUpdateStatus& status);

/// The status as a JSON object, the shape the JS bridge returns:
/// {available, canCheckNow, automaticChecks, automaticInstall, stub,
///  appName, version, build, lastCheckUnixSeconds, releasesUrl, feedHost,
///  installer: "package"|"app"|"unknown", note, versionText, lastCheckText}
[[nodiscard]] std::string app_update_status_json(const AppUpdateStatus& status);

} // namespace pulp::format
