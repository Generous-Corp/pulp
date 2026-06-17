#pragma once

/// @file w3c_tokens.hpp
/// W3C Design Tokens <-> Pulp Theme conversion (runtime-needed subset).
///
/// `parse_w3c_tokens` / `export_w3c_tokens` are reached at runtime by the default
/// WidgetBridge theme API (`importDesignTokens` / `exportDesignTokens`), so they
/// live in their own always-compiled translation unit, independent of the
/// design-import authoring subsystem that `PULP_ENABLE_DESIGN_IMPORT` gates out
/// of shipped plugins. This header deliberately depends only on `theme.hpp` (no
/// `design_ir.hpp` / `design_import.hpp`) so the runtime path never drags in the
/// authoring cluster.

#include <string>

#include <pulp/view/theme.hpp>

namespace pulp::view {

/// Parse W3C / DTCG Design Tokens JSON into a Pulp Theme.
Theme parse_w3c_tokens(const std::string& json);

/// Export a Pulp Theme to W3C Design Tokens JSON.
std::string export_w3c_tokens(const Theme& theme);

}  // namespace pulp::view
