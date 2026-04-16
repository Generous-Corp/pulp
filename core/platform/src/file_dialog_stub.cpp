// File dialog common + non-Apple implementation.
//
// The backend registration API (set_backend/clear_backend/has_backend)
// is compiled on every platform — on mac/iOS it's a no-op since those
// platforms have a native built-in impl in file_dialog_mac.mm / the
// iOS UIDocumentPicker backend. On non-Apple platforms (Windows,
// Linux, Android) the open/save/folder dialog methods route through
// the registered backend; without one every call returns an explicit
// "no selection" (#301 P1 — replaces the old silent-nullopt stubs
// so the JS bridge can distinguish "user cancelled" from "platform
// unsupported").

#include <pulp/platform/file_dialog.hpp>

#include <mutex>

// TargetConditionals provides TARGET_OS_OSX / TARGET_OS_IOS macros
// so has_backend() can narrow its unconditional-true to macOS only
// (Apple has no built-in file_dialog impl on iOS yet — #316 P2).
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pulp::platform {

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

void FileDialog::clear_backend() {
    std::lock_guard lock(g_backend_mu);
    g_backend = {};
    g_backend_installed = false;
}

bool FileDialog::has_backend() {
    // #312 + #316 Codex P2s: report "true" only on platforms where a
    // real native dialog impl is compiled in. macOS ships
    // file_dialog_mac.mm; iOS does NOT have a built-in file_dialog
    // impl yet (UIDocumentPicker wiring is a follow-up). Narrow the
    // unconditional-true to macOS only; iOS and everyone else
    // reflect the host-registered state so callers get honest
    // "unsupported" signaling until the iOS impl lands.
#if defined(__APPLE__) && TARGET_OS_OSX
    return true;
#else
    std::lock_guard lock(g_backend_mu);
    return g_backend_installed;
#endif
}

#if !defined(__APPLE__)

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

#endif // !defined(__APPLE__)

} // namespace pulp::platform
