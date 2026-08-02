// SPDX-License-Identifier: MIT
#include "browser_capture_styles.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace pulp::import_design {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string(text.substr(begin, end - begin));
}

/// CSS keywords that mean "this property contributes nothing". Recording them
/// would turn every unstyled element into a node carrying a dozen inert
/// declarations, which buries the handful that actually paint.
bool is_absent(const std::string& value) {
    return value.empty() || value == "none" || value == "normal" ||
           value == "auto" || value == "initial";
}

bool is_transparent(const std::string& value) {
    const auto compact = trim(value);
    return compact == "transparent" || compact == "rgba(0, 0, 0, 0)";
}

std::optional<double> parse_number(const std::string& value) {
    const auto compact = trim(value);
    if (compact.empty()) return std::nullopt;
    try {
        size_t consumed = 0;
        const double parsed = std::stod(compact, &consumed);
        if (consumed == 0) return std::nullopt;
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Resolve a CSS length to logical pixels. Percentages need the element's own
/// box, which is why `reference` is threaded through: `border-radius: 50%` on a
/// 68px knob face is 34px, and dropping it would square off every round control.
std::optional<float> parse_length(const std::string& value, double reference) {
    const auto compact = trim(value);
    if (compact.empty() || compact == "auto" || compact == "normal")
        return std::nullopt;
    const auto number = parse_number(compact);
    if (!number) return std::nullopt;
    if (compact.find('%') != std::string::npos)
        return static_cast<float>(*number * reference / 100.0);
    return static_cast<float>(*number);
}

/// Split on a delimiter that appears at paren depth zero. Both `box-shadow`
/// layers and gradient stops are comma-separated lists whose members contain
/// commas of their own, so a naive split shreds every colour function.
std::vector<std::string> split_top_level(const std::string& value, char delim) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (const char c : value) {
        if (c == '(') ++depth;
        else if (c == ')') depth = std::max(0, depth - 1);
        if (c == delim && depth == 0) {
            parts.push_back(trim(current));
            current.clear();
            continue;
        }
        current += c;
    }
    const auto tail = trim(current);
    if (!tail.empty()) parts.push_back(tail);
    return parts;
}

/// Whitespace tokens at paren depth zero, so `oklab(0.5 -0.07 0.04 / .26)`
/// stays one token instead of five.
std::vector<std::string> split_tokens(const std::string& value) {
    std::vector<std::string> tokens;
    int depth = 0;
    std::string current;
    for (const char c : value) {
        if (c == '(') ++depth;
        else if (c == ')') depth = std::max(0, depth - 1);
        if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current += c;
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

bool is_length_token(const std::string& token) {
    if (token.empty()) return false;
    const char first = token.front();
    if (!(std::isdigit(static_cast<unsigned char>(first)) || first == '-' ||
          first == '+' || first == '.')) {
        return false;
    }
    return parse_number(token).has_value();
}

bool starts_with(const std::string& value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

/// A `background-image` value that paints a gradient rather than referencing an
/// asset. Both live in the same CSS property but land on different IR fields.
bool is_gradient(const std::string& value) {
    static constexpr std::string_view kFunctions[] = {
        "linear-gradient", "radial-gradient", "conic-gradient",
        "repeating-linear-gradient", "repeating-radial-gradient",
        "repeating-conic-gradient",
    };
    for (const auto& function : kFunctions) {
        if (value.find(function) != std::string::npos) return true;
    }
    return false;
}

int json_int(const choc::value::ValueView& value, int fallback) {
    if (value.isInt32() || value.isInt64() || value.isFloat() ||
        value.isFloat32() || value.isFloat64()) {
        return static_cast<int>(value.getWithDefault<double>(fallback));
    }
    return fallback;
}

choc::value::ValueView member(const choc::value::ValueView& object,
                              const char* name) {
    if (!object.isObject() || !object.hasObjectMember(name))
        return choc::value::ValueView();
    return object[name];
}

}  // namespace

std::vector<std::string> split_css_list(const std::string& value) {
    return split_top_level(value, ',');
}

std::vector<pulp::view::IRBoxShadow> parse_box_shadow(
    const std::string& value) {
    std::vector<pulp::view::IRBoxShadow> layers;
    if (is_absent(trim(value))) return layers;

    for (const auto& raw_layer : split_top_level(value, ',')) {
        if (raw_layer.empty()) continue;
        pulp::view::IRBoxShadow layer;
        layer.raw = raw_layer;

        std::vector<float> lengths;
        std::string color;
        for (const auto& token : split_tokens(raw_layer)) {
            if (token == "inset") {
                layer.inset = true;
            } else if (is_length_token(token)) {
                // Shadow offsets are absolute lengths; a percentage is invalid
                // here, so no box reference is needed.
                if (const auto length = parse_length(token, 0.0))
                    lengths.push_back(*length);
            } else if (color.empty()) {
                color = token;
            }
        }
        if (lengths.size() > 0) layer.offset_x = lengths[0];
        if (lengths.size() > 1) layer.offset_y = lengths[1];
        if (lengths.size() > 2) layer.blur = lengths[2];
        if (lengths.size() > 3) layer.spread = lengths[3];
        layer.color = color;
        layers.push_back(std::move(layer));
    }
    return layers;
}

std::optional<CapturedStyleIndex> CapturedStyleIndex::load(
    const fs::path& snapshot_path) {
    std::error_code ec;
    if (!fs::is_regular_file(snapshot_path, ec)) return std::nullopt;

    std::ifstream input(snapshot_path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream bytes;
    bytes << input.rdbuf();

    choc::value::Value snapshot;
    try {
        snapshot = choc::json::parse(bytes.str());
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!snapshot.isObject()) return std::nullopt;

    CapturedStyleIndex index;

    // Without the request order the style rows cannot be read at all: entry N
    // is meaningless on its own. Refusing here is what keeps an older capture
    // from being decoded against a guessed property list.
    const auto names = member(snapshot, "computedStyleNames");
    if (!names.isArray() || names.size() == 0) return std::nullopt;
    for (uint32_t i = 0; i < names.size(); ++i) {
        const auto name = names[static_cast<int>(i)];
        index.property_names_.push_back(
            name.isString() ? std::string(name.getString()) : std::string());
    }

    const auto strings = member(snapshot, "strings");
    if (!strings.isArray()) return std::nullopt;
    index.strings_.reserve(strings.size());
    for (uint32_t i = 0; i < strings.size(); ++i) {
        const auto entry = strings[static_cast<int>(i)];
        index.strings_.push_back(
            entry.isString() ? std::string(entry.getString()) : std::string());
    }

    const auto documents = member(snapshot, "documents");
    if (!documents.isArray() || documents.size() == 0) return std::nullopt;
    const auto document = documents[0];
    const auto nodes = member(document, "nodes");
    const auto layout = member(document, "layout");
    if (!nodes.isObject() || !layout.isObject()) return std::nullopt;

    const auto backend_ids = member(nodes, "backendNodeId");
    const auto parents = member(nodes, "parentIndex");
    if (!backend_ids.isArray()) return std::nullopt;
    index.node_to_backend_.assign(backend_ids.size(), -1);
    for (uint32_t i = 0; i < backend_ids.size(); ++i) {
        const int backend = json_int(backend_ids[static_cast<int>(i)], -1);
        if (backend < 0) continue;
        index.backend_to_node_[backend] = static_cast<int>(i);
        index.node_to_backend_[i] = backend;
    }
    if (parents.isArray()) {
        index.parent_index_.reserve(parents.size());
        for (uint32_t i = 0; i < parents.size(); ++i)
            index.parent_index_.push_back(
                json_int(parents[static_cast<int>(i)], -1));
    }

    // Node identity. Whole-tree lowering needs to know WHAT each painted node
    // is — an element or a text run, and which tag — because that decides
    // whether it can be drawn natively at all.
    const auto node_types = member(nodes, "nodeType");
    const auto node_names = member(nodes, "nodeName");
    const auto node_attributes = member(nodes, "attributes");
    const auto node_count = backend_ids.size();
    index.node_type_.assign(node_count, 0);
    index.node_name_.assign(node_count, -1);
    index.node_attributes_.resize(node_count);
    for (uint32_t i = 0; i < node_count; ++i) {
        const auto at = static_cast<int>(i);
        if (node_types.isArray() && i < node_types.size())
            index.node_type_[i] = json_int(node_types[at], 0);
        if (node_names.isArray() && i < node_names.size())
            index.node_name_[i] = json_int(node_names[at], -1);
        if (!node_attributes.isArray() || i >= node_attributes.size())
            continue;
        const auto entry = node_attributes[at];
        if (!entry.isArray()) continue;
        auto& pairs = index.node_attributes_[i];
        pairs.reserve(entry.size());
        for (uint32_t j = 0; j < entry.size(); ++j)
            pairs.push_back(json_int(entry[static_cast<int>(j)], -1));
    }

    const auto node_index = member(layout, "nodeIndex");
    const auto styles = member(layout, "styles");
    const auto bounds = member(layout, "bounds");
    const auto texts = member(layout, "text");
    const auto paint_orders = member(layout, "paintOrders");
    if (!node_index.isArray() || !styles.isArray()) return std::nullopt;

    index.layout_to_node_.assign(node_index.size(), -1);
    index.layout_text_.assign(node_index.size(), -1);
    // Absent rather than zero when the capture did not request paint order:
    // zero is a legitimate order, so defaulting to it would silently reorder
    // the whole panel into document order while looking like real data.
    index.layout_paint_order_.assign(node_index.size(), -1);
    for (uint32_t i = 0; i < node_index.size(); ++i) {
        const auto at = static_cast<int>(i);
        const int node = json_int(node_index[at], -1);
        if (texts.isArray() && i < texts.size())
            index.layout_text_[i] = json_int(texts[at], -1);
        if (paint_orders.isArray() && i < paint_orders.size())
            index.layout_paint_order_[i] = json_int(paint_orders[at], -1);
        if (node < 0) continue;
        index.node_to_layout_[node] = at;
        index.layout_to_node_[i] = node;
    }

    index.style_rows_.reserve(styles.size());
    for (uint32_t i = 0; i < styles.size(); ++i) {
        std::vector<int> row;
        const auto entry = styles[static_cast<int>(i)];
        if (entry.isArray()) {
            row.reserve(entry.size());
            for (uint32_t j = 0; j < entry.size(); ++j)
                row.push_back(json_int(entry[static_cast<int>(j)], -1));
        }
        index.style_rows_.push_back(std::move(row));
    }

    index.layout_bounds_.resize(index.style_rows_.size());
    if (bounds.isArray()) {
        for (uint32_t i = 0;
             i < bounds.size() && i < index.layout_bounds_.size(); ++i) {
            const auto entry = bounds[static_cast<int>(i)];
            if (!entry.isArray() || entry.size() < 4) continue;
            CapturedBox box;
            box.left = entry[0].getWithDefault<double>(0.0);
            box.top = entry[1].getWithDefault<double>(0.0);
            box.width = entry[2].getWithDefault<double>(0.0);
            box.height = entry[3].getWithDefault<double>(0.0);
            index.layout_bounds_[i] = box;
        }
    }

    // Per-line text boxes. `includeDOMRects` is what makes Chrome emit these,
    // and a capture taken without it simply has no `textBoxes` — so their
    // absence is a capture-age question, not a malformed-snapshot one, and
    // leaves every node with an empty box list rather than failing the load.
    index.layout_text_boxes_.resize(index.style_rows_.size());
    const auto text_boxes = member(document, "textBoxes");
    if (text_boxes.isObject()) {
        const auto box_layout = member(text_boxes, "layoutIndex");
        const auto box_bounds = member(text_boxes, "bounds");
        const auto box_start = member(text_boxes, "start");
        const auto box_length = member(text_boxes, "length");
        if (box_layout.isArray() && box_bounds.isArray()) {
            for (uint32_t i = 0; i < box_layout.size(); ++i) {
                const auto at = static_cast<int>(i);
                const int layout_index = json_int(box_layout[at], -1);
                if (layout_index < 0 ||
                    layout_index >=
                        static_cast<int>(index.layout_text_boxes_.size())) {
                    continue;
                }
                if (i >= box_bounds.size()) break;
                const auto entry = box_bounds[at];
                if (!entry.isArray() || entry.size() < 4) continue;
                CapturedTextBox box;
                box.bounds.left = entry[0].getWithDefault<double>(0.0);
                box.bounds.top = entry[1].getWithDefault<double>(0.0);
                box.bounds.width = entry[2].getWithDefault<double>(0.0);
                box.bounds.height = entry[3].getWithDefault<double>(0.0);
                if (box_start.isArray() && i < box_start.size())
                    box.start = json_int(box_start[at], 0);
                if (box_length.isArray() && i < box_length.size())
                    box.length = json_int(box_length[at], 0);
                index.layout_text_boxes_[static_cast<size_t>(layout_index)]
                    .push_back(box);
            }
        }
    }

    // Which face Blink actually shaped each run with, from the sidecar beside
    // the snapshot. Optional by design: a capture taken before that sidecar
    // existed simply has no faces, and every consumer must already treat an
    // empty answer as unusable rather than as agreement.
    const auto fonts_path = snapshot_path.parent_path() / "platform-fonts.json";
    std::ifstream fonts_input(fonts_path, std::ios::binary);
    if (fonts_input) {
        std::ostringstream font_bytes;
        font_bytes << fonts_input.rdbuf();
        try {
            const auto report = choc::json::parse(font_bytes.str());
            const auto runs = member(report, "runs");
            if (runs.isArray()) {
                for (uint32_t i = 0; i < runs.size(); ++i) {
                    const auto run = runs[static_cast<int>(i)];
                    if (!run.isObject()) continue;
                    const int layout_index =
                        json_int(member(run, "layout_index"), -1);
                    if (layout_index < 0) continue;
                    const auto resolved = member(run, "resolved");
                    if (!resolved.isArray() || resolved.size() == 0) continue;
                    // The face that shaped the MOST glyphs, not the first one
                    // listed. Chrome does not order this array by primacy: in
                    // every mixed run across the corpus the single-glyph
                    // fallback is listed FIRST, so reading `resolved[0]` stores
                    // the face that drew one character as the basis for the
                    // whole paragraph.
                    //
                    // The consequence is not a near-miss, it is a permanent
                    // refusal. A Jost paragraph containing one `→` — a glyph
                    // Jost lacks — was stored with LucidaGrande as its basis;
                    // native resolution of Jost can never equal that, so the
                    // captured line breaking was rejected on every render, the
                    // run re-derived its own, and a run resuming mid-line after
                    // an inline span printed on top of its sibling. One arrow
                    // in a paragraph was enough, and arrows are everywhere in
                    // these UIs.
                    int best_at = 0;
                    int best_glyphs =
                        json_int(member(resolved[0], "glyph_count"), 0);
                    for (uint32_t r = 1; r < resolved.size(); ++r) {
                        const int glyphs = json_int(
                            member(resolved[static_cast<int>(r)],
                                   "glyph_count"),
                            0);
                        if (glyphs > best_glyphs) {
                            best_glyphs = glyphs;
                            best_at = static_cast<int>(r);
                        }
                    }
                    const auto face =
                        member(resolved[best_at], "post_script_name");
                    if (!face.isString()) continue;
                    index.layout_resolved_face_[layout_index] =
                        std::string(face.getString());
                }
            }
        } catch (const std::exception&) {
            // A malformed sidecar leaves the map empty, which is the same
            // state as no sidecar: nothing validates, everything reflows.
        }
    }

    if (index.style_rows_.empty()) return std::nullopt;
    return index;
}

std::string CapturedStyleIndex::resolved_face_for_layout(int layout_index) const {
    const auto it = layout_resolved_face_.find(layout_index);
    return it == layout_resolved_face_.end() ? std::string{} : it->second;
}

std::vector<CapturedTextBox> CapturedStyleIndex::text_boxes_for_layout(
    int layout_index) const {
    if (layout_index < 0 ||
        layout_index >= static_cast<int>(layout_text_boxes_.size())) {
        return {};
    }
    return layout_text_boxes_[static_cast<size_t>(layout_index)];
}

std::optional<int> CapturedStyleIndex::layout_for_node(int node_index) const {
    const auto it = node_to_layout_.find(node_index);
    if (it == node_to_layout_.end()) return std::nullopt;
    return it->second;
}

bool CapturedStyleIndex::is_descendant(int node_index,
                                       int ancestor_index) const {
    if (node_index == ancestor_index) return true;
    int current = node_index;
    // Bounded by the tree depth; parentIndex is a forest of -1-terminated
    // chains, and the guard keeps a malformed cycle from spinning here.
    for (size_t steps = 0; steps < parent_index_.size(); ++steps) {
        if (current < 0 || current >= static_cast<int>(parent_index_.size()))
            return false;
        current = parent_index_[static_cast<size_t>(current)];
        if (current < 0) return false;
        if (current == ancestor_index) return true;
    }
    return false;
}

std::optional<CapturedBox> CapturedStyleIndex::bounds_for(
    int backend_node_id) const {
    const auto node = backend_to_node_.find(backend_node_id);
    if (node == backend_to_node_.end()) return std::nullopt;
    const auto layout = layout_for_node(node->second);
    if (!layout) return std::nullopt;
    return layout_bounds_[static_cast<size_t>(*layout)];
}

std::string CapturedStyleIndex::string_at(int index) const {
    if (index < 0 || index >= static_cast<int>(strings_.size())) return {};
    return strings_[static_cast<size_t>(index)];
}

int CapturedStyleIndex::parent_of(int node_index) const {
    if (node_index < 0 ||
        node_index >= static_cast<int>(parent_index_.size())) {
        return -1;
    }
    return parent_index_[static_cast<size_t>(node_index)];
}

CapturedStyleIndex::InheritedTypeScale
CapturedStyleIndex::inherited_type_scale(int node_index) const {
    InheritedTypeScale result;
    // Bounded by the node count for the same reason `is_descendant` is: a
    // snapshot is a forest of -1-terminated chains only when it is well
    // formed, and an unbounded walk turns a malformed one into a hang rather
    // than a wrong answer.
    int cursor = parent_of(node_index);
    for (size_t steps = 0; steps < parent_index_.size() && cursor >= 0;
         ++steps, cursor = parent_of(cursor)) {
        const auto layout = layout_for_node(cursor);
        if (!layout) continue;
        const auto styles = styles_for_layout(*layout);
        const auto it = styles.find("transform");
        if (it == styles.end() || it->second.empty() || it->second == "none")
            continue;

        // Chrome serializes computed `transform` as a matrix, so the six
        // numbers are the whole answer — any other spelling (`matrix3d` above
        // all) is refused rather than guessed at.
        const auto& value = it->second;
        if (value.rfind("matrix(", 0) != 0 || value.back() != ')') {
            result.refused = value;
            return result;
        }
        std::vector<double> n;
        const std::string body = value.substr(7, value.size() - 8);
        size_t start = 0;
        while (start <= body.size()) {
            const auto comma = body.find(',', start);
            try {
                n.push_back(std::stod(body.substr(
                    start, comma == std::string::npos ? std::string::npos
                                                      : comma - start)));
            } catch (const std::exception&) {
                result.refused = value;
                return result;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        constexpr double kFlat = 1e-6;
        // b and c non-zero is a rotation or a skew; a != d is two axes; a
        // non-positive scale is a flip. A `font-size` is one positive scalar
        // and can express none of the three.
        if (n.size() != 6 || std::abs(n[1]) > kFlat || std::abs(n[2]) > kFlat ||
            std::abs(n[0] - n[3]) > kFlat || n[0] <= kFlat) {
            result.refused = value;
            return result;
        }
        result.scale *= n[0];
    }
    return result;
}

std::string CapturedStyleIndex::attribute(int node_index,
                                          std::string_view name) const {
    if (node_index < 0 ||
        node_index >= static_cast<int>(node_attributes_.size())) {
        return {};
    }
    const auto& pairs = node_attributes_[static_cast<size_t>(node_index)];
    // Flattened name/value string indices, so step two at a time and stop
    // before a trailing name with no value.
    for (size_t i = 0; i + 1 < pairs.size(); i += 2) {
        if (string_at(pairs[i]) == name) return string_at(pairs[i + 1]);
    }
    return {};
}

std::string CapturedStyleIndex::tag_name(int node_index) const {
    if (node_index < 0 || node_index >= static_cast<int>(node_name_.size()))
        return {};
    auto name = string_at(node_name_[static_cast<size_t>(node_index)]);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name;
}

std::map<std::string, std::string> CapturedStyleIndex::styles_for_layout(
    int layout_index) const {
    std::map<std::string, std::string> computed;
    if (layout_index < 0 ||
        layout_index >= static_cast<int>(style_rows_.size())) {
        return computed;
    }
    const auto& row = style_rows_[static_cast<size_t>(layout_index)];
    for (size_t i = 0; i < row.size() && i < property_names_.size(); ++i) {
        const auto value = string_at(row[i]);
        if (value.empty()) continue;
        computed[property_names_[i]] = value;
    }
    return computed;
}

bool CapturedStyleIndex::has_property(std::string_view name) const {
    return std::find(property_names_.begin(), property_names_.end(), name) !=
           property_names_.end();
}

std::map<std::string, std::string> CapturedStyleIndex::styles_for_node(
    int node_index) const {
    const auto layout = node_to_layout_.find(node_index);
    if (layout == node_to_layout_.end()) return {};
    return styles_for_layout(layout->second);
}

std::vector<CapturedPaintNode> CapturedStyleIndex::painted_nodes() const {
    std::vector<CapturedPaintNode> painted;
    painted.reserve(layout_to_node_.size());
    for (size_t layout = 0; layout < layout_to_node_.size(); ++layout) {
        const int node = layout_to_node_[layout];
        if (node < 0) continue;
        CapturedPaintNode entry;
        entry.layout_index = static_cast<int>(layout);
        entry.node_index = node;
        entry.paint_order = layout < layout_paint_order_.size()
                                ? layout_paint_order_[layout]
                                : -1;
        if (node < static_cast<int>(node_type_.size()))
            entry.node_type = node_type_[static_cast<size_t>(node)];
        entry.tag_name = tag_name(node);
        if (layout < layout_text_.size())
            entry.text = string_at(layout_text_[layout]);
        if (layout < layout_bounds_.size())
            entry.bounds = layout_bounds_[layout];
        entry.backend_node_id =
            node < static_cast<int>(node_to_backend_.size())
                ? node_to_backend_[static_cast<size_t>(node)]
                : -1;
        painted.push_back(std::move(entry));
    }
    // Chrome's paint order, ties broken by document order. The layout array is
    // already in document order, so a STABLE sort is what preserves it — and it
    // matters, because a paint order groups every node painted in one phase and
    // ties are the common case, not the exception.
    std::stable_sort(painted.begin(), painted.end(),
                     [](const CapturedPaintNode& a,
                        const CapturedPaintNode& b) {
                         return a.paint_order < b.paint_order;
                     });
    return painted;
}

std::map<std::string, std::string> CapturedStyleIndex::styles_for(
    int backend_node_id, const std::optional<CapturedBox>& paint_box) const {
    std::map<std::string, std::string> computed;
    const auto node = backend_to_node_.find(backend_node_id);
    if (node == backend_to_node_.end()) return computed;

    std::optional<int> chosen;
    if (paint_box) {
        // Sub-pixel layout means the paint box and the layout box agree to
        // within a fraction of a pixel rather than exactly.
        constexpr double kEpsilon = 1.0;
        for (size_t layout = 0; layout < layout_bounds_.size(); ++layout) {
            const auto& box = layout_bounds_[layout];
            if (std::fabs(box.left - paint_box->left) >= kEpsilon ||
                std::fabs(box.top - paint_box->top) >= kEpsilon ||
                std::fabs(box.width - paint_box->width) >= kEpsilon ||
                std::fabs(box.height - paint_box->height) >= kEpsilon) {
                continue;
            }
            const int owner = layout_to_node_[layout];
            if (owner < 0 || !is_descendant(owner, node->second)) continue;
            chosen = static_cast<int>(layout);
            break;
        }
    }
    if (!chosen) chosen = layout_for_node(node->second);
    if (!chosen) return computed;

    const auto& row = style_rows_[static_cast<size_t>(*chosen)];
    for (size_t i = 0; i < row.size() && i < property_names_.size(); ++i) {
        const int string_index = row[i];
        if (string_index < 0 ||
            string_index >= static_cast<int>(strings_.size())) {
            continue;
        }
        const auto& value = strings_[static_cast<size_t>(string_index)];
        if (value.empty()) continue;
        computed[property_names_[i]] = value;
    }
    return computed;
}

void apply_computed_styles(const std::map<std::string, std::string>& computed,
                           const std::optional<CapturedBox>& box,
                           pulp::view::IRStyle& style,
                           ComputedStyleScope scope,
                           double type_scale) {
    const auto lookup = [&computed](const char* name) -> std::string {
        const auto it = computed.find(name);
        return it == computed.end() ? std::string{} : it->second;
    };
    const auto set_string = [&](const char* name,
                                std::optional<std::string>& field) {
        const auto value = lookup(name);
        if (!is_absent(value)) field = value;
    };
    const double reference_width = box ? box->width : 0.0;
    const double reference_height = box ? box->height : 0.0;
    const double reference_min = std::min(reference_width, reference_height);
    const auto set_length = [&](const char* name, std::optional<float>& field,
                                double reference) {
        const auto value = lookup(name);
        if (is_absent(value)) return;
        if (const auto length = parse_length(value, reference)) field = *length;
    };

    const auto color = lookup("color");
    if (!is_absent(color)) style.color = color;

    // A type length is authored in the untransformed space, while the box it
    // will be drawn into is the post-transform one. Only lengths get the
    // factor: `font-weight` and the family are unitless, and a percentage
    // resolves against a reference that already carries the scale.
    const auto set_type_length = [&](const char* name,
                                     std::optional<float>& field,
                                     double reference) {
        set_length(name, field, reference);
        if (field && type_scale != 1.0)
            field = static_cast<float>(*field * type_scale);
    };

    // Typography. Inherited, so it is the half a text run legitimately owns.
    set_string("font-family", style.font_family);
    set_type_length("font-size", style.font_size, reference_height);
    const auto weight = lookup("font-weight");
    if (!is_absent(weight)) {
        if (const auto parsed = parse_number(weight))
            style.font_weight = static_cast<int>(*parsed);
    }
    set_string("font-style", style.font_style);
    const auto align = lookup("text-align");
    if (!is_absent(align) && align != "start") style.text_align = align;
    set_type_length("letter-spacing", style.letter_spacing, reference_width);
    set_type_length("line-height", style.line_height, reference_height);
    set_string("text-transform", style.text_transform);
    const auto decoration = lookup("text-decoration-line");
    if (!is_absent(decoration)) style.text_decoration = decoration;
    // Read with the raw lookup, NOT `set_string`.
    //
    // `is_absent` treats `normal` as "this property contributes nothing",
    // which is right for `text-transform` and `mix-blend-mode` and wrong for
    // exactly one property: `white-space: normal` is the value that turns
    // wrapping ON. Chrome serializes it on every wrapping node, so dropping it
    // left `white_space` unset — and the materializer cannot tell an unset
    // field from a capture that never carried the property, so it declined to
    // enable multi-line and every paragraph drew its first line and dropped
    // the rest. A gate that cannot fire, reported as if it had been evaluated.
    const auto white_space = lookup("white-space");
    if (!white_space.empty()) style.white_space = white_space;

    if (scope == ComputedStyleScope::text_only) return;

    // Fills. `background-image` carries both gradients and asset references;
    // they land on different IR fields because only one of them is paintable
    // without resolving an asset.
    const auto background_color = lookup("background-color");
    if (!is_absent(background_color) && !is_transparent(background_color))
        style.background_color = background_color;
    const auto background_image = lookup("background-image");
    if (!is_absent(background_image)) {
        if (is_gradient(background_image))
            style.background_gradient = background_image;
        else
            style.background_image = background_image;
        // Tiling is part of the fill, not a decoration: the standard CSS grid
        // idiom is one gradient with a hard stop repeated at a fixed size, so a
        // fill recorded without its size and repeat paints a single 1px line
        // where the design has a grid.
        set_string("background-size", style.background_size);
        set_string("background-repeat", style.background_repeat);
    }

    const auto opacity = lookup("opacity");
    if (!opacity.empty()) {
        if (const auto parsed = parse_number(opacity)) {
            if (*parsed < 1.0) style.opacity = static_cast<float>(*parsed);
        }
    }

    const auto blend = lookup("mix-blend-mode");
    if (!is_absent(blend)) style.mix_blend_mode = blend;

    // Borders. Chrome resolves each side independently even when the author
    // wrote the shorthand, so the per-side fields are the faithful record; the
    // uniform fields stay in sync for consumers that only read those.
    //
    // A side's colour is recorded only when that side actually has width.
    // Computed style reports a colour for every element whether or not a border
    // is drawn (it defaults to `color`), so recording it unconditionally would
    // hand every control four border colours it never had — and any consumer
    // that paints from `border_color` would draw a border the design does not.
    set_length("border-top-width", style.border_top_width, reference_width);
    set_length("border-right-width", style.border_right_width,
               reference_width);
    set_length("border-bottom-width", style.border_bottom_width,
               reference_width);
    set_length("border-left-width", style.border_left_width, reference_width);
    const auto set_side_color = [&](const char* name,
                                    const std::optional<float>& width,
                                    std::optional<std::string>& field) {
        if (!width || *width <= 0.0f) return;
        set_string(name, field);
    };
    set_side_color("border-top-color", style.border_top_width,
                   style.border_top_color);
    set_side_color("border-right-color", style.border_right_width,
                   style.border_right_color);
    set_side_color("border-bottom-color", style.border_bottom_width,
                   style.border_bottom_color);
    set_side_color("border-left-color", style.border_left_width,
                   style.border_left_color);
    // A border's style has to come from a side that actually HAS a border.
    // Reading only the top edge lost `border-left: 1px dashed` entirely: the
    // top edge has no width, so nothing was recorded and the left edge painted
    // solid. The capture protocol records all four edges for this reason.
    const auto side_style = [&](const char* name,
                                const std::optional<float>& width)
        -> std::optional<std::string> {
        if (!width || *width <= 0.0f) return std::nullopt;
        std::optional<std::string> out;
        set_string(name, out);
        return out;
    };
    const std::optional<std::string> edge_styles[4] = {
        side_style("border-top-style", style.border_top_width),
        side_style("border-right-style", style.border_right_width),
        side_style("border-bottom-style", style.border_bottom_width),
        side_style("border-left-style", style.border_left_width),
    };
    // `border_style` is one slot, so a box whose edges disagree keeps the first
    // edge that has one. That is a known narrowing, not a silent one: it is
    // still better than dropping the only style a one-edge border has.
    bool uniform_style = true;
    for (const auto& edge : edge_styles) {
        if (!edge) continue;
        if (!style.border_style) style.border_style = edge;
        else if (*style.border_style != *edge) uniform_style = false;
    }

    const bool uniform_color =
        style.border_top_color && style.border_right_color &&
        style.border_bottom_color && style.border_left_color &&
        *style.border_top_color == *style.border_right_color &&
        *style.border_top_color == *style.border_bottom_color &&
        *style.border_top_color == *style.border_left_color;
    if (uniform_color) style.border_color = style.border_top_color;
    const bool uniform_width =
        style.border_top_width && style.border_right_width &&
        style.border_bottom_width && style.border_left_width &&
        *style.border_top_width == *style.border_right_width &&
        *style.border_top_width == *style.border_bottom_width &&
        *style.border_top_width == *style.border_left_width;
    if (uniform_width) style.border_width = style.border_top_width;

    // A border only reads as one when it has a width, a style, and a colour;
    // Chrome reports a colour for every element whether or not one is drawn.
    if (uniform_width && uniform_color && uniform_style && style.border_style &&
        *style.border_width > 0.0f) {
        std::ostringstream shorthand;
        shorthand << *style.border_width << "px " << *style.border_style << ' '
                  << *style.border_color;
        style.border = shorthand.str();
    }

    const auto radius = lookup("border-radius");
    if (!is_absent(radius)) {
        // Expand the shorthand to all four corners. Taking only the first
        // collapses `12px 12px 0 0` — a card whose media area rounds at the top
        // and squares off where it meets its caption — into one value, so the
        // background paints a plain rectangle inside a rounded border.
        //
        // A second axis after a slash makes the corners elliptical. Nothing
        // downstream carries two radii per corner, so an elliptical value keeps
        // only its horizontal axis rather than being dropped: a wrong-but-round
        // corner is closer than a square one, and the alternative silently
        // discards the whole declaration.
        const auto horizontal = split_top_level(radius, '/').front();
        const auto corners = split_tokens(horizontal);
        const auto corner_at = [&](std::size_t i) -> std::optional<float> {
            if (i >= corners.size()) return std::nullopt;
            return parse_length(corners[i], reference_min);
        };
        // CSS box-corner shorthand: 1 value sets all four; 2 sets TL/BR then
        // TR/BL; 3 sets TL, TR/BL, BR; 4 sets TL TR BR BL, clockwise from
        // top-left.
        std::optional<float> tl, tr, br, bl;
        switch (corners.size()) {
            case 0: break;
            case 1: tl = tr = br = bl = corner_at(0); break;
            case 2: tl = br = corner_at(0); tr = bl = corner_at(1); break;
            case 3:
                tl = corner_at(0);
                tr = bl = corner_at(1);
                br = corner_at(2);
                break;
            default:
                tl = corner_at(0);
                tr = corner_at(1);
                br = corner_at(2);
                bl = corner_at(3);
                break;
        }
        if (tl) style.border_top_left_radius = *tl;
        if (tr) style.border_top_right_radius = *tr;
        if (br) style.border_bottom_right_radius = *br;
        if (bl) style.border_bottom_left_radius = *bl;
        // The single-radius slot stays populated for consumers that read only
        // it, and only when every corner agrees — a uniform value it can
        // represent without lying about the other three.
        if (tl && tr && br && bl && *tl == *tr && *tr == *br && *br == *bl) {
            style.border_radius = *tl;
        }
    }

    const auto shadows = parse_box_shadow(lookup("box-shadow"));
    if (!shadows.empty()) style.box_shadow = shadows;

    set_string("filter", style.filter);
    set_string("backdrop-filter", style.backdrop_filter);
    set_string("clip-path", style.clip_path);
    set_string("mask-image", style.mask_image);

    set_string("cursor", style.cursor);

    const auto overflow = lookup("overflow");
    if (!is_absent(overflow) && overflow != "visible")
        style.overflow = overflow;

    // `transform` is deliberately NOT carried. The paint box this node is
    // placed by is already the transformed rectangle, so re-applying the
    // matrix would rotate or scale the control a second time — off its own
    // artwork. Recovering an authored rotation means separating the untransformed
    // box from the painted one, which the capture does not report today.
}

}  // namespace pulp::import_design
