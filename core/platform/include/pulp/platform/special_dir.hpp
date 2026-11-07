#pragma once

/// @file special_dir.hpp
/// Well-known per-user, per-machine and per-bundle directories.
///
/// `special_dir(kind)` is the single place that knows where each platform keeps
/// the user's home, documents, application data, caches, logs and so on, so
/// settings, presets, content and log code do not each carry their own
/// `#ifdef` ladder.
///
/// Every function returns an empty path when the location is unknown (no home
/// in the environment, a mobile hook that has not run yet, a kind that does not
/// exist on that platform). Callers must treat empty as "unavailable" and fail
/// closed rather than fall back to a world-shared location such as `/tmp`.
/// Nothing here creates a directory or checks that it exists.
///
/// Thread safety: all functions are safe to call from any thread. They read the
/// environment, so they are not realtime-safe.
///
/// Per-platform shape (`<name>` is whatever the caller appends):
///
/// | kind               | macOS / iOS                      | Windows                      | Linux                           |
/// |--------------------|----------------------------------|------------------------------|---------------------------------|
/// | home               | $HOME                            | %USERPROFILE%                | $HOME                           |
/// | documents          | ~/Documents                      | Documents known folder       | $XDG_DOCUMENTS_DIR or ~/Documents |
/// | desktop            | ~/Desktop (macOS only)           | Desktop known folder         | $XDG_DESKTOP_DIR or ~/Desktop   |
/// | music              | ~/Music (macOS only)             | Music known folder           | $XDG_MUSIC_DIR or ~/Music       |
/// | app_data           | ~/Library/Application Support    | %APPDATA%                    | $XDG_DATA_HOME or ~/.local/share |
/// | app_data_shared    | /Library/Application Support (macOS only) | %ProgramData%       | /usr/share                      |
/// | config             | ~/Library/Application Support    | %APPDATA%                    | $XDG_CONFIG_HOME or ~/.config   |
/// | temp               | $TMPDIR or /tmp                  | %TMP% / %TEMP%               | $TMPDIR or /tmp                 |
/// | cache              | ~/Library/Caches                 | %LOCALAPPDATA%               | $XDG_CACHE_HOME or ~/.cache     |
/// | logs               | ~/Library/Logs                   | %LOCALAPPDATA%\Logs          | $XDG_STATE_HOME or ~/.local/state, plus /logs |
/// | current_executable | the running executable (a file)  | same                         | same                            |
/// | bundle_dir         | <bundle>/Contents/Resources (macOS), <bundle> (iOS) | <name>.vst3\Contents\Resources, else the module's folder | same as Windows |
/// | user_presets       | ~/Library/Audio/Presets          | %APPDATA%                    | $XDG_CONFIG_HOME or ~/.config   |
///
/// Android resolves everything under the app files directory supplied by the
/// existing `set_app_data_dir` hook (the Kotlin `PulpFileProvider` calls it at
/// startup), so every kind is empty until that hook has run. WebAssembly has no
/// real file system: the kinds resolve inside the in-memory (MEMFS) tree rooted
/// at `/home/web_user`, with `/tmp` for temp and `/` for bundle_dir.
///
/// `bundle_dir` names the resource root of the code that contains
/// `pulp-platform`, so a plugin gets its own bundle rather than the host's. It
/// falls back to the folder holding the module when the module is not inside a
/// bundle. `current_executable` is the host process for a plugin.

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::platform {

enum class SpecialDir {
    home,
    documents,
    desktop,
    music,
    app_data,        ///< Per-user application data.
    app_data_shared, ///< Application data shared by every user of the machine.
    config,          ///< Per-user settings root (an XDG distinction; equals app_data off Linux).
    temp,
    cache,
    logs,
    current_executable,
    bundle_dir,
    user_presets,
};

inline constexpr std::array<SpecialDir, 13> kAllSpecialDirs = {
    SpecialDir::home,
    SpecialDir::documents,
    SpecialDir::desktop,
    SpecialDir::music,
    SpecialDir::app_data,
    SpecialDir::app_data_shared,
    SpecialDir::config,
    SpecialDir::temp,
    SpecialDir::cache,
    SpecialDir::logs,
    SpecialDir::current_executable,
    SpecialDir::bundle_dir,
    SpecialDir::user_presets,
};

/// Stable lower_snake_case name, for diagnostics and tests.
std::string_view special_dir_name(SpecialDir kind) noexcept;

/// Resolve `kind` for the running process. Empty when unavailable.
std::filesystem::path special_dir(SpecialDir kind);

/// Replace the resolved location for `kind` (a mobile host that learns its
/// sandbox paths late, a test that must not touch the real home). An empty
/// `path` removes the override. Thread-safe.
void set_special_dir_override(SpecialDir kind, std::filesystem::path path);
void clear_special_dir_overrides();

// ── Pure resolver ───────────────────────────────────────────────────────────

/// Operating system a path is shaped for. Distinct from `platform::OS` because
/// WebAssembly has its own layout.
enum class SpecialDirOs { macos, ios, windows, linux_desktop, android, web };

/// The host OS this binary was built for.
SpecialDirOs host_special_dir_os() noexcept;

/// Everything the resolver may read, so every platform's layout can be
/// computed (and tested) on any host. Paths are UTF-8 strings written with the
/// separator of the target OS.
struct SpecialDirInputs {
    /// Environment lookup; an unset or empty variable returns nullopt/empty.
    std::function<std::optional<std::string>(std::string_view)> getenv;
    std::string executable;      ///< Absolute path of the running executable.
    std::string module;          ///< Absolute path of the binary holding this code.
    std::string known_documents; ///< Windows known-folder overrides, else derived.
    std::string known_desktop;
    std::string known_music;
    std::string android_files_dir; ///< From the app-data-dir hook.
    std::string android_cache_dir; ///< Optional; defaults to <files>/cache.
};

/// Resolve `kind` for `os` from `inputs` alone. Empty string when unavailable.
std::string special_dir_for(SpecialDirOs os, SpecialDir kind, const SpecialDirInputs& inputs);

} // namespace pulp::platform
