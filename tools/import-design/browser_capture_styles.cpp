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
    for (uint32_t i = 0; i < backend_ids.size(); ++i) {
        const int backend = json_int(backend_ids[static_cast<int>(i)], -1);
        if (backend >= 0) index.backend_to_node_[backend] =
            static_cast<int>(i);
    }
    if (parents.isArray()) {
        index.parent_index_.reserve(parents.size());
        for (uint32_t i = 0; i < parents.size(); ++i)
            index.parent_index_.push_back(
                json_int(parents[static_cast<int>(i)], -1));
    }

    const auto node_index = member(layout, "nodeIndex");
    const auto styles = member(layout, "styles");
    const auto bounds = member(layout, "bounds");
    if (!node_index.isArray() || !styles.isArray()) return std::nullopt;

    index.layout_to_node_.assign(node_index.size(), -1);
    for (uint32_t i = 0; i < node_index.size(); ++i) {
        const int node = json_int(node_index[static_cast<int>(i)], -1);
        if (node < 0) continue;
        index.node_to_layout_[node] = static_cast<int>(i);
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

    if (index.style_rows_.empty()) return std::nullopt;
    return index;
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
                           pulp::view::IRStyle& style) {
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
    }

    const auto color = lookup("color");
    if (!is_absent(color)) style.color = color;

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
    if (style.border_top_width && *style.border_top_width > 0.0f)
        set_string("border-top-style", style.border_style);

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
    if (uniform_width && uniform_color && style.border_style &&
        *style.border_width > 0.0f) {
        std::ostringstream shorthand;
        shorthand << *style.border_width << "px " << *style.border_style << ' '
                  << *style.border_color;
        style.border = shorthand.str();
    }

    const auto radius = lookup("border-radius");
    if (!is_absent(radius)) {
        // The shorthand may carry up to four corners and an optional second
        // axis after a slash. The IR's single radius takes the first corner;
        // per-corner fidelity is a separate field set no source populates yet.
        const auto horizontal = split_top_level(radius, '/').front();
        const auto corners = split_tokens(horizontal);
        if (!corners.empty()) {
            if (const auto length = parse_length(corners.front(),
                                                 reference_min)) {
                style.border_radius = *length;
            }
        }
    }

    const auto shadows = parse_box_shadow(lookup("box-shadow"));
    if (!shadows.empty()) style.box_shadow = shadows;

    set_string("filter", style.filter);
    set_string("backdrop-filter", style.backdrop_filter);
    set_string("clip-path", style.clip_path);
    set_string("mask-image", style.mask_image);

    // Typography.
    set_string("font-family", style.font_family);
    set_length("font-size", style.font_size, reference_height);
    const auto weight = lookup("font-weight");
    if (!is_absent(weight)) {
        if (const auto parsed = parse_number(weight))
            style.font_weight = static_cast<int>(*parsed);
    }
    set_string("font-style", style.font_style);
    const auto align = lookup("text-align");
    if (!is_absent(align) && align != "start") style.text_align = align;
    set_length("letter-spacing", style.letter_spacing, reference_width);
    set_length("line-height", style.line_height, reference_height);
    set_string("text-transform", style.text_transform);
    const auto decoration = lookup("text-decoration-line");
    if (!is_absent(decoration)) style.text_decoration = decoration;
    set_string("white-space", style.white_space);
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
