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
#include <string>
#include <unordered_map>

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
