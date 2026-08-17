#include "design_cpp_codegen_internal.hpp"

#include "design_ir_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace pulp::view::cpp_codegen {

void emit_common_layout(std::ostringstream& out,
                        int depth,
                        const EmitContext& ctx,
                        std::string_view var,
                        const IRNode& node,
                        std::optional<LayoutDirection> parent_direction) {
    const auto& opts = ctx.opts;
    emit_line(out, depth, opts.indent_spaces, "auto& flex = " + std::string(var) + "->flex();");
    emit_line(out, depth, opts.indent_spaces,
              "flex.direction = " + flex_direction_expr(node.layout.direction) + ";");
    emit_line(out, depth, opts.indent_spaces,
              "flex.justify_content = " + flex_justify_expr(node.layout.justify) + ";");
    emit_line(out, depth, opts.indent_spaces,
              "flex.align_items = " + flex_align_expr(node.layout.align) + ";");
    // Grid signal mirrors the JS codegen / native materializer: display:grid
    // OR an explicit track template. Templates prefer the v0/TSX contract
    // attributes, falling back to the IR layout fields the Figma producers
    // emit (gridTemplateColumns/Rows).
    if ((node.layout.display && *node.layout.display == "grid") ||
        node.layout.grid_template_columns || node.layout.grid_template_rows) {
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_layout_mode(pulp::view::LayoutMode::grid);");
        emit_line(out, depth, opts.indent_spaces, "{");
        emit_line(out, depth + 1, opts.indent_spaces, "auto& grid = " + std::string(var) + "->grid();");
        std::optional<std::string> template_columns;
        if (auto it = node.attributes.find("pulpGridTemplateColumns"); it != node.attributes.end())
            template_columns = it->second;
        else if (node.layout.grid_template_columns)
            template_columns = *node.layout.grid_template_columns;
        // A grid with no explicit columns gets a single implicit column
        // rather than dropping every child (same fallback as the JS lane).
        emit_line(out, depth + 1, opts.indent_spaces,
                  "grid.template_columns = pulp::view::GridStyle::parse_template(" +
                      cpp_string_literal(template_columns.value_or("1fr")) + ");");
        std::optional<std::string> template_rows;
        if (auto it = node.attributes.find("pulpGridTemplateRows"); it != node.attributes.end())
            template_rows = it->second;
        else if (node.layout.grid_template_rows)
            template_rows = *node.layout.grid_template_rows;
        if (template_rows) {
            emit_line(out, depth + 1, opts.indent_spaces,
                      "grid.template_rows = pulp::view::GridStyle::parse_template(" +
                          cpp_string_literal(*template_rows) + ");");
        }
        if (node.layout.column_gap || node.layout.gap != 0.0f)
            emit_line(out, depth + 1, opts.indent_spaces,
                      "grid.column_gap = " +
                          format_float(node.layout.column_gap.value_or(node.layout.gap)) + ";");
        if (node.layout.row_gap || node.layout.gap != 0.0f)
            emit_line(out, depth + 1, opts.indent_spaces,
                      "grid.row_gap = " + format_float(node.layout.row_gap.value_or(node.layout.gap)) + ";");
        // Track flow direction. Without it a `grid-auto-flow: column` design
        // fills row-first, so implicitly-placed children land transposed.
        if (node.layout.grid_auto_flow)
            emit_line(out, depth + 1, opts.indent_spaces,
                      "grid.auto_flow = pulp::view::GridStyle::parse_auto_flow(" +
                          cpp_string_literal(lower_copy(*node.layout.grid_auto_flow)) + ");");
        emit_line(out, depth, opts.indent_spaces, "}");
    }
    if (node.layout.gap != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.gap = " + format_float(node.layout.gap) + ";");
    emit_optional_float(out, depth, opts, "flex", "row_gap", node.layout.row_gap);
    emit_optional_float(out, depth, opts, "flex", "column_gap", node.layout.column_gap);
    if (node.layout.padding_top != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.padding_top = " + format_float(node.layout.padding_top) + ";");
    if (node.layout.padding_right != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.padding_right = " + format_float(node.layout.padding_right) + ";");
    if (node.layout.padding_bottom != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.padding_bottom = " + format_float(node.layout.padding_bottom) + ";");
    if (node.layout.padding_left != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.padding_left = " + format_float(node.layout.padding_left) + ";");
    emit_optional_float(out, depth, opts, "flex", "margin_top", node.layout.margin_top);
    emit_optional_float(out, depth, opts, "flex", "margin_right", node.layout.margin_right);
    emit_optional_float(out, depth, opts, "flex", "margin_bottom", node.layout.margin_bottom);
    emit_optional_float(out, depth, opts, "flex", "margin_left", node.layout.margin_left);
    emit_optional_float(out, depth, opts, "flex", "flex_grow", node.layout.flex_grow);
    emit_optional_float(out, depth, opts, "flex", "flex_shrink", node.layout.flex_shrink);
    if (node.layout.flex_basis) {
        emit_line(out, depth, opts.indent_spaces, "{");
        emit_line(out, depth + 1, opts.indent_spaces,
                  "const auto basis = pulp::view::Dimension::parse(" +
                      cpp_string_literal(*node.layout.flex_basis) + ");");
        emit_line(out, depth + 1, opts.indent_spaces, "flex.dim_flex_basis = basis;");
        emit_line(out, depth + 1, opts.indent_spaces,
                  "if (basis.unit == pulp::view::DimensionUnit::px) flex.flex_basis = basis.value;");
        emit_line(out, depth, opts.indent_spaces, "}");
    }
    if (node.layout.order)
        emit_line(out, depth, opts.indent_spaces, "flex.order = " + std::to_string(*node.layout.order) + ";");
    if (node.layout.wrap)
        emit_line(out, depth, opts.indent_spaces, "flex.flex_wrap = pulp::view::FlexWrap::wrap;");
    if (node.layout.aspect_ratio)
        emit_line(out, depth, opts.indent_spaces, "flex.aspect_ratio = " + float_expr(ctx, *node.layout.aspect_ratio) + ";");
    if (node.layout.align_self) {
        if (auto expr = flex_align_value_expr(*node.layout.align_self))
            emit_line(out, depth, opts.indent_spaces, "flex.align_self = " + *expr + ";");
    }
    if (node.layout.align_content) {
        const auto align = lower_copy(*node.layout.align_content);
        if (align == "space-between") {
            emit_line(out, depth, opts.indent_spaces,
                      "flex.align_content_space = pulp::view::FlexStyle::AlignContentSpace::space_between;");
        } else if (align == "space-around") {
            emit_line(out, depth, opts.indent_spaces,
                      "flex.align_content_space = pulp::view::FlexStyle::AlignContentSpace::space_around;");
        } else if (align == "space-evenly") {
            emit_line(out, depth, opts.indent_spaces,
                      "flex.align_content_space = pulp::view::FlexStyle::AlignContentSpace::space_evenly;");
        } else if (auto expr = flex_align_value_expr(align)) {
            emit_line(out, depth, opts.indent_spaces, "flex.align_content = " + *expr + ";");
        }
    }

    // Per-child grid placement: "N", "N / M", or "N / span S" line strings
    // (CSS 1-based), resolved to start/end ints at codegen time. Named lines
    // and span-only forms stay on auto-placement — same contract as the JS
    // lane's emit_js_grid_placement and the native materializer.
    auto emit_grid_track = [&](const std::optional<std::string>& value,
                               const char* start_field, const char* end_field) {
        if (!value || value->empty())
            return;
        auto trim = [](std::string s) {
            const auto a = s.find_first_not_of(" \t");
            const auto b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        auto as_int = [](const std::string& t, int& out_i) {
            if (t.empty()) return false;
            char* end = nullptr;
            const long n = std::strtol(t.c_str(), &end, 10);
            if (end == t.c_str()) return false;
            out_i = static_cast<int>(n);
            return true;
        };
        const auto slash = value->find('/');
        const std::string lo = trim(slash == std::string::npos ? *value : value->substr(0, slash));
        const std::string hi = slash == std::string::npos ? std::string() : trim(value->substr(slash + 1));
        int lo_i = 0;
        if (!as_int(lo, lo_i))
            return;
        emit_line(out, depth, opts.indent_spaces,
                  std::string(var) + "->grid()." + start_field + " = " + std::to_string(lo_i) + ";");
        int hi_i = 0;
        if (as_int(hi, hi_i)) {
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->grid()." + end_field + " = " + std::to_string(hi_i) + ";");
        } else if (hi.rfind("span", 0) == 0) {
            int span = 0;
            if (as_int(trim(hi.substr(4)), span) && span > 0)
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->grid()." + end_field + " = " +
                              std::to_string(lo_i + span) + ";");
        }
    };
    emit_grid_track(node.layout.grid_column, "grid_column_start", "grid_column_end");
    emit_grid_track(node.layout.grid_row, "grid_row_start", "grid_row_end");

    auto emit_dimension = [&](std::string_view preferred,
                              std::string_view dim,
                              const std::optional<float>& value) {
        if (!value)
            return;
        emit_line(out, depth, opts.indent_spaces,
                  "flex." + std::string(preferred) + " = " + float_expr(ctx, *value) + ";");
        emit_line(out, depth, opts.indent_spaces,
                  "flex." + std::string(dim) + " = {" + float_expr(ctx, *value) + ", pulp::view::DimensionUnit::px};");
    };
    emit_dimension("preferred_width", "dim_width", node.style.width);
    emit_dimension("preferred_height", "dim_height", node.style.height);
    emit_dimension("min_width", "dim_min_width", node.style.min_width);
    emit_dimension("min_height", "dim_min_height", node.style.min_height);
    emit_dimension("max_width", "dim_max_width", node.style.max_width);
    emit_dimension("max_height", "dim_max_height", node.style.max_height);

    const bool parent_is_row = parent_direction && *parent_direction == LayoutDirection::row;
    const bool parent_is_column = parent_direction && *parent_direction == LayoutDirection::column;
    const bool has_explicit_align_self = node.layout.align_self.has_value();
    if (node.layout.width_mode == SizingMode::fill && !node.style.width) {
        if (!parent_direction || parent_is_row) {
            emit_line(out, depth, opts.indent_spaces, "flex.flex_grow = std::max(flex.flex_grow, 1.0f);");
        } else if (parent_is_column && !has_explicit_align_self) {
            emit_line(out, depth, opts.indent_spaces, "flex.align_self = pulp::view::FlexAlign::stretch;");
        }
    }
    if (node.layout.height_mode == SizingMode::fill && !node.style.height) {
        if (!parent_direction || parent_is_column) {
            emit_line(out, depth, opts.indent_spaces, "flex.flex_grow = std::max(flex.flex_grow, 1.0f);");
        } else if (parent_is_row && !has_explicit_align_self) {
            emit_line(out, depth, opts.indent_spaces, "flex.align_self = pulp::view::FlexAlign::stretch;");
        }
    }
    if (node.layout.width_mode == SizingMode::hug && !node.style.width)
        emit_line(out, depth, opts.indent_spaces, "flex.dim_width = {0.0f, pulp::view::DimensionUnit::auto_};");
    if (node.layout.height_mode == SizingMode::hug && !node.style.height)
        emit_line(out, depth, opts.indent_spaces, "flex.dim_height = {0.0f, pulp::view::DimensionUnit::auto_};");

    // Resize constraints, through the shared mapping every lane consults.
    // Emitted last so an explicit flex value the design already carries wins:
    // `grow` raises flex-grow rather than assigning it, and `stretch` yields to
    // an author-set align-self, matching the fill-sizing rules just above.
    const auto constraints = resolve_layout_constraints(node.layout.h_constraint, node.layout.v_constraint);
    auto emit_auto_margin = [&](const char* dim_field, const char* float_field) {
        emit_line(out, depth, opts.indent_spaces,
                  "flex." + std::string(dim_field) + " = {0.0f, pulp::view::DimensionUnit::auto_};");
        emit_line(out, depth, opts.indent_spaces, "flex." + std::string(float_field) + " = -1.0f;");
    };
    if (constraints.margin_left_auto) emit_auto_margin("dim_margin_left", "margin_left");
    if (constraints.margin_right_auto) emit_auto_margin("dim_margin_right", "margin_right");
    if (constraints.margin_top_auto) emit_auto_margin("dim_margin_top", "margin_top");
    if (constraints.margin_bottom_auto) emit_auto_margin("dim_margin_bottom", "margin_bottom");
    if (constraints.grow)
        emit_line(out, depth, opts.indent_spaces, "flex.flex_grow = std::max(flex.flex_grow, 1.0f);");
    if (constraints.stretch && !has_explicit_align_self)
        emit_line(out, depth, opts.indent_spaces, "flex.align_self = pulp::view::FlexAlign::stretch;");
    if (constraints.fill_width)
        emit_line(out, depth, opts.indent_spaces,
                  "flex.dim_min_width = {100.0f, pulp::view::DimensionUnit::percent};");
    if (constraints.fill_height)
        emit_line(out, depth, opts.indent_spaces,
                  "flex.dim_min_height = {100.0f, pulp::view::DimensionUnit::percent};");
}

void emit_visual_style(std::ostringstream& out,
                       int depth,
                       const EmitContext& ctx,
                       std::string_view var,
                       const IRStyle& style) {
    const auto& opts = ctx.opts;
    auto emit_color = [&](std::string_view method, const std::optional<std::string>& value) {
        if (!value)
            return;
        auto expr = color_expr(ctx, *value);
        if (!expr.empty())
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->" + std::string(method) + "(" + expr + ");");
    };
    emit_color("set_background_color", style.background_color);
    if (style.background_gradient && !style.background_gradient->empty())
        emit_line(out, depth, opts.indent_spaces,
                  "pulp::view::apply_css_background_gradient(*" + std::string(var) + ", " +
                  cpp_string_literal(*style.background_gradient) + ");");
    if (style.background_repeat)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_background_repeat(" + cpp_string_literal(*style.background_repeat) + ");");
    if (style.background_size)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_background_size(" + cpp_string_literal(*style.background_size) + ");");
    if (style.object_fit)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_object_fit(" + cpp_string_literal(*style.object_fit) + ");");
    emit_color("set_inheritable_text_color", style.color);
    emit_color("set_border_color", style.border_color);
    emit_color("set_border_top_color", style.border_top_color);
    emit_color("set_border_right_color", style.border_right_color);
    emit_color("set_border_bottom_color", style.border_bottom_color);
    emit_color("set_border_left_color", style.border_left_color);
    if (style.opacity)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_opacity(" + float_expr(ctx, *style.opacity) + ");");
    if (style.border_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_radius(" + float_expr(ctx, *style.border_radius) + ");");
    if (style.border_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_width(" + float_expr(ctx, *style.border_width) + ");");
    if (style.border_top_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_top_width(" + float_expr(ctx, *style.border_top_width) + ");");
    if (style.border_right_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_right_width(" + float_expr(ctx, *style.border_right_width) + ");");
    if (style.border_bottom_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_bottom_width(" + float_expr(ctx, *style.border_bottom_width) + ");");
    if (style.border_left_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_left_width(" + float_expr(ctx, *style.border_left_width) + ");");
    if (style.border_top_left_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius_tl(" + float_expr(ctx, *style.border_top_left_radius) + ");");
    if (style.border_top_right_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius_tr(" + float_expr(ctx, *style.border_top_right_radius) + ");");
    if (style.border_bottom_right_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius_br(" + float_expr(ctx, *style.border_bottom_right_radius) + ");");
    if (style.border_bottom_left_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius_bl(" + float_expr(ctx, *style.border_bottom_left_radius) + ");");
    if (style.font_family)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_inheritable_font_family(" + cpp_string_literal(*style.font_family) + ");");
    if (style.font_size)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_inheritable_font_size(" + float_expr(ctx, *style.font_size) + ");");
    if (style.font_weight)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_inheritable_font_weight(" + std::to_string(*style.font_weight) + ");");
    if (style.letter_spacing)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_inheritable_letter_spacing(" + float_expr(ctx, *style.letter_spacing) + ");");
    if (style.text_align)
        emit_line(out, depth, opts.indent_spaces,
                  std::string(var) + "->set_inheritable_text_align(static_cast<int>(" + label_align_expr(*style.text_align) + "));");
    // The clip an importer resolved along CSS's containing-block chain, in the
    // node's own space. Emitted alongside `overflow` rather than folded into
    // it: `overflow` clips the node's children — DOM parentage — and this
    // clips the node itself, which is the only one of the two that can express
    // a node escaping a clip its emitted parent is inside.
    if (style.clip_rect) {
        emit_line(out, depth, opts.indent_spaces,
                  std::string(var) + "->set_ancestor_clip_rect({" +
                      float_expr(ctx, style.clip_rect->x) + ", " +
                      float_expr(ctx, style.clip_rect->y) + ", " +
                      float_expr(ctx, style.clip_rect->width) + ", " +
                      float_expr(ctx, style.clip_rect->height) + "});");
    }
    if (style.overflow) {
        std::string lower;
        for (unsigned char c : *style.overflow)
            lower += static_cast<char>(std::tolower(c));
        if (lower == "hidden")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_overflow(pulp::view::View::Overflow::hidden);");
        else if (lower == "scroll" || lower == "auto")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_overflow(pulp::view::View::Overflow::scroll);");
    }
    if (style.position) {
        std::string lower;
        for (unsigned char c : *style.position)
            lower += static_cast<char>(std::tolower(c));
        if (lower == "absolute")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_position(pulp::view::View::Position::absolute);");
        else if (lower == "relative")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_position(pulp::view::View::Position::relative);");
        else if (lower == "fixed")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_position(pulp::view::View::Position::fixed);");
    }
    if (style.top)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_top(" + float_expr(ctx, *style.top) + ");");
    if (style.right)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_right(" + float_expr(ctx, *style.right) + ");");
    if (style.bottom)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_bottom(" + float_expr(ctx, *style.bottom) + ");");
    if (style.left)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_left(" + float_expr(ctx, *style.left) + ");");
    if (style.z_index)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_z_index(" + std::to_string(*style.z_index) + ");");
}

void emit_label_style(std::ostringstream& out,
                      int depth,
                      const EmitContext& ctx,
                      std::string_view var,
                      const IRStyle& style) {
    const auto& opts = ctx.opts;
    if (style.font_family)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_family(" + cpp_string_literal(*style.font_family) + ");");
    if (style.font_size)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_size(" + float_expr(ctx, *style.font_size) + ");");
    if (style.font_weight)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_weight(" + std::to_string(*style.font_weight) + ");");
    if (style.font_style && *style.font_style == "italic")
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_style(1);");
    else if (style.font_style && style.font_style->rfind("oblique", 0) == 0)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_style(2);");
    if (style.letter_spacing)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_letter_spacing(" + float_expr(ctx, *style.letter_spacing) + ");");
    if (style.line_height)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_line_height(" + float_expr(ctx, *style.line_height) + ");");
    if (style.text_align)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_align(" + label_align_expr(*style.text_align) + ");");
    if (style.color) {
        auto expr = color_expr(ctx, *style.color);
        if (!expr.empty())
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_color(" + expr + ");");
    }
    if (style.text_transform) {
        const auto value = lower_copy(*style.text_transform);
        if (value == "uppercase")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_transform(pulp::view::Label::TextTransform::uppercase);");
        else if (value == "lowercase")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_transform(pulp::view::Label::TextTransform::lowercase);");
        else if (value == "capitalize")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_transform(pulp::view::Label::TextTransform::capitalize);");
    }
    if (style.text_decoration) {
        const auto value = lower_copy(*style.text_decoration);
        if (value.find("underline") != std::string::npos)
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_decoration(pulp::view::Label::TextDecoration::underline);");
        else if (value.find("line-through") != std::string::npos)
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_decoration(pulp::view::Label::TextDecoration::line_through);");
        else if (value.find("overline") != std::string::npos)
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_decoration(pulp::view::Label::TextDecoration::overline);");
    }
    if (style.white_space && *style.white_space != "nowrap")
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_multi_line(true);");
}

void emit_svg_paint(std::ostringstream& out,
                    int depth,
                    const EmitContext& ctx,
                    std::string_view var,
                    const IRNode& node,
                    bool supports_fill) {
    const auto& opts = ctx.opts;
    if (supports_fill) {
        for (const char* key : {"svg_fill", "fill"}) {
            if (auto fill = attr(node, key)) {
                if (*fill == "none") {
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->clear_fill();");
                } else if (auto expr = color_expr(ctx, *fill); !expr.empty()) {
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_fill_color(" + expr + ");");
                }
                break;
            }
        }
        for (const char* key : {"svg_fill_gradient", "fillGradient"}) {
            if (auto gradient = attr(node, key)) {
                if (!gradient->empty()) {
                    emit_line(out, depth, opts.indent_spaces,
                              std::string(var) + "->set_fill_gradient(" +
                                  cpp_string_literal(*gradient) + ");");
                }
                break;
            }
        }
    }
    for (const char* key : {"svg_stroke", "stroke"}) {
        if (auto stroke = attr(node, key)) {
            if (*stroke == "none") {
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->clear_stroke();");
            } else if (auto expr = color_expr(ctx, *stroke); !expr.empty()) {
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_stroke_color(" + expr + ");");
            }
            break;
        }
    }
    for (const char* key : {"svg_stroke_gradient", "strokeGradient"}) {
        if (auto gradient = attr(node, key)) {
            if (!gradient->empty()) {
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_stroke_gradient(" +
                              cpp_string_literal(*gradient) + ");");
            }
            break;
        }
    }
    for (const char* key : {"svg_stroke_width", "stroke-width", "strokeWidth"}) {
        if (auto stroke_width = attr_float(node, key)) {
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_stroke_width(" +
                          float_expr(ctx, *stroke_width) + ");");
            break;
        }
    }
}


}  // namespace pulp::view::cpp_codegen
