// SPDX-License-Identifier: MIT
#include "browser_capture_tree.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace pulp::import_design {
namespace {

using pulp::view::IRNode;

constexpr int kElementNode = 1;
constexpr int kTextNode = 3;

/// Elements whose pixels do not come from CSS at all.
///
/// `<canvas>` is imperative drawing; `<video>`/`<iframe>`/`<embed>`/`<object>`
/// host a separate document or decoder. `<svg>` is here for now because its
/// painted content is a shape tree the IR does not carry yet — it is the
/// largest single cause on the can't-draw list and is scoped as its own piece
/// of work, not something to half-do inside the tree walk.
bool is_capture_only_element(std::string_view tag) {
    static const std::unordered_set<std::string_view> kTags{
        "canvas", "svg", "video", "iframe", "embed", "object", "math",
    };
    return kTags.count(tag) != 0;
}

bool is_blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

/// A `background-image` that names an asset rather than painting a gradient.
/// `apply_computed_styles` has already made that split, so read the field it
/// decided rather than re-parsing the declaration and risking a different
/// answer from the one the node actually carries.
bool references_asset(const pulp::view::IRStyle& style) {
    return style.background_image.has_value() &&
           style.background_image->find("url(") != std::string::npos;
}

/// A readable, stable name. Reviewers and structural assertions both need to
/// see WHAT a node is, and `div` alone across two hundred nodes says nothing.
std::string describe(const CapturedStyleIndex& index,
                     const CapturedPaintNode& node) {
    if (node.node_type == kTextNode) return "#text";
    std::string name = node.tag_name;
    const auto id = index.attribute(node.node_index, "id");
    if (!id.empty()) return name + "#" + id;
    const auto classes = index.attribute(node.node_index, "class");
    if (classes.empty()) return name;
    // The first class is the identifying one in every design-system idiom we
    // import; appending all of them makes the name unreadable.
    const auto end = classes.find(' ');
    return name + "." + classes.substr(0, end);
}

}  // namespace

std::string_view to_string(PaintClass paint_class) {
    switch (paint_class) {
        case PaintClass::native: return "native";
        case PaintClass::image_asset: return "image-asset";
        case PaintClass::element_capture_fallback:
            return "element-capture-fallback";
    }
    return "native";
}

PaintedTreeCounts lower_painted_tree(const CapturedStyleIndex& index,
                                     double dx,
                                     double dy,
                                     IRNode& root) {
    PaintedTreeCounts counts;
    const auto painted = index.painted_nodes();
    counts.painted = static_cast<int>(painted.size());

    // "Is some ancestor of this node captured as one element?" — answered from
    // the DOCUMENT tree, never from what the walk has already visited. Paint
    // order is not document order, so a node inside an `<svg>` can be reached
    // before the `<svg>` itself; a memo seeded by visit order would record
    // "not under a capture" for that whole chain and then keep answering it.
    std::unordered_map<int, bool> under_capture_only_memo;
    const auto under_capture_only = [&](int node_index) {
        // Bounded by tree depth: `parent_of` returns -1 at the root and for any
        // out-of-range index, so a malformed forest terminates rather than
        // spinning. The chain is back-filled so each node is walked once.
        std::vector<int> chain;
        bool answer = false;
        int current = index.parent_of(node_index);
        while (current >= 0) {
            const auto seen = under_capture_only_memo.find(current);
            if (seen != under_capture_only_memo.end()) {
                answer = seen->second;
                break;
            }
            if (is_capture_only_element(index.tag_name(current))) {
                answer = true;
                break;
            }
            chain.push_back(current);
            current = index.parent_of(current);
        }
        for (const int node : chain) under_capture_only_memo[node] = answer;
        return answer;
    };

    for (const auto& node : painted) {
        if (node.node_type != kElementNode && node.node_type != kTextNode) {
            ++counts.skipped_non_visual;
            continue;
        }
        if (node.paint_order < 0) ++counts.missing_paint_order;

        const bool capture_only =
            node.node_type == kElementNode &&
            is_capture_only_element(node.tag_name);
        if (under_capture_only(node.node_index)) {
            // The captured ancestor covers this node's pixels already. Emitting
            // it too would draw the same content twice, once wrongly. A nested
            // `<svg>` inside an `<svg>` pools for the same reason.
            ++counts.pooled_into_fallback;
            continue;
        }

        if (node.bounds.width <= 0.0 || node.bounds.height <= 0.0) {
            ++counts.skipped_empty_box;
            continue;
        }
        const bool is_text = node.node_type == kTextNode;
        if (is_text && (node.text.empty() || is_blank(node.text))) {
            ++counts.skipped_blank_text;
            continue;
        }

        IRNode lowered;
        lowered.name = describe(index, node);
        CapturedBox box = node.bounds;
        const auto computed = index.styles_for_layout(node.layout_index);
        apply_computed_styles(computed, box, lowered.style,
                              is_text ? ComputedStyleScope::text_only
                                      : ComputedStyleScope::box_and_text);

        PaintClass paint_class = PaintClass::native;
        if (capture_only) {
            paint_class = PaintClass::element_capture_fallback;
            lowered.type = "frame";
            lowered.attributes["capture_fallback_element"] = node.tag_name;
        } else if (is_text) {
            lowered.type = "text";
            lowered.text_content = node.text;
        } else if (node.tag_name == "img") {
            paint_class = PaintClass::image_asset;
            lowered.type = "image";
            const auto src = index.attribute(node.node_index, "src");
            if (!src.empty()) lowered.attributes["src"] = src;
        } else if (references_asset(lowered.style)) {
            paint_class = PaintClass::image_asset;
            lowered.type = "frame";
        } else {
            lowered.type = "frame";
        }

        // Geometry LAST and unconditionally: Chrome already solved this box, so
        // it is the placement authority and no computed declaration may move
        // it. Sub-pixel values are Blink's 1/64px layout grid arriving as data;
        // they are consumed verbatim, never rounded to look tidy.
        lowered.style.position = "absolute";
        lowered.style.left = static_cast<float>(box.left + dx);
        lowered.style.top = static_cast<float>(box.top + dy);
        lowered.style.width = static_cast<float>(box.width);
        lowered.style.height = static_cast<float>(box.height);

        lowered.attributes["paint_class"] =
            std::string(to_string(paint_class));
        lowered.attributes["paint_order"] = std::to_string(node.paint_order);
        lowered.attributes["source_tag"] = node.tag_name;
        // Keyed on the layout index, which is the identity of the painted node
        // itself. Paint order is not unique — a tie would collapse two anchors
        // into one and the tweaks layer would apply a human edit to whichever
        // node happened to be found first.
        lowered.stable_anchor_id =
            "capture:paint:" + std::to_string(node.layout_index);
        lowered.anchor_strategy = "path";

        switch (paint_class) {
            case PaintClass::native: ++counts.native; break;
            case PaintClass::image_asset: ++counts.image_asset; break;
            case PaintClass::element_capture_fallback:
                ++counts.element_capture_fallback;
                break;
        }
        if (is_text) ++counts.text;
        ++counts.lowered;
        root.children.push_back(std::move(lowered));
    }
    return counts;
}

}  // namespace pulp::import_design
