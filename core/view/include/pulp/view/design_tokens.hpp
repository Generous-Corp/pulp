#pragma once

/// @file design_tokens.hpp
/// W3C Design Tokens and external-tool token sync. Converts between Pulp
/// Theme, the normalized IR token collection, and the on-disk token formats
/// emitted by W3C tooling, Figma Variables, and Google Stitch.

#include <pulp/view/design_ir.hpp>
#include <pulp/view/theme.hpp>
#include <string>
#include <unordered_map>

namespace pulp::view {

// ── W3C Design Tokens ───────────────────────────────────────────────────

/// Parse W3C Design Tokens JSON into a Pulp Theme.
///
/// W3C format:
/// @code
/// {
///   "color": {
///     "primary": { "$value": "#89B4FA", "$type": "color" },
///     "bg": { "$value": "#1E1E2E", "$type": "color" }
///   },
///   "dimension": {
///     "spacing-md": { "$value": "8px", "$type": "dimension" }
///   }
/// }
/// @endcode
Theme parse_w3c_tokens(const std::string& json);

/// Export a Pulp Theme to W3C Design Tokens JSON format.
std::string export_w3c_tokens(const Theme& theme);

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

/// Convert IR tokens to a Pulp Theme.
Theme ir_tokens_to_theme(const IRTokens& tokens);

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
