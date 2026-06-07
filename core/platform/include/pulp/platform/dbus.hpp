#pragma once

// Minimal session-bus D-Bus client over libdbus-1, loaded at runtime (no
// build-time libdbus-dev dependency, mirroring the libudev approach). Linux is
// the only real backend; on other platforms every call is an honest no-op
// (library_available() / connect_session() return false). Lives in
// core/platform — the lowest layer — so any subsystem can use it for
// session-bus IPC (xdg-desktop-portal file chooser / screenshot / notifications,
// etc.) without a build-time dependency or a layering cycle.

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulp::platform {

class DBus {
public:
    DBus();
    ~DBus();

    /// True iff libdbus-1 can be loaded right now (Linux dlopen probe). Always
    /// false off Linux. Cheap; does not connect.
    static bool library_available();

    /// Connect to the session bus. Returns false off Linux, without libdbus, or
    /// when no session bus is reachable. Idempotent (returns true if already
    /// connected).
    bool connect_session();
    bool connected() const;

    /// Reply value for an xdg-desktop-portal request: the response code
    /// (0 = success, 1 = user cancelled, 2 = other) plus any returned URIs.
    struct PortalResult {
        int response = 2;                  // portal "response" code
        std::vector<std::string> uris;     // results["uris"] (file:// URIs)
    };

    /// Invoke org.freedesktop.portal.FileChooser.<method> ("OpenFile" or
    /// "SaveFile") and block until the portal's Response signal arrives (or
    /// `timeout_ms` elapses). `options` are string→string entries marshalled as
    /// the portal's a{sv} with string variants; `bool_options` are marshalled as
    /// boolean variants (e.g. {"multiple", true} / {"directory", true}).
    /// Returns std::nullopt on any transport error (no portal, disconnect, …).
    std::optional<PortalResult> file_chooser(
        const std::string& method,
        const std::string& title,
        const std::map<std::string, std::string>& options,
        const std::map<std::string, bool>& bool_options,
        int timeout_ms = 120000);

    DBus(const DBus&) = delete;
    DBus& operator=(const DBus&) = delete;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

/// Convert a file:// URI to a local filesystem path (percent-decoded). Returns
/// the input unchanged if it has no file:// scheme. Pure + testable everywhere.
std::string file_uri_to_path(const std::string& uri);

}  // namespace pulp::platform
