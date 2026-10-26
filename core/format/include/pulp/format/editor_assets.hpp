#pragma once

#include <cstddef>

// Editor assets a plug-in binary declares: the scripted-UI entry file, its
// theme file, the directories its script may load assets from, and the
// embedded copy of the editor that a shipped image carries.
//
// The selection belongs to the plug-in target, not to the Pulp libraries it
// links. `pulp_add_plugin(... UI_SCRIPT ...)` generates one small source for
// each VST3, CLAP, AU, AAX, Standalone and ControlHost target, and
// `pulp_use_kit_ui()` one for every format target that exists when it runs;
// that source's static initializer calls `register_editor_assets()`. Every
// editor path (ViewBridge in each adapter, the standalone window, the control
// host) then reads the same runtime value, whichever translation unit builds
// the editor.
//
// Two kinds of value live in one record:
//
// - The embedded editor: the script's bytes, the theme beside it, and the
//   asset files beside it, compiled into the image by the same helper. Every
//   build carries these, so a shipped plug-in never depends on a file outside
//   its own bundle, and the developer's directory is not a string in the binary.
// - The on-disk paths: absolute build-machine paths a dev build
//   (`PULP_DEV_BUILD`) also records so a hot-reloading editor can watch the
//   file being edited. A release build records none. The paths are absolute
//   build-machine paths, so `UI_SCRIPT` is not applied to AUv3: an iOS app
//   extension cannot resolve them, and an AUv3 editor ships its script inside
//   the extension bundle instead.
//
// Registration runs at static-init time, so the record is POD: no heap, no
// std::string, no logging. The byte arrays it points at are constant-initialized
// in their own translation units, so the order in which those units initialize
// cannot leave a dangling pointer here.

namespace pulp::format {

/// One file embedded beside the editor script. `name` is the path the script
/// uses for it, relative to the script's directory (or to a declared asset
/// root), with forward slashes: `assets/knob.png`.
struct EmbeddedEditorFile {
    const char* name;
    const unsigned char* data;
    std::size_t size;
};

struct EditorAssets {
    /// Absolute path of the scripted-UI entry file. Null or empty: none.
    const char* script_path = nullptr;
    /// Absolute path of the theme / design-token file. Null or empty: none.
    const char* theme_path = nullptr;
    /// '|'-separated absolute directories the script may load assets from.
    const char* asset_roots = nullptr;

    /// File name of the embedded entry script (`main.js`), for diagnostics.
    /// Never a directory.
    const char* script_name = nullptr;
    /// The embedded entry script's bytes. Null or zero-length: no embedded
    /// editor.
    const unsigned char* script = nullptr;
    std::size_t script_size = 0;
    /// The embedded theme override (JSON), or null/0 when the build had none.
    const unsigned char* theme = nullptr;
    std::size_t theme_size = 0;
    /// Files embedded beside the script, resolved by the embedded editor in
    /// place of its script directory. Null/0: none.
    const EmbeddedEditorFile* files = nullptr;
    std::size_t file_count = 0;
};

/// True when `assets` names a script, theme, or asset root, or embeds a script.
bool declares_editor_assets(const EditorAssets& assets) noexcept;

/// True when `assets` carries an embedded entry script.
bool embeds_editor_script(const EditorAssets& assets) noexcept;

/// Record this binary's editor assets. Static-init safe; last write wins.
void register_editor_assets(const EditorAssets& assets) noexcept;

/// The binary-wide record from `register_editor_assets()`, or an empty record.
EditorAssets registered_editor_assets() noexcept;

/// Editor assets for the plug-in with reverse-DNS id `plugin_id`. A keyed
/// bundle registration (`PluginRegistration::ui_*`) that declares any asset
/// wins, so each plug-in in a multi-plug-in bundle can carry its own editor;
/// otherwise the binary-wide record applies. A null id selects the
/// binary-wide record.
EditorAssets editor_assets_for(const char* plugin_id) noexcept;

/// Link and install the scripted editor builder (see
/// `scripted_editor_builder_slot()` in editor_ui.hpp). The generated source of
/// a target that declares a UI script calls this at static init; a binary
/// whose script comes only from a keyed bundle registration calls it itself.
/// Defined in pulp-format-view, so only a binary that calls it links the
/// script runtime. Static-init safe.
void install_scripted_editor_builder() noexcept;

/// Test-only: clear the binary-wide record. NOT for production use.
void reset_editor_assets_for_testing() noexcept;

} // namespace pulp::format
