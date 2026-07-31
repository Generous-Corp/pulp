#pragma once

#include <pulp/view/design_codegen.hpp>

#include "design_import_native_common.hpp"

#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::view::cpp_codegen {

struct Component {
    const IRNode* node = nullptr;
    const ResolvedNativeNode* resolved = nullptr;
    std::string function_name;
    std::string rule_comment;
    std::optional<LayoutDirection> parent_direction;
};

struct TokenSymbols {
    std::unordered_map<std::string, std::string> color_by_name;
    std::unordered_map<std::string, std::string> color_by_value;
    std::unordered_map<std::string, std::string> dimension_by_name;
    std::vector<std::pair<float, std::string>> dimension_by_value;
    std::unordered_map<std::string, std::string> string_by_name;
    std::unordered_map<std::string, std::string> string_by_value;
};

struct AssetSymbols {
    std::unordered_map<std::string, std::string> id_by_asset_id;
};

struct EmitContext {
    const CppExportOptions& opts;
    const IRAssetManifest& manifest;
    TokenSymbols tokens;
    AssetSymbols assets;
    std::vector<Component> components;
    std::unordered_map<const IRNode*, std::string> extracted;
    std::set<std::string> used_functions;
    std::set<std::string> used_token_names;
    std::set<std::string> used_asset_names;
};

std::string cpp_string_literal(std::string_view input);
std::string format_float(float value);
std::optional<float> attr_float(const IRNode& node, std::string_view key);
std::string asset_uri(const IRAssetManifest& manifest, std::string_view asset_id);
std::string asset_uri_expression(const IRAssetManifest& manifest, std::string_view asset_id);
std::string flex_direction_expr(LayoutDirection direction);
std::string flex_justify_expr(LayoutAlign align);
std::string flex_align_expr(LayoutAlign align);
std::string label_align_expr(std::string_view value);
void collect_components(const IRNode& node,
                        const ResolvedNativeNode& resolved,
                        EmitContext& ctx,
                        std::optional<LayoutDirection> parent_direction,
                        bool root = false);
void emit_line(std::ostringstream& out, int depth, int spaces, std::string_view text);
void emit_optional_float(std::ostringstream& out,
                         int depth,
                         const CppExportOptions& opts,
                         std::string_view target,
                         std::string_view field,
                         const std::optional<float>& value,
                         std::string_view expr = {});
TokenSymbols build_token_symbols(const DesignIR& ir, EmitContext& ctx);
AssetSymbols build_asset_symbols(const IRAssetManifest& manifest, EmitContext& ctx);
std::string color_expr(const EmitContext& ctx, std::string_view value);
std::string float_expr(const EmitContext& ctx, float value);
std::string asset_id_expr(const EmitContext& ctx, std::string_view asset_id);
std::optional<std::string> flex_align_value_expr(std::string_view value);

void emit_common_layout(std::ostringstream& out,
                        int depth,
                        const EmitContext& ctx,
                        std::string_view var,
                        const IRNode& node,
                        std::optional<LayoutDirection> parent_direction);
void emit_visual_style(std::ostringstream& out,
                       int depth,
                       const EmitContext& ctx,
                       std::string_view var,
                       const IRStyle& style);
void emit_label_style(std::ostringstream& out,
                      int depth,
                      const EmitContext& ctx,
                      std::string_view var,
                      const IRStyle& style);
void emit_svg_paint(std::ostringstream& out,
                    int depth,
                    const EmitContext& ctx,
                    std::string_view var,
                    const IRNode& node,
                    bool supports_fill);

void emit_function(std::ostringstream& out,
                   EmitContext& ctx,
                   std::string_view function_name,
                   const IRNode& node,
                   const ResolvedNativeNode& resolved,
                   std::optional<LayoutDirection> parent_direction,
                   std::string_view comment = {});
void emit_namespace_open(std::ostringstream& out, std::string_view ns);
void emit_namespace_close(std::ostringstream& out, std::string_view ns);
std::string trim_trailing_blank_lines(std::string text);
void emit_tokens(std::ostringstream& out, const DesignIR& ir, const EmitContext& ctx);
void emit_asset_constants(std::ostringstream& out,
                          const IRAssetManifest& manifest,
                          const EmitContext& ctx);
void emit_manifest(std::ostringstream& out,
                   const IRAssetManifest& manifest,
                   const EmitContext& ctx);

}  // namespace pulp::view::cpp_codegen
