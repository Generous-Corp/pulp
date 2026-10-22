#pragma once

// Editor assets a plug-in binary declares: the scripted-UI entry file, its
// theme file, and the directories its script may load assets from.
//
// The selection belongs to the plug-in target, not to the Pulp libraries it
// links. `pulp_add_plugin(... UI_SCRIPT ...)` and `pulp_use_kit_ui()` generate
// one small source per format target (VST3, CLAP, AU, AUv3, LV2, AAX,
// Standalone, ControlHost) whose static initializer calls
// `register_editor_assets()`. Every editor path — ViewBridge in each adapter,
// the standalone window, the control host — then reads the same runtime value,
// whichever translation unit builds the editor.
//
// Registration runs at static-init time, so the record is POD: no heap, no
// std::string, no logging.

namespace pulp::format {

struct EditorAssets {
    /// Absolute path of the scripted-UI entry file. Null or empty: none.
    const char* script_path = nullptr;
    /// Absolute path of the theme / design-token file. Null or empty: none.
    const char* theme_path = nullptr;
    /// '|'-separated absolute directories the script may load assets from.
    const char* asset_roots = nullptr;
};

/// True when `assets` names a script, theme, or asset root.
bool declares_editor_assets(const EditorAssets& assets) noexcept;

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

/// Test-only: clear the binary-wide record. NOT for production use.
void reset_editor_assets_for_testing() noexcept;

} // namespace pulp::format
