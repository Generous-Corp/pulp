#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>

#include <pulp/platform/dialog_session.hpp>
#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/native_handle.hpp>

namespace pulp::platform {

// File filter (e.g., {"Audio Files", "wav;flac;mp3"})
struct FileFilter {
    std::string description;
    std::string extensions; // Semicolon-separated: "wav;flac;mp3"
};

// Native file open/save dialogs.
//
// Platform notes:
//   - macOS: built-in NSOpenPanel/NSSavePanel dialogs.
//   - Windows: opt-in built-in IFileDialog backend via
//     FileDialog::install_native_backend().
//   - Linux: opt-in built-in xdg-desktop-portal FileChooser backend via
//     FileDialog::install_native_backend() when libdbus is loadable.
//   - iOS/Android: no built-in backend yet; hosts can register one via
//     FileDialog::set_backend().
// Without a native or host-registered backend, calls return std::nullopt / {}
// as explicit "no backend" rather than silent success.
class FileDialog {
public:
    // Show an open file dialog. Returns selected path or nullopt.
    static std::optional<std::string> open_file(
        const std::string& title = "Open",
        const std::vector<FileFilter>& filters = {},
        const std::string& default_path = "");

    // Show an open file dialog allowing multiple selection.
    static std::vector<std::string> open_files(
        const std::string& title = "Open",
        const std::vector<FileFilter>& filters = {},
        const std::string& default_path = "");

    // Show a save file dialog. Returns selected path or nullopt.
    static std::optional<std::string> save_file(
        const std::string& title = "Save",
        const std::vector<FileFilter>& filters = {},
        const std::string& default_path = "",
        const std::string& default_name = "");

    // Copy an existing file to a destination chosen by a native save dialog.
    // The copy occurs while the platform still owns the selection grant. This
    // distinction is load-bearing in sandboxed plugin hosts on macOS: returning
    // a pathname from NSSavePanel and opening it later loses the granted
    // capability and fails with EPERM. Returns the chosen destination on
    // success, nullopt on cancellation or failure. Native backends populate
    // `error` when their platform API distinguishes failure from cancellation;
    // an empty error means cancellation or an undifferentiated backend refusal.
    static std::optional<std::string> save_copy(
        const std::string& source_path,
        const std::string& title = "Save",
        const std::vector<FileFilter>& filters = {},
        const std::string& default_path = "",
        const std::string& default_name = "",
        std::string* error = nullptr);

    // Show a folder selection dialog.
    static std::optional<std::string> choose_folder(
        const std::string& title = "Choose Folder",
        const std::string& default_path = "");

    // Show a folder selection dialog that allows several folders. Returns the
    // chosen folders in the platform's order, or an empty vector on cancel or
    // when no backend is available (has_backend() tells the two apart). A
    // backend that only registered a single-folder handler yields at most one
    // folder rather than failing; see choose_folders_with().
    static std::vector<std::string> choose_folders(const std::string& title = "Choose Folders",
                                                   const std::string& default_path = "");

    // ── Host-registered backend ────────────────────────────────────────
    //
    // On platforms without an auto-available native dialog (Windows,
    // Linux, iOS, Android) the host app can install a backend that
    // implements real native dialogs. Windows and Linux ship opt-in
    // built-in backends through install_native_backend(); iOS and
    // Android remain host-provided for now. Without a backend installed,
    // each call returns no-selection and the JS bridge can probe
    // `has_backend()` to distinguish "user cancelled" from
    // "platform unsupported".

    struct Backend {
        std::function<std::optional<std::string>(
            const std::string& title,
            const std::vector<FileFilter>& filters,
            const std::string& default_path)> open_file;
        std::function<std::vector<std::string>(
            const std::string& title,
            const std::vector<FileFilter>& filters,
            const std::string& default_path)> open_files;
        std::function<std::optional<std::string>(
            const std::string& title,
            const std::vector<FileFilter>& filters,
            const std::string& default_path,
            const std::string& default_name)> save_file;
        std::function<std::optional<std::string>(
            const std::string& title,
            const std::string& default_path)> choose_folder;
        // Optional. When unset, choose_folders() falls back to choose_folder()
        // and returns at most one folder.
        std::function<std::vector<std::string>(const std::string& title,
                                               const std::string& default_path)>
            choose_folders;
    };

    // The routing rule behind choose_folders(), exposed so it can be checked
    // without raising a dialog: the backend's multi-folder handler when it has
    // one, else its single-folder handler wrapped into a vector, else empty.
    static std::vector<std::string> choose_folders_with(const Backend& backend,
                                                        const std::string& title,
                                                        const std::string& default_path);

    using SaveCopyBackend = std::function<std::optional<std::string>(
            const std::string& source_path,
            const std::string& title,
            const std::vector<FileFilter>& filters,
            const std::string& default_path,
            const std::string& default_name,
            std::string* error)>;

    static void set_backend(Backend backend);
    static void set_save_copy_backend(SaveCopyBackend backend);
    static bool has_save_copy_backend();
    static void clear_backend();
    static bool has_backend();

    // Install the platform's built-in backend, if one exists and no backend
    // is already installed. Hosts call this once at startup to opt into
    // native dialogs on platforms that ship a built-in bridge:
    //   - Linux: the xdg-desktop-portal FileChooser bridge — returns true
    //     when libdbus is loadable (the portal service is probed lazily, per
    //     call), false otherwise.
    //   - macOS: a native impl is already compiled in; returns has_backend().
    //   - Windows: Vista+ IFileDialog backend; returns true after installing.
    //   - iOS / Android: no built-in backend yet; returns has_backend()
    //     (false unless a host registered one).
    // Idempotent: an already-installed (incl. host-set) backend is left in
    // place. We deliberately do NOT auto-install — raising a portal dialog
    // blocks, so the default "no backend → no selection" contract must hold
    // for headless/test callers until a host opts in.
    static bool install_native_backend();

    // ── Asynchronous dialogs ───────────────────────────────────────────
    //
    // Non-blocking versions for editors, parented to the window that holds
    // `parent` (an NSView* on macOS): a sheet on that window, or a free
    // standing panel when the view is not in a window yet. Each returns at
    // once; `done` runs later, exactly once, on the main thread, unless the
    // MainThreadScope is destroyed first, which closes the panel and drops
    // `done`. Cancelling gives an empty result. Without a native or async
    // backend the empty result is delivered asynchronously too, except where
    // there is no main-thread backend at all (main_thread_supported() is
    // false): then `done` runs inline, before the call returns.

    struct OpenRequest {
        std::string title = "Open";
        std::vector<FileFilter> filters;
        std::string default_path;
        bool choose_files = true;
        bool choose_directories = false;
        bool multiple = false;
    };

    struct SaveRequest {
        std::string title = "Save";
        std::vector<FileFilter> filters;
        std::string default_path;
        std::string default_name;
    };

    static DialogSession open_async(MainThreadScope& scope, NativeViewHandle parent,
                                    OpenRequest request,
                                    std::function<void(std::vector<std::string>)> done);
    static DialogSession save_async(MainThreadScope& scope, NativeViewHandle parent,
                                    SaveRequest request,
                                    std::function<void(std::optional<std::string>)> done);
    // As save_copy(): the copy happens while the platform still holds the
    // selection grant. `error` is empty on success and on cancellation.
    static DialogSession save_copy_async(
        MainThreadScope& scope, NativeViewHandle parent, std::string source_path,
        SaveRequest request,
        std::function<void(std::optional<std::string> destination, std::string error)> done);

    // Host- or test-provided asynchronous dialogs. When installed they serve
    // the *_async calls on every platform, macOS included. Each handler must
    // call its completion once, from any thread; delivery to `done` is
    // always posted through the scope. `cancel` closes whatever is showing.
    struct AsyncBackend {
        std::function<void(const OpenRequest&, std::function<void(std::vector<std::string>)>)> open;
        std::function<void(const SaveRequest&, std::function<void(std::optional<std::string>)>)>
            save;
        std::function<void()> cancel;
    };
    static void set_async_backend(AsyncBackend backend);
    static void clear_async_backend();
    static bool has_async_backend();

    // The system content types the filters name, as uniform type identifiers
    // (macOS; dynamic identifiers for extensions the system does not know, so
    // none is dropped). Empty off macOS.
    static std::vector<std::string> content_type_ids_for(const std::vector<FileFilter>& filters);
};

} // namespace pulp::platform
