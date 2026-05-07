// Semantic visual snapshot harness for deterministic Yoga layout fixtures.

#include <pulp/view/view.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
using pulp::view::Dimension;
using pulp::view::DimensionUnit;
using pulp::view::FlexAlign;
using pulp::view::FlexDirection;
using pulp::view::FlexJustify;
using pulp::view::FlexStyle;
using pulp::view::FlexWrap;
using pulp::view::Rect;
using pulp::view::View;

struct FixtureTree {
    std::unique_ptr<View> root;
    std::unordered_map<const View*, std::string> types;
};

struct BuildContext {
    int next_id = 0;
    std::unordered_map<const View*, std::string> types;
};

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("could not open fixture: " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::optional<choc::value::ValueView> get_any(const choc::value::ValueView& obj,
                                              std::initializer_list<const char*> keys) {
    if (!obj.isObject()) return std::nullopt;
    for (auto* key : keys) {
        if (obj.hasObjectMember(key)) return obj[key];
    }
    return std::nullopt;
}

bool is_number(const choc::value::ValueView& value) {
    return value.isInt32() || value.isInt64() || value.isFloat32() || value.isFloat64();
}

double number_value(const choc::value::ValueView& value, double fallback = 0.0) {
    if (value.isInt32() || value.isInt64()) return static_cast<double>(value.getInt64());
    if (value.isFloat32() || value.isFloat64()) return value.getFloat64();
    return fallback;
}

std::optional<double> get_number(const choc::value::ValueView& obj,
                                 std::initializer_list<const char*> keys) {
    auto value = get_any(obj, keys);
    if (!value || !is_number(*value)) return std::nullopt;
    return number_value(*value);
}

std::optional<std::string> get_string(const choc::value::ValueView& obj,
                                      std::initializer_list<const char*> keys) {
    auto value = get_any(obj, keys);
    if (!value || !value->isString()) return std::nullopt;
    return value->getWithDefault<std::string>("");
}

std::optional<bool> get_bool(const choc::value::ValueView& obj,
                             std::initializer_list<const char*> keys) {
    auto value = get_any(obj, keys);
    if (!value || !value->isBool()) return std::nullopt;
    return value->getWithDefault<bool>(false);
}

std::optional<Dimension> get_dimension(const choc::value::ValueView& obj,
                                       std::initializer_list<const char*> keys) {
    auto value = get_any(obj, keys);
    if (!value) return std::nullopt;
    if (is_number(*value)) {
        return Dimension{static_cast<float>(number_value(*value)), DimensionUnit::px};
    }
    if (value->isString()) {
        return Dimension::parse(value->getWithDefault<std::string>(""));
    }
    return std::nullopt;
}

FlexDirection parse_direction(std::string_view s) {
    if (s == "row") return FlexDirection::row;
    if (s == "row_reverse" || s == "row-reverse") return FlexDirection::row_reverse;
    if (s == "column_reverse" || s == "column-reverse") return FlexDirection::column_reverse;
    return FlexDirection::column;
}

FlexAlign parse_align(std::string_view s, FlexAlign fallback) {
    if (s == "start" || s == "flex-start") return FlexAlign::start;
    if (s == "center") return FlexAlign::center;
    if (s == "end" || s == "flex-end") return FlexAlign::end;
    if (s == "stretch") return FlexAlign::stretch;
    if (s == "auto") return FlexAlign::auto_;
    if (s == "baseline") return FlexAlign::baseline;
    return fallback;
}

FlexJustify parse_justify(std::string_view s) {
    if (s == "center") return FlexJustify::center;
    if (s == "end" || s == "flex-end") return FlexJustify::end_;
    if (s == "space_between" || s == "space-between") return FlexJustify::space_between;
    if (s == "space_around" || s == "space-around") return FlexJustify::space_around;
    if (s == "space_evenly" || s == "space-evenly") return FlexJustify::space_evenly;
    return FlexJustify::start;
}

FlexWrap parse_wrap(std::string_view s) {
    if (s == "wrap") return FlexWrap::wrap;
    if (s == "wrap_reverse" || s == "wrap-reverse") return FlexWrap::wrap_reverse;
    return FlexWrap::no_wrap;
}

void apply_dimension_to(float& legacy, Dimension& dim_slot, const std::optional<Dimension>& dim) {
    if (!dim) return;
    if (dim->unit == DimensionUnit::px) {
        legacy = dim->value;
    } else {
        dim_slot = *dim;
    }
}

void apply_style(View& view, const choc::value::ValueView& style) {
    if (!style.isObject()) return;
    auto& flex = view.flex();

    if (auto value = get_string(style, {"direction", "flex_direction", "flexDirection"}))
        flex.direction = parse_direction(*value);
    if (auto value = get_string(style, {"align_items", "alignItems"}))
        flex.align_items = parse_align(*value, flex.align_items);
    if (auto value = get_string(style, {"align_self", "alignSelf"}))
        flex.align_self = parse_align(*value, flex.align_self);
    if (auto value = get_string(style, {"justify_content", "justifyContent"}))
        flex.justify_content = parse_justify(*value);
    if (auto value = get_string(style, {"flex_wrap", "flexWrap", "wrap"}))
        flex.flex_wrap = parse_wrap(*value);

    if (auto value = get_number(style, {"gap"})) flex.gap = static_cast<float>(*value);
    if (auto value = get_number(style, {"row_gap", "rowGap"})) flex.row_gap = static_cast<float>(*value);
    if (auto value = get_number(style, {"column_gap", "columnGap"})) flex.column_gap = static_cast<float>(*value);

    if (auto value = get_number(style, {"padding"})) flex.padding = static_cast<float>(*value);
    apply_dimension_to(flex.padding_top, flex.dim_padding_top,
                       get_dimension(style, {"padding_top", "paddingTop"}));
    apply_dimension_to(flex.padding_right, flex.dim_padding_right,
                       get_dimension(style, {"padding_right", "paddingRight"}));
    apply_dimension_to(flex.padding_bottom, flex.dim_padding_bottom,
                       get_dimension(style, {"padding_bottom", "paddingBottom"}));
    apply_dimension_to(flex.padding_left, flex.dim_padding_left,
                       get_dimension(style, {"padding_left", "paddingLeft"}));

    if (auto value = get_number(style, {"margin"})) flex.margin = static_cast<float>(*value);
    apply_dimension_to(flex.margin_top, flex.dim_margin_top,
                       get_dimension(style, {"margin_top", "marginTop"}));
    apply_dimension_to(flex.margin_right, flex.dim_margin_right,
                       get_dimension(style, {"margin_right", "marginRight"}));
    apply_dimension_to(flex.margin_bottom, flex.dim_margin_bottom,
                       get_dimension(style, {"margin_bottom", "marginBottom"}));
    apply_dimension_to(flex.margin_left, flex.dim_margin_left,
                       get_dimension(style, {"margin_left", "marginLeft"}));

    if (auto value = get_number(style, {"flex_grow", "flexGrow"})) flex.flex_grow = static_cast<float>(*value);
    if (auto value = get_number(style, {"flex_shrink", "flexShrink"})) flex.flex_shrink = static_cast<float>(*value);
    apply_dimension_to(flex.flex_basis, flex.dim_flex_basis,
                       get_dimension(style, {"flex_basis", "flexBasis"}));

    apply_dimension_to(flex.preferred_width, flex.dim_width, get_dimension(style, {"width"}));
    apply_dimension_to(flex.preferred_height, flex.dim_height, get_dimension(style, {"height"}));
    apply_dimension_to(flex.min_width, flex.dim_min_width, get_dimension(style, {"min_width", "minWidth"}));
    apply_dimension_to(flex.min_height, flex.dim_min_height, get_dimension(style, {"min_height", "minHeight"}));
    apply_dimension_to(flex.max_width, flex.dim_max_width, get_dimension(style, {"max_width", "maxWidth"}));
    apply_dimension_to(flex.max_height, flex.dim_max_height, get_dimension(style, {"max_height", "maxHeight"}));

    if (auto value = get_number(style, {"aspect_ratio", "aspectRatio"}))
        flex.aspect_ratio = static_cast<float>(*value);
    if (auto value = get_number(style, {"order"})) flex.order = static_cast<int>(*value);

    if (auto value = get_string(style, {"position"})) {
        if (*value == "absolute") view.set_position(View::Position::absolute);
        else if (*value == "fixed") view.set_position(View::Position::fixed);
        else if (*value == "static") view.set_position(View::Position::static_);
        else view.set_position(View::Position::relative);
    }

    if (auto dim = get_dimension(style, {"top"})) {
        if (dim->unit == DimensionUnit::percent) view.set_top(dim->value, DimensionUnit::percent);
        else if (dim->unit == DimensionUnit::px) view.set_top(dim->value);
    }
    if (auto dim = get_dimension(style, {"right"})) {
        if (dim->unit == DimensionUnit::percent) view.set_right(dim->value, DimensionUnit::percent);
        else if (dim->unit == DimensionUnit::px) view.set_right(dim->value);
    }
    if (auto dim = get_dimension(style, {"bottom"})) {
        if (dim->unit == DimensionUnit::percent) view.set_bottom(dim->value, DimensionUnit::percent);
        else if (dim->unit == DimensionUnit::px) view.set_bottom(dim->value);
    }
    if (auto dim = get_dimension(style, {"left"})) {
        if (dim->unit == DimensionUnit::percent) view.set_left(dim->value, DimensionUnit::percent);
        else if (dim->unit == DimensionUnit::px) view.set_left(dim->value);
    }

    if (auto value = get_number(style, {"z_index", "zIndex"})) view.set_z_index(static_cast<int>(*value));
    if (auto value = get_string(style, {"overflow"}))
        view.set_overflow(*value == "hidden" ? View::Overflow::hidden : View::Overflow::visible);
    if (auto value = get_bool(style, {"visible"})) view.set_visible(*value);
    if (auto value = get_bool(style, {"hit_testable", "hitTestable"})) view.set_hit_testable(*value);

    if (auto value = get_number(style, {"border_width", "borderWidth"}))
        view.set_border_width(static_cast<float>(*value));
    if (auto value = get_number(style, {"border_top_width", "borderTopWidth"}))
        view.set_border_top_width(static_cast<float>(*value));
    if (auto value = get_number(style, {"border_right_width", "borderRightWidth"}))
        view.set_border_right_width(static_cast<float>(*value));
    if (auto value = get_number(style, {"border_bottom_width", "borderBottomWidth"}))
        view.set_border_bottom_width(static_cast<float>(*value));
    if (auto value = get_number(style, {"border_left_width", "borderLeftWidth"}))
        view.set_border_left_width(static_cast<float>(*value));
}

std::unique_ptr<View> build_view(const choc::value::ValueView& spec, BuildContext& ctx) {
    auto view = std::make_unique<View>();
    auto id = get_string(spec, {"id"}).value_or("node-" + std::to_string(ctx.next_id++));
    auto type = get_string(spec, {"type"}).value_or("View");
    view->set_id(id);
    ctx.types[view.get()] = type;

    if (auto style = get_any(spec, {"style"})) apply_style(*view, *style);

    if (auto children = get_any(spec, {"children"}); children && children->isArray()) {
        for (uint32_t i = 0; i < children->size(); ++i)
            view->add_child(build_view((*children)[i], ctx));
    }

    return view;
}

FixtureTree build_tree(const choc::value::ValueView& fixture) {
    if (!fixture.isObject()) throw std::runtime_error("fixture root must be a JSON object");
    auto tree = get_any(fixture, {"tree"});
    if (!tree || !tree->isObject()) throw std::runtime_error("fixture missing object field: tree");

    BuildContext ctx;
    FixtureTree out;
    out.root = build_view(*tree, ctx);
    out.types = std::move(ctx.types);
    return out;
}

float rounded(float value) {
    return std::round(value * 1000.0f) / 1000.0f;
}

Rect intersect_rect(Rect a, Rect b) {
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    return {x1, y1, std::max(0.0f, x2 - x1), std::max(0.0f, y2 - y1)};
}

choc::value::Value rect_json(Rect rect) {
    auto out = choc::value::createObject("");
    out.addMember("x", choc::value::createFloat64(rounded(rect.x)));
    out.addMember("y", choc::value::createFloat64(rounded(rect.y)));
    out.addMember("w", choc::value::createFloat64(rounded(rect.width)));
    out.addMember("h", choc::value::createFloat64(rounded(rect.height)));
    return out;
}

void append_node_snapshot(const View& view,
                          Rect parent_abs,
                          Rect inherited_clip,
                          const std::unordered_map<const View*, std::string>& types,
                          choc::value::Value& nodes,
                          int& paint_order) {
    const auto b = view.bounds();
    const Rect abs{parent_abs.x + b.x, parent_abs.y + b.y, b.width, b.height};
    const Rect clip = view.overflow() == View::Overflow::hidden
        ? intersect_rect(inherited_clip, abs)
        : inherited_clip;

    auto node = choc::value::createObject("");
    node.addMember("id", choc::value::createString(view.id()));
    auto type_it = types.find(&view);
    node.addMember("type", choc::value::createString(type_it == types.end() ? "View" : type_it->second));
    node.addMember("rect", rect_json(abs));

    auto z = choc::value::createObject("");
    z.addMember("paint", choc::value::createInt32(paint_order++));
    z.addMember("z_index", choc::value::createInt32(view.z_index()));
    node.addMember("z_order", z);

    auto clipping = choc::value::createObject("");
    clipping.addMember("rect", rect_json(clip));
    node.addMember("clipping", clipping);

    node.addMember("measured_text_boxes", choc::value::createEmptyArray());

    auto hit_regions = choc::value::createEmptyArray();
    if (view.visible() && view.hit_testable() && !abs.is_empty()) {
        auto hit = choc::value::createObject("");
        hit.addMember("rect", rect_json(abs));
        hit_regions.addArrayElement(hit);
    }
    node.addMember("hit_regions", hit_regions);
    nodes.addArrayElement(node);

    for (auto* child : view.sorted_children_by_z_index())
        append_node_snapshot(*child, abs, clip, types, nodes, paint_order);
}

choc::value::Value render_snapshot(const choc::value::ValueView& fixture) {
    auto built = build_tree(fixture);
    auto viewport = get_any(fixture, {"viewport"});
    const float width = viewport && viewport->isObject()
        ? static_cast<float>(get_number(*viewport, {"w", "width"}).value_or(0.0))
        : 0.0f;
    const float height = viewport && viewport->isObject()
        ? static_cast<float>(get_number(*viewport, {"h", "height"}).value_or(0.0))
        : 0.0f;
    if (width <= 0.0f || height <= 0.0f)
        throw std::runtime_error("fixture viewport must include positive w/h");

    built.root->set_bounds({0.0f, 0.0f, width, height});
    built.root->layout_children();

    auto out = choc::value::createObject("");
    const auto fixture_id = get_string(fixture, {"id"}).value_or("unknown");
    const auto surface = get_string(fixture, {"surface"}).value_or("yoga");
    out.addMember("schema_version", choc::value::createString("visual-layout-snapshot-v1"));
    out.addMember("surface", choc::value::createString(surface));
    out.addMember("fixture", choc::value::createString(fixture_id));
    if (auto slash = fixture_id.find('/'); slash != std::string::npos)
        out.addMember("entry", choc::value::createString(fixture_id.substr(slash + 1)));
    else
        out.addMember("entry", choc::value::createString(fixture_id));

    auto viewport_out = choc::value::createObject("");
    viewport_out.addMember("w", choc::value::createFloat64(width));
    viewport_out.addMember("h", choc::value::createFloat64(height));
    out.addMember("viewport", viewport_out);

    auto nodes = choc::value::createEmptyArray();
    int paint_order = 0;
    append_node_snapshot(*built.root, {0, 0, 0, 0}, {0, 0, width, height},
                         built.types, nodes, paint_order);
    out.addMember("nodes", nodes);
    return out;
}

std::string render_fixture_file(const fs::path& path) {
    auto fixture = choc::json::parse(read_file(path));
    return choc::json::toString(render_snapshot(fixture), true);
}

std::optional<choc::value::ValueView> find_node(const choc::value::ValueView& snapshot,
                                                std::string_view id) {
    if (!snapshot.isObject() || !snapshot.hasObjectMember("nodes") ||
        !snapshot["nodes"].isArray()) {
        return std::nullopt;
    }

    auto nodes = snapshot["nodes"];
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        auto node = nodes[i];
        if (node.isObject() && get_string(node, {"id"}).value_or("") == id) {
            return node;
        }
    }
    return std::nullopt;
}

int run_fixture_mode(const fs::path& fixture_path, const std::optional<fs::path>& out_path) {
    try {
        const auto rendered = render_fixture_file(fixture_path);
        if (out_path) {
            std::ofstream out(*out_path, std::ios::binary);
            if (!out) {
                std::cerr << "pulp-test-visual: could not write " << out_path->string() << "\n";
                return 2;
            }
            out << rendered << "\n";
        } else {
            std::cout << rendered << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "pulp-test-visual: " << e.what() << "\n";
        return 1;
    }
}

void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " --fixture <path> [--out <path>]\n"
              << "       " << argv0 << " --self-test [catch2 args...]\n";
}

}  // namespace

TEST_CASE("visual fixture renderer emits deterministic Yoga layout JSON",
          "[visual][layout][yoga]") {
    const auto fixture = choc::json::parse(R"JSON({
      "id": "yoga/self-test",
      "surface": "yoga",
      "viewport": { "w": 120, "h": 60 },
      "tree": {
        "id": "root",
        "style": { "direction": "row" },
        "children": [
          { "id": "left", "style": { "width": 40, "height": 20 } },
          { "id": "right", "style": { "flex_grow": 1, "height": 20 } }
        ]
      }
    })JSON");

    const auto snapshot = render_snapshot(fixture);
    const auto rendered = choc::json::toString(snapshot, true);
    const auto left = find_node(snapshot, "left");
    const auto right = find_node(snapshot, "right");

    REQUIRE(rendered.find("\"fixture\": \"yoga/self-test\"") != std::string::npos);
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(get_number((*left)["rect"], {"w"}).value_or(-1.0) == Catch::Approx(40.0));
    REQUIRE(get_number((*right)["rect"], {"w"}).value_or(-1.0) == Catch::Approx(80.0));
    REQUIRE(rendered.find("\"measured_text_boxes\": []") != std::string::npos);
    REQUIRE(rendered.find("\"hit_regions\"") != std::string::npos);
}

TEST_CASE("visual fixture renderer preserves z-index paint ordering",
          "[visual][layout][z-order]") {
    const auto fixture = choc::json::parse(R"JSON({
      "id": "yoga/z-order-self-test",
      "surface": "yoga",
      "viewport": { "w": 80, "h": 80 },
      "tree": {
        "id": "root",
        "children": [
          { "id": "back", "style": { "z_index": 0, "width": 10, "height": 10 } },
          { "id": "front", "style": { "z_index": 10, "width": 10, "height": 10 } }
        ]
      }
    })JSON");

    const auto rendered = choc::json::toString(render_snapshot(fixture), true);
    REQUIRE(rendered.find("\"id\": \"back\"") < rendered.find("\"id\": \"front\""));
    REQUIRE(rendered.find("\"z_index\": 10") != std::string::npos);
}

int main(int argc, char* argv[]) {
    std::optional<fs::path> fixture_path;
    std::optional<fs::path> out_path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--self-test") {
            std::vector<char*> catch_argv;
            catch_argv.push_back(argv[0]);
            for (int j = i + 1; j < argc; ++j) catch_argv.push_back(argv[j]);
            return Catch::Session().run(static_cast<int>(catch_argv.size()), catch_argv.data());
        }
        if (arg == "--fixture" && i + 1 < argc) {
            fixture_path = fs::path(argv[++i]);
            continue;
        }
        if (arg == "--out" && i + 1 < argc) {
            out_path = fs::path(argv[++i]);
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        std::cerr << "pulp-test-visual: unknown argument " << arg << "\n";
        print_usage(argv[0]);
        return 2;
    }

    if (!fixture_path) {
        print_usage(argv[0]);
        return 2;
    }

    return run_fixture_mode(*fixture_path, out_path);
}
