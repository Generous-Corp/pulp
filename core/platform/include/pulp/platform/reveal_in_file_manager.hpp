#pragma once

#include <filesystem>

namespace pulp::platform {

/// What `reveal_in_file_manager` managed to do. The outcome is reported
/// honestly per platform rather than collapsed to a bool, because the
/// platforms do not offer the same capability.
enum class RevealOutcome {
    /// The platform file manager was asked to show the item selected in its
    /// parent folder (macOS Finder, Windows Explorer).
    revealed,
    /// The item's containing folder was opened but the item itself could not
    /// be selected (Linux: no portable "select this file" request exists).
    opened_containing_folder,
    /// The path does not exist; nothing was shown.
    not_found,
    /// This platform has no file manager to ask (iOS, Android, or a Linux
    /// system without `xdg-open`).
    unsupported,
    /// The platform call was attempted and failed.
    failed,
};

/// Short stable name for an outcome ("revealed", "not_found", ...).
const char* to_string(RevealOutcome outcome) noexcept;

/// Show `path` in the platform file manager: Finder (NSWorkspace
/// activateFileViewerSelectingURLs), Windows Explorer
/// (SHOpenFolderAndSelectItems), or on Linux the containing folder via
/// `xdg-open`. A directory is selected within its parent on macOS and Windows.
/// Checks existence first, so a missing path yields `not_found` without
/// touching the shell. Call from the main/UI thread.
RevealOutcome reveal_in_file_manager(const std::filesystem::path& path);

} // namespace pulp::platform
