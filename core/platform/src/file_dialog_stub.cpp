// File dialog common + non-Apple implementation.
//
// The backend registration API (set_backend/clear_backend/has_backend)
// is compiled on every platform. macOS has a native built-in impl in
// file_dialog_mac.mm. Windows and Linux can install their built-in backends
// explicitly via FileDialog::install_native_backend(). iOS and Android have no
// built-in backend yet. On backend-routed platforms, open/save/folder dialog
// methods return explicit "no selection" until a host or opt-in native backend
// is installed, so the JS bridge can distinguish "user cancelled" from
// "platform unsupported".

#include <pulp/platform/file_dialog.hpp>

#include <mutex>

// On Linux a real built-in backend exists: the xdg-desktop-portal bridge
// (file_dialog_portal_linux.cpp). A host opts in by calling
// FileDialog::install_native_backend() at startup — we do NOT auto-install,
// because a portal call raises a real (blocking) dialog and unit tests /
// headless callers must keep the documented "no backend → no selection"
// contract. install_native_backend() references the portal factory, which
// also force-links that TU.
#if defined(__linux__) && !defined(__ANDROID__)
#include <pulp/platform/dbus.hpp>
#endif

// TargetConditionals provides TARGET_OS_OSX / TARGET_OS_IOS macros
// so has_backend() can narrow its unconditional-true to macOS only
// (Apple has no built-in file_dialog impl on iOS yet).
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pulp::platform {

#if defined(__linux__) && !defined(__ANDROID__)
// Defined in platform/linux/file_dialog_portal_linux.cpp. Referencing it here
// forces that TU to link (static-lib object files with only static
// initializers can otherwise be dropped).
FileDialog::Backend make_linux_portal_backend();
#elif defined(_WIN32)
// Defined in platform/win/file_dialog_win.cpp (IFileDialog COM backend).
FileDialog::Backend make_win_file_dialog_backend();
#endif

namespace {
    std::mutex           g_backend_mu;
    FileDialog::Backend  g_backend;
    bool                 g_backend_installed = false;
}

void FileDialog::set_backend(Backend backend) {
    std::lock_guard lock(g_backend_mu);
    g_backend = std::move(backend);
    g_backend_installed = true;
}

bool FileDialog::install_native_backend() {
    // Opt-in install of the platform's built-in backend. Linux: the
    // xdg-desktop-portal bridge, available only when libdbus is loadable.
    // Windows: the IFileDialog COM backend.
    // Idempotent — leaves an already-installed (incl. host-set) backend in
    // place. No-op elsewhere (macOS has a compiled-in native impl; iOS
    // and Android have no built-in backend yet).
#if defined(__linux__) && !defined(__ANDROID__)
    std::lock_guard lock(g_backend_mu);
    if (g_backend_installed) return true;
    if (!DBus::library_available()) return false;
    g_backend = make_linux_portal_backend();
    g_backend_installed = true;
    return true;
#elif defined(_WIN32)
    std::lock_guard lock(g_backend_mu);
    if (g_backend_installed) return true;   // leave a host-set backend in place
    g_backend = make_win_file_dialog_backend();
    g_backend_installed = true;
    return true;
#else
    return has_backend();
#endif
}

void FileDialog::clear_backend() {
    std::lock_guard lock(g_backend_mu);
    g_backend = {};
    g_backend_installed = false;
}

namespace detail {
// True if a host/explicitly-set Backend handled open_file (result in `out`);
// false means "use the platform-native dialog". The macOS impl consults this
// FIRST so the Backend seam (host overrides + headless test mocks) works there
// too — without it, file_dialog_mac.mm went straight to a blocking NSOpenPanel
// and ignored set_backend(). The compiled-in native panel remains the default
// when no explicit backend is installed.
bool file_dialog_open_file_via_backend(const std::string& title,
                                       const std::vector<FileFilter>& filters,
                                       const std::string& default_path,
                                       std::optional<std::string>& out) {
    std::lock_guard lock(g_backend_mu);
    if (!g_backend_installed || !g_backend.open_file) return false;
    out = g_backend.open_file(title, filters, default_path);
    return true;
}
}  // namespace detail

bool FileDialog::has_backend() {
    // Report "true" only on platforms where a real native dialog impl is
    // compiled in. macOS ships file_dialog_mac.mm; iOS does not have a
    // built-in file_dialog impl yet. Narrow the unconditional-true to macOS
    // only; iOS and everyone else reflect the host-registered state so callers
    // get honest "unsupported" signaling until a backend is installed.
#if defined(__APPLE__) && TARGET_OS_OSX
    return true;
#else
    std::lock_guard lock(g_backend_mu);
    return g_backend_installed;
#endif
}

// iOS has no built-in file_dialog backend (no `file_dialog_ios.mm` yet).
// Without these definitions the link step for any iOS bundle that pulls
// pulp-view-core fails on undefined symbols for FileDialog::{open_file,
// save_file, choose_folder}. Provide the same backend-routed fallback that the
// non-Apple stub uses so the symbols exist; until a backend is installed
// callers get an honest "no backend installed" nullopt instead of a link error.
#if !defined(__APPLE__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)

std::optional<std::string> FileDialog::open_file(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::string& default_path)
{
    std::lock_guard lock(g_backend_mu);
    if (!g_backend_installed || !g_backend.open_file) return std::nullopt;
    return g_backend.open_file(title, filters, default_path);
}

std::vector<std::string> FileDialog::open_files(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::string& default_path)
{
    std::lock_guard lock(g_backend_mu);
    if (!g_backend_installed || !g_backend.open_files) return {};
    return g_backend.open_files(title, filters, default_path);
}

std::optional<std::string> FileDialog::save_file(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::string& default_path,
    const std::string& default_name)
{
    std::lock_guard lock(g_backend_mu);
    if (!g_backend_installed || !g_backend.save_file) return std::nullopt;
    return g_backend.save_file(title, filters, default_path, default_name);
}

std::optional<std::string> FileDialog::choose_folder(
    const std::string& title,
    const std::string& default_path)
{
    std::lock_guard lock(g_backend_mu);
    if (!g_backend_installed || !g_backend.choose_folder) return std::nullopt;
    return g_backend.choose_folder(title, default_path);
}

#endif // !defined(__APPLE__) || iOS

} // namespace pulp::platform
