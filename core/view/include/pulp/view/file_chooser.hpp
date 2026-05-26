#pragma once

/// @file file_chooser.hpp
/// @brief Pulp-shaped builder for native open/save/folder dialogs.
///
/// macOS plan item 6.2 (planning/2026-05-24-macos-plugin-authoring-plan.md):
/// wraps `pulp::platform::FileDialog::open_file / open_files / save_file /
/// choose_folder` in a builder-style API that returns selected paths through
/// a `std::function` callback. The underlying native call is synchronous on
/// macOS (NSOpenPanel runs modally on the main thread); the callback surface
/// lets editor code register completion logic without blocking the calling
/// site on a `std::optional<std::string>` return value, and gives us a single
/// extension point if a real async (non-modal) backend lands later.
///
/// Filter strings follow the existing `pulp::platform::FileFilter` shape:
/// description plus a semicolon-separated extension list (e.g.
/// `"wav;flac;mp3"`). Helpers `add_filter()` / `add_extension_filter()` make
/// the common cases ergonomic while still routing through the same struct
/// the platform layer expects.

#include <pulp/platform/file_dialog.hpp>

#include <functional>
#include <string>
#include <vector>

namespace pulp::view {

/// Builder-style file chooser.
///
/// Usage:
/// @code
///   FileChooser chooser;
///   chooser.set_title("Load preset")
///          .set_initial_directory("/Users/me/Presets")
///          .add_extension_filter("Pulp preset", "pulp")
///          .add_extension_filter("All audio",  "wav;flac;aiff");
///   chooser.open([](const std::vector<std::string>& paths) {
///       if (!paths.empty()) load_preset(paths.front());
///   });
/// @endcode
///
/// `open()` calls the open-file dialog (single-selection unless
/// `set_allow_multiple(true)`). `save()` calls the save dialog. The chooser
/// can be reused across multiple calls; per-call results never mutate the
/// builder state.
class FileChooser {
public:
    /// Callback signature shared by every dialog kind.
    /// - `open()` yields 0 or 1 paths (or N when allow_multiple is true).
    /// - `save()` yields 0 or 1 paths.
    /// - `choose_folder()` yields 0 or 1 paths.
    /// An empty vector means the user cancelled or no backend was installed.
    using ResultCallback = std::function<void(std::vector<std::string>)>;

    FileChooser() = default;

    /// Dialog window title. Defaults differ per dialog kind (Open / Save /
    /// Choose Folder); set explicitly when you want a custom prompt.
    FileChooser& set_title(std::string title);
    const std::string& title() const { return title_; }

    /// Initial directory the dialog opens to. Empty == platform default.
    FileChooser& set_initial_directory(std::string path);
    const std::string& initial_directory() const { return initial_dir_; }

    /// Default filename suggested in save dialogs. Ignored by `open()` /
    /// `choose_folder()`. Empty == platform default.
    FileChooser& set_default_name(std::string name);
    const std::string& default_name() const { return default_name_; }

    /// Append a fully-formed `FileFilter`. See
    /// `pulp::platform::FileFilter` for the description + semicolon-extension
    /// contract.
    FileChooser& add_filter(pulp::platform::FileFilter filter);

    /// Convenience wrapper: append a description + semicolon-separated
    /// extension list. Equivalent to `add_filter({desc, exts})`.
    FileChooser& add_extension_filter(std::string description,
                                      std::string extensions);

    /// Clear all filters. Returns `*this` so the call chain still flows.
    FileChooser& clear_filters();

    /// Read-only view of accumulated filters (preserves insertion order).
    const std::vector<pulp::platform::FileFilter>& filters() const {
        return filters_;
    }

    /// When true, `open()` invokes the platform multi-file dialog and the
    /// callback may receive more than one path. Defaults to false.
    FileChooser& set_allow_multiple(bool allow);
    bool allow_multiple() const { return allow_multiple_; }

    /// Show an open dialog. Runs the callback exactly once with the
    /// selected paths (empty on cancel / no-backend). Callback may be null
    /// — the dialog still runs, but the result is discarded.
    void open(ResultCallback callback) const;

    /// Show a save dialog. Runs the callback exactly once.
    void save(ResultCallback callback) const;

    /// Show a folder-chooser dialog. Filters are ignored. Runs the
    /// callback exactly once.
    void choose_folder(ResultCallback callback) const;

private:
    std::string title_;
    std::string initial_dir_;
    std::string default_name_;
    std::vector<pulp::platform::FileFilter> filters_;
    bool allow_multiple_ = false;
};

} // namespace pulp::view
