// editor_url.hpp — Inspector source-jump editor URI configuration.
//
// Phase 5.3 of the inspector source-jump roadmap
// (planning/2026-05-19-inspector-phase5-source-jump-spike.md). This is the
// smallest concrete slice — the configuration plumbing for *which* editor
// URI scheme to use when a future Phase 5.1 source-jump action wants to
// open a file:line in the user's editor. No jumping happens here; this
// file only deals with the template and its substitution.
//
// Common editor URL templates (any of these are valid `editor_url_template`
// values for `InspectorConfig`):
//
//   VS Code   : "vscode://file/{path}:{line}"
//   Cursor    : "cursor://file/{path}:{line}"
//   Zed       : "zed://file/{path}:{line}:{col}"
//   Sublime   : "sublime://open?url=file://{path}&line={line}"
//   JetBrains : "idea://open?file={path}&line={line}"
//
// The substitution helper `format_editor_url` replaces `{path}`, `{line}`,
// and `{col}` tokens with the supplied values. Tokens not present in the
// template are silently ignored (so a Cursor template without `{col}` will
// still render correctly when a column is supplied).
//
// The runtime config (`InspectorConfig::editor_url_template`) is overridden
// by the `PULP_INSPECTOR_EDITOR_URL` environment variable when present, so
// developers can switch editors per-shell without rebuilding.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect {

/// Inspector runtime configuration. Currently scoped to source-jump
/// editor URI plumbing (Phase 5.3). Future inspector-wide settings
/// (e.g. theme, default tab) can be added here without churning the
/// per-component constructors.
struct InspectorConfig {
    /// URL template used to open a source file in the user's editor.
    /// Recognized substitution tokens: `{path}`, `{line}`, `{col}`.
    /// See header comment for common presets.
    std::string editor_url_template = "vscode://file/{path}:{line}";
};

/// Source that produced the effective editor URL template — used by
/// `Inspector.getEditorUrlTemplate` so clients can show which override
/// is in force.
enum class EditorUrlSource {
    Default,      ///< Built-in fallback (vscode://file/{path}:{line}).
    Config,       ///< Set via InspectorConfig / setEditorUrlTemplate.
    Environment,  ///< Overridden by PULP_INSPECTOR_EDITOR_URL.
};

/// Substitute `{path}`, `{line}`, and `{col}` in `tmpl` with the supplied
/// values. Tokens not present in the template are skipped. `col` is
/// optional; when omitted, `{col}` substitutes as an empty string (the
/// expected behavior for templates that omit the token entirely).
std::string format_editor_url(std::string_view tmpl,
                              std::string_view path,
                              int line,
                              std::optional<int> col = std::nullopt);

/// Validate a candidate editor URL template. A template is valid iff it
/// contains the `{path}` token — without a path the URI cannot point at
/// the user's source file. Returns true on success; on failure, sets
/// `error_out` to a human-readable message when non-null.
bool validate_editor_url_template(std::string_view tmpl,
                                  std::string* error_out = nullptr);

/// Look up the environment override (`PULP_INSPECTOR_EDITOR_URL`).
/// Returns `std::nullopt` when unset or empty. Centralized here so tests
/// and the protocol handler share one implementation.
std::optional<std::string> editor_url_env_override();

/// Compute the effective template and its source given the supplied
/// config. Environment override (when set) wins over config, which wins
/// over the built-in default. The default string matches
/// `InspectorConfig::editor_url_template`'s initializer.
struct EffectiveEditorUrl {
    std::string template_str;
    EditorUrlSource source = EditorUrlSource::Default;
};

EffectiveEditorUrl effective_editor_url(const InspectorConfig& config);

/// Stringify the source enum for protocol responses.
std::string_view editor_url_source_name(EditorUrlSource source);

} // namespace pulp::inspect
