#include <pulp/view/layout_snapshot.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widgets.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace pulp::view {
namespace {

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

const char* overflow_name(View::Overflow overflow) {
    switch (overflow) {
        case View::Overflow::hidden: return "hidden";
        case View::Overflow::visible: return "visible";
        case View::Overflow::scroll: return "scroll";
    }
    return "visible";
}

std::string snapshot_kind(const View& view) {
    if (dynamic_cast<const TextEditor*>(&view)) return "TextEditor";
    if (dynamic_cast<const TextButton*>(&view)) return "TextButton";
    if (dynamic_cast<const HyperlinkButton*>(&view)) return "HyperlinkButton";
    return ViewInspector::type_name(view);
}

void add_text_box(choc::value::Value& boxes,
                  const std::string& text,
                  const Rect& abs,
                  float measured_width,
                  float measured_height) {
    if (text.empty()) return;

    auto box = choc::value::createObject("");
    box.addMember("text", choc::value::createString(text));
    auto rect = choc::value::createObject("");
    rect.addMember("x", choc::value::createFloat64(rounded(abs.x)));
    rect.addMember("y", choc::value::createFloat64(rounded(abs.y)));
    rect.addMember("w", choc::value::createFloat64(rounded(measured_width)));
    rect.addMember("h", choc::value::createFloat64(rounded(measured_height)));
    box.addMember("rect", rect);
    boxes.addArrayElement(box);
}

choc::value::Value measured_text_boxes_json(const View& view, const Rect& abs) {
    auto boxes = choc::value::createEmptyArray();

    if (auto* label = dynamic_cast<const Label*>(&view)) {
        const float width = label->intrinsic_width();
        const float height = label->measured_height(abs.width > 0.0f ? abs.width : width);
        add_text_box(boxes, label->text(), abs, width, height);
    } else if (auto* editor = dynamic_cast<const TextEditor*>(&view)) {
        add_text_box(boxes, editor->text(), abs, abs.width, abs.height);
    } else if (auto* button = dynamic_cast<const TextButton*>(&view)) {
        add_text_box(boxes, button->label(), abs, abs.width, abs.height);
    } else if (auto* link = dynamic_cast<const HyperlinkButton*>(&view)) {
        add_text_box(boxes, link->text(), abs, abs.width, abs.height);
    }

    return boxes;
}

void append_node_snapshot(const View& view,
                          Rect parent_abs,
                          Rect inherited_clip,
                          choc::value::Value& nodes,
                          int& paint_order) {
    const auto b = view.bounds();
    const Rect abs{parent_abs.x + b.x, parent_abs.y + b.y, b.width, b.height};
    const Rect clip = view.overflow() == View::Overflow::hidden ||
                      view.overflow() == View::Overflow::scroll
        ? intersect_rect(inherited_clip, abs)
        : inherited_clip;

    auto node = choc::value::createObject("");
    node.addMember("id", choc::value::createString(view.id()));
    const auto kind = snapshot_kind(view);
    node.addMember("kind", choc::value::createString(kind));
    node.addMember("type", choc::value::createString(kind));  // visual-harness compatibility
    node.addMember("rect", rect_json(abs));
    node.addMember("visible", choc::value::createBool(view.visible()));
    node.addMember("overflow", choc::value::createString(overflow_name(view.overflow())));

    auto z = choc::value::createObject("");
    z.addMember("paint", choc::value::createInt32(paint_order++));
    z.addMember("z_index", choc::value::createInt32(view.z_index()));
    node.addMember("z_order", z);

    auto clipping = choc::value::createObject("");
    // This is the clip the node imposes on descendants, not the inherited clip
    // that constrains the node's own pixels.
    clipping.addMember("rect", rect_json(clip));
    node.addMember("clipping", clipping);

    node.addMember("measured_text_boxes", measured_text_boxes_json(view, abs));

    auto hit_regions = choc::value::createEmptyArray();
    if (view.visible() && view.hit_testable() && !abs.is_empty()) {
        auto hit = choc::value::createObject("");
        hit.addMember("rect", rect_json(abs));
        hit_regions.addArrayElement(hit);
    }
    node.addMember("hit_regions", hit_regions);
    nodes.addArrayElement(node);

    for (auto* child : view.sorted_children_by_z_index())
        append_node_snapshot(*child, abs, clip, nodes, paint_order);
}

bool is_number(const choc::value::ValueView& value) {
    return value.isInt32() || value.isInt64() || value.isFloat32() || value.isFloat64();
}

double number_value(const choc::value::ValueView& value, double fallback = 0.0) {
    if (value.isInt32() || value.isInt64()) return static_cast<double>(value.getInt64());
    if (value.isFloat32() || value.isFloat64()) return value.getFloat64();
    return fallback;
}

std::string string_value(const choc::value::ValueView& obj, const char* key) {
    if (!obj.isObject() || !obj.hasObjectMember(key) || !obj[key].isString()) return {};
    return obj[key].getWithDefault<std::string>("");
}

std::optional<std::string> string_member(const choc::value::ValueView& obj, const char* key) {
    if (!obj.isObject() || !obj.hasObjectMember(key) || !obj[key].isString()) return std::nullopt;
    return obj[key].getWithDefault<std::string>("");
}

std::optional<choc::value::ValueView> member(const choc::value::ValueView& obj, const char* key) {
    if (!obj.isObject() || !obj.hasObjectMember(key)) return std::nullopt;
    return obj[key];
}

std::optional<double> number_member(const choc::value::ValueView& obj, const char* key) {
    auto value = member(obj, key);
    if (!value || !is_number(*value)) return std::nullopt;
    return number_value(*value);
}

std::optional<bool> bool_member(const choc::value::ValueView& obj, const char* key) {
    auto value = member(obj, key);
    if (!value || !value->isBool()) return std::nullopt;
    return value->getWithDefault<bool>(false);
}

void add_diff(LayoutTreeDiff* diff, std::string message) {
    if (diff) diff->messages.push_back(std::move(message));
}

std::string path_for_node(uint32_t index, const choc::value::ValueView& node) {
    std::ostringstream out;
    out << "nodes[" << index << "]";
    auto id = string_value(node, "id");
    if (!id.empty()) out << " #" << id;
    return out.str();
}

bool compare_string_field(const choc::value::ValueView& actual,
                          const choc::value::ValueView& expected,
                          const char* field,
                          const std::string& path,
                          LayoutTreeDiff* diff) {
    const auto a = string_member(actual, field);
    const auto e = string_member(expected, field);
    if (a == e) return true;

    add_diff(diff,
             path + "." + field + " expected '" + (e ? *e : "<missing>") +
                 "' got '" + (a ? *a : "<missing>") + "'");
    return false;
}

bool compare_bool_field(const choc::value::ValueView& actual,
                        const choc::value::ValueView& expected,
                        const char* field,
                        const std::string& path,
                        LayoutTreeDiff* diff) {
    const auto a = bool_member(actual, field);
    const auto e = bool_member(expected, field);
    if (a == e) return true;

    add_diff(diff, path + "." + field + " expected " +
                       (e ? (*e ? "true" : "false") : "<missing>") +
                       " got " + (a ? (*a ? "true" : "false") : "<missing>"));
    return false;
}

bool compare_rect(const choc::value::ValueView& actual,
                  const choc::value::ValueView& expected,
                  float tolerance,
                  const std::string& path,
                  LayoutTreeDiff* diff) {
    bool ok = true;
    for (auto* key : {"x", "y", "w", "h"}) {
        const auto a = number_member(actual, key);
        const auto e = number_member(expected, key);
        if (!a || !e || std::fabs(*a - *e) > static_cast<double>(tolerance)) {
            std::ostringstream msg;
            msg << path << "." << key << " expected "
                << (e ? std::to_string(*e) : "<missing>") << " got "
                << (a ? std::to_string(*a) : "<missing>")
                << " (tolerance " << tolerance << ")";
            add_diff(diff, msg.str());
            ok = false;
        }
    }
    return ok;
}

bool compare_z_order(const choc::value::ValueView& actual,
                     const choc::value::ValueView& expected,
                     const std::string& path,
                     LayoutTreeDiff* diff) {
    bool ok = true;
    for (auto* key : {"paint", "z_index"}) {
        const auto a = number_member(actual, key);
        const auto e = number_member(expected, key);
        if (!a || !e || static_cast<int>(*a) != static_cast<int>(*e)) {
            add_diff(diff, path + "." + key + " mismatch");
            ok = false;
        }
    }
    return ok;
}

bool compare_text_boxes(const choc::value::ValueView& actual,
                        const choc::value::ValueView& expected,
                        float tolerance,
                        const std::string& path,
                        LayoutTreeDiff* diff) {
    if (!actual.isArray() || !expected.isArray()) {
        add_diff(diff, path + " text boxes missing array");
        return false;
    }
    if (actual.size() != expected.size()) {
        add_diff(diff, path + " text box count mismatch");
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0; i < actual.size(); ++i) {
        const auto box_path = path + ".measured_text_boxes[" + std::to_string(i) + "]";
        ok = compare_string_field(actual[i], expected[i], "text", box_path, diff) && ok;
        auto actual_rect = member(actual[i], "rect");
        auto expected_rect = member(expected[i], "rect");
        if (!actual_rect || !expected_rect) {
            add_diff(diff, box_path + ".rect missing");
            ok = false;
            continue;
        }
        ok = compare_rect(*actual_rect, *expected_rect, tolerance, box_path + ".rect", diff) && ok;
    }
    return ok;
}

std::optional<choc::value::Value> parse_snapshot(std::string_view json, LayoutTreeDiff* diff, const char* label) {
    try {
        return choc::json::parse(std::string(json));
    } catch (const std::exception& e) {
        add_diff(diff, std::string(label) + " JSON parse failed: " + e.what());
        return std::nullopt;
    } catch (...) {
        add_diff(diff, std::string(label) + " JSON parse failed");
        return std::nullopt;
    }
}

} // namespace

std::string dump_layout_tree(const View& root, const LayoutTreeSnapshotOptions& options) {
    auto out = choc::value::createObject("");
    out.addMember("schema_version", choc::value::createString("visual-layout-snapshot-v1"));
    out.addMember("surface", choc::value::createString(options.surface.empty() ? "view" : options.surface));
    if (!options.fixture.empty()) out.addMember("fixture", choc::value::createString(options.fixture));

    if (options.viewport_width > 0.0f || options.viewport_height > 0.0f) {
        auto viewport = choc::value::createObject("");
        viewport.addMember("w", choc::value::createFloat64(rounded(options.viewport_width)));
        viewport.addMember("h", choc::value::createFloat64(rounded(options.viewport_height)));
        out.addMember("viewport", viewport);
    }

    auto nodes = choc::value::createEmptyArray();
    int paint_order = 0;
    const Rect root_clip{0.0f, 0.0f,
                         options.viewport_width > 0.0f ? options.viewport_width : root.bounds().width,
                         options.viewport_height > 0.0f ? options.viewport_height : root.bounds().height};
    append_node_snapshot(root, {0, 0, 0, 0}, root_clip, nodes, paint_order);
    out.addMember("nodes", nodes);
    return choc::json::toString(out, true);
}

bool layout_tree_snapshots_equivalent(std::string_view actual_json,
                                      std::string_view expected_json,
                                      const LayoutTreeTolerance& tolerance,
                                      LayoutTreeDiff* diff) {
    LayoutTreeDiff local;
    if (!diff) diff = &local;

    auto actual_snapshot = parse_snapshot(actual_json, diff, "actual");
    auto expected_snapshot = parse_snapshot(expected_json, diff, "expected");
    if (!actual_snapshot || !expected_snapshot) return false;

    auto actual_nodes = member(*actual_snapshot, "nodes");
    auto expected_nodes = member(*expected_snapshot, "nodes");
    if (!actual_nodes || !expected_nodes || !actual_nodes->isArray() || !expected_nodes->isArray()) {
        add_diff(diff, "snapshot nodes missing array");
        return false;
    }

    bool ok = true;
    if (actual_nodes->size() != expected_nodes->size()) {
        add_diff(diff, "node count expected " + std::to_string(expected_nodes->size()) +
                       " got " + std::to_string(actual_nodes->size()));
        ok = false;
    }

    const auto count = std::min(actual_nodes->size(), expected_nodes->size());
    for (uint32_t i = 0; i < count; ++i) {
        const auto path = path_for_node(i, (*expected_nodes)[i]);
        ok = compare_string_field((*actual_nodes)[i], (*expected_nodes)[i], "id", path, diff) && ok;
        ok = compare_string_field((*actual_nodes)[i], (*expected_nodes)[i], "kind", path, diff) && ok;
        ok = compare_bool_field((*actual_nodes)[i], (*expected_nodes)[i], "visible", path, diff) && ok;
        ok = compare_string_field((*actual_nodes)[i], (*expected_nodes)[i], "overflow", path, diff) && ok;

        auto actual_rect = member((*actual_nodes)[i], "rect");
        auto expected_rect = member((*expected_nodes)[i], "rect");
        if (actual_rect && expected_rect) {
            ok = compare_rect(*actual_rect, *expected_rect,
                              tolerance.numeric_bounds_px, path + ".rect", diff) && ok;
        } else {
            add_diff(diff, path + ".rect missing");
            ok = false;
        }

        auto actual_z = member((*actual_nodes)[i], "z_order");
        auto expected_z = member((*expected_nodes)[i], "z_order");
        if (actual_z && expected_z) {
            ok = compare_z_order(*actual_z, *expected_z, path + ".z_order", diff) && ok;
        } else {
            add_diff(diff, path + ".z_order missing");
            ok = false;
        }

        auto actual_clip = member((*actual_nodes)[i], "clipping");
        auto expected_clip = member((*expected_nodes)[i], "clipping");
        auto actual_clip_rect = actual_clip ? member(*actual_clip, "rect") : std::nullopt;
        auto expected_clip_rect = expected_clip ? member(*expected_clip, "rect") : std::nullopt;
        if (actual_clip_rect && expected_clip_rect) {
            ok = compare_rect(*actual_clip_rect, *expected_clip_rect,
                              tolerance.numeric_bounds_px, path + ".clipping.rect", diff) && ok;
        } else {
            add_diff(diff, path + ".clipping.rect missing");
            ok = false;
        }

        auto actual_boxes = member((*actual_nodes)[i], "measured_text_boxes");
        auto expected_boxes = member((*expected_nodes)[i], "measured_text_boxes");
        if (actual_boxes && expected_boxes) {
            ok = compare_text_boxes(*actual_boxes, *expected_boxes,
                                    tolerance.text_box_px, path, diff) && ok;
        } else {
            add_diff(diff, path + ".measured_text_boxes missing");
            ok = false;
        }
    }

    return ok;
}

} // namespace pulp::view
