#pragma once

/// @file design_tokens.hpp
/// W3C Design Tokens and external-tool token sync. Converts between Pulp
/// Theme, the normalized IR token collection, and the on-disk token formats
/// emitted by W3C tooling, Figma Variables, and Google Stitch.

#include <pulp/view/design_ir.hpp>
#include <pulp/view/theme.hpp>
// parse_w3c_tokens / export_w3c_tokens live in their own always-compiled TU
// (reached at runtime by the WidgetBridge theme API); re-exported here so
// existing includers of this header still see them.
#include <pulp/view/w3c_tokens.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

// ── W3C Design Tokens ───────────────────────────────────────────────────
// parse_w3c_tokens / export_w3c_tokens are declared in <pulp/view/w3c_tokens.hpp>
// (included above) — they are runtime-needed and not gated by
// PULP_ENABLE_DESIGN_IMPORT.

/// Export a Pulp Theme to CSS custom properties (variables).
///
/// Base (light/default) tokens are emitted under `:root`; tokens whose name
/// ends in the `.dark` multi-mode suffix (the convention the Figma plugin and
/// DESIGN.md body parser use for dark-mode values) are emitted as overrides
/// under `@media (prefers-color-scheme: dark) { :root { … } }`. Token names map
/// to custom properties by replacing `.` with `-` (e.g. `color.bg` →
/// `--color-bg`); colors become hex, dimensions get a `px` unit, strings are
/// emitted verbatim. This is the themed output sink for the modes the importers
/// capture — consumable by Pulp's `var(--x)` runtime and by web tooling.
std::string export_css_variables(const Theme& theme);

/// The CSS custom-property id a token binds through: `--` + the token name with
/// its trailing `.dark` mode suffix stripped and every `.` mapped to `-` (e.g.
/// `color.bg` → `--color-bg`, `color.bg.dark` → `--color-bg`). This is the single
/// owner of the token → `var(--x)` mapping; export_css_variables and the
/// adherence lint both call it so they can never disagree about a token's id.
std::string token_css_var(const std::string& token_name);

/// Convert IR tokens to a Pulp Theme.
///
/// Token names are copied across verbatim, then the widget keys Pulp's own
/// primitives resolve (`control.fill`, `knob.arc`, `meter.green`, …) are
/// filled from the design tokens that mean the same thing — `accent`,
/// `line-strong`, `signal-low` and the shadcn / Material spellings of each.
/// Every widget key is a direct copy of a colour the design stated: nothing is
/// hue-shifted, blended, or defaulted, so a design's palette reaches its
/// primitives without Pulp adding a colour of its own.
///
/// A widget key the design supplies no source for is left UNSET, and its name
/// is appended to @p unresolved_widget_tokens when that is non-null. The widget
/// then paints its own built-in default — an honest "the design did not say" —
/// rather than a synthesized colour that would drift from the design's palette.
/// A design that names a widget key itself always keeps its own value.
Theme ir_tokens_to_theme(const IRTokens& tokens,
                         std::vector<std::string>* unresolved_widget_tokens = nullptr);

/// The diagnostic naming the widget keys a design supplied no colour for, or
/// nothing when it supplied them all. Takes the `unresolved_widget_tokens` that
/// ir_tokens_to_theme filled. One diagnostic lists every key, because a design
/// with no tokens at all leaves them all unset and one warning per key would
/// bury the rest of the import's diagnostics.
std::optional<ImportDiagnostic> unmapped_widget_token_diagnostic(
    const std::vector<std::string>& unresolved);

/// Convert a Pulp Theme to W3C-compatible IR tokens.
IRTokens theme_to_ir_tokens(const Theme& theme);

// ── External tool token sync ────────────────────────────────────────────

/// Parse Figma Variables JSON (from MCP get_variable_defs) into a Pulp Theme.
/// Figma variables are organized into collections with modes.
/// Each variable has resolvedValue for the default mode.
Theme parse_figma_variables(const std::string& json);

/// Export a Pulp Theme as Figma Variables-compatible JSON.
/// Produces the structure expected by Figma's variable creation APIs.
std::string export_figma_variables(const Theme& theme);

/// Parse a Stitch Design System JSON (from MCP list_design_systems/get_screen)
/// into a Pulp Theme. Maps Stitch colors, fonts, and roundness to tokens.
Theme parse_stitch_design_system(const std::string& json);

/// Export a Pulp Theme as Stitch Design System-compatible JSON.
/// Produces the structure expected by Stitch's create/update_design_system APIs.
std::string export_stitch_design_system(const Theme& theme);

} // namespace pulp::view
