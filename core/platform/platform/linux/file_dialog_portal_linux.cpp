// Linux file dialogs via xdg-desktop-portal.
//
// The desktop portal (org.freedesktop.portal.Desktop, FileChooser
// interface) is the toolkit-agnostic way to raise a native file picker on
// modern Linux desktops (GNOME/KDE/sandboxed Flatpak alike). We talk to it
// over the session bus using the runtime-dlopen `pulp::platform::DBus`
// client — so there is NO build-time libdbus-dev dependency, mirroring the
// libudev approach. When libdbus or the portal service is unavailable every
// call honest-fails (nullopt / {}) so the JS bridge can distinguish "user
// cancelled" from "platform unsupported".
//
// Supported operations: open one/many files, save with a suggested name, and
// choose a folder. File-type filters and a pre-selected folder are not wired
// through this minimal DBus client yet; the dialog still works without them
// and shows all files / the portal's default folder.
//
// This is a real built-in backend (like file_dialog_mac.mm), not a
// host-registered one: make_linux_portal_backend() is referenced from
// file_dialog_stub.cpp's lazy installer, which both forces this TU to link
// (no static-init-stripping) and registers the backend on first use when
// libdbus is loadable. A host can still override via FileDialog::set_backend.

#include <pulp/platform/dbus.hpp>
#include <pulp/platform/file_dialog.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulp::platform {

namespace {

// Run a FileChooser request and return the selected local paths (empty on any
// failure / cancel). `method` is "OpenFile" or "SaveFile".
std::vector<std::string> run_chooser(
    const std::string& method,
    const std::string& title,
    const std::map<std::string, std::string>& str_options,
    const std::map<std::string, bool>& bool_options) {
    DBus bus;
    auto reply = bus.file_chooser(method, title, str_options, bool_options);
    if (!reply || reply->response != 0) return {};  // no portal / cancelled
    std::vector<std::string> paths;
    paths.reserve(reply->uris.size());
    for (const auto& uri : reply->uris) paths.push_back(file_uri_to_path(uri));
    return paths;
}

}  // namespace

// Build the portal-backed FileDialog backend. Pure factory — no global state,
// no locking — so the stub can call it while holding its own backend mutex.
FileDialog::Backend make_linux_portal_backend() {
    FileDialog::Backend backend;

    backend.open_file = [](const std::string& title,
                           const std::vector<FileFilter>&,
                           const std::string&) -> std::optional<std::string> {
        auto paths = run_chooser("OpenFile", title, {}, {});
        if (paths.empty()) return std::nullopt;
        return paths.front();
    };

    backend.open_files = [](const std::string& title,
                            const std::vector<FileFilter>&,
                            const std::string&) -> std::vector<std::string> {
        return run_chooser("OpenFile", title, {}, {{"multiple", true}});
    };

    backend.save_file = [](const std::string& title,
                           const std::vector<FileFilter>&,
                           const std::string&,
                           const std::string& default_name)
        -> std::optional<std::string> {
        std::map<std::string, std::string> opts;
        if (!default_name.empty()) opts["current_name"] = default_name;
        auto paths = run_chooser("SaveFile", title, opts, {});
        if (paths.empty()) return std::nullopt;
        return paths.front();
    };

    backend.choose_folder = [](const std::string& title,
                               const std::string&) -> std::optional<std::string> {
        auto paths = run_chooser("OpenFile", title, {}, {{"directory", true}});
        if (paths.empty()) return std::nullopt;
        return paths.front();
    };

    return backend;
}

}  // namespace pulp::platform
