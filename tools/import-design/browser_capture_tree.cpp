// SPDX-License-Identifier: MIT
#include "browser_capture_tree.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

/// A fill that needs a decoded raster rather than a paintable gradient.
///
/// `apply_computed_styles` has already split gradients out, so ANY surviving
/// `background_image` needs an asset — testing for `url(` specifically would
/// classify `image-set(...)` and `-webkit-image-set(...)` as `native` and then
/// draw nothing, which is the one answer the census must never give.
bool references_asset(const pulp::view::IRStyle& style) {
    return style.background_image.has_value() &&
           !style.background_image->empty();
}

/// A readable, stable name. Reviewers and structural assertions both need to
/// see WHAT a node is, and `div` alone across two hundred nodes says nothing.
///
/// Also the signature half of a stable anchor's path segment, so one function
/// owns "what identifies this node" for both the human and the machine reader.
std::string describe(const CapturedStyleIndex& index, int node_index) {
    const int type = index.node_type_of(node_index);
    // The document node, the doctype, comments: nothing an edit can be anchored
    // to, and an empty signature keeps them from sharing a sibling tally with
    // the element they are named after.
    if (type != kElementNode && type != kTextNode) return {};
    if (type == kTextNode) return "#text";
    std::string name = index.tag_name(node_index);
    if (name.empty()) return name;
    const auto id = index.attribute(node_index, "id");
    if (!id.empty()) return name + "#" + id;
    const auto classes = index.attribute(node_index, "class");
    if (classes.empty()) return name;
    // The first class is the identifying one in every design-system idiom we
    // import; appending all of them makes the name unreadable.
    const auto end = classes.find(' ');
    return name + "." + classes.substr(0, end);
}

/// Ordinal of every node among its same-signature siblings.
///
/// Computed over the WHOLE document rather than over the painted set: an
/// anchor that counted only painted siblings would move whenever a sibling was
/// hidden, which is a change to the page's state and not to its structure.
std::vector<int> sibling_ordinals(const CapturedStyleIndex& index) {
    const int count = index.node_count();
    std::vector<int> ordinal(static_cast<size_t>(std::max(count, 0)), 0);
    // DOMSnapshot lists nodes in document order, so a single forward pass
    // visits every parent's children in order and the running tally per
    // (parent, signature) is the ordinal.
    std::map<std::pair<int, std::string>, int> seen;
    for (int node = 0; node < count; ++node) {
        const auto key = std::make_pair(index.parent_of(node),
                                        describe(index, node));
        ordinal[static_cast<size_t>(node)] = seen[key]++;
    }
    return ordinal;
}

/// A DOM path an edit can be stored against and re-found after a re-capture.
///
/// The old key was the layout index, which is a position in whatever order the
/// capture happened to serialize — it identifies the node only within one
/// capture, which is the one job an anchor has to do ACROSS captures. A path of
/// `tag#id` / `tag.class` segments with sibling ordinals is derived from the
/// document's own structure, so the same design yields the same anchor however
/// it was captured.
std::string anchor_path(const CapturedStyleIndex& index,
                        const std::vector<int>& ordinal,
                        int node_index) {
    std::vector<std::string> segments;
    for (int node = node_index; node >= 0; node = index.parent_of(node)) {
        auto key = describe(index, node);
        if (key.empty()) continue;
        const int position =
            node < static_cast<int>(ordinal.size())
                ? ordinal[static_cast<size_t>(node)]
                : 0;
        segments.push_back(key + "[" + std::to_string(position) + "]");
    }
    std::string path = "capture:";
    for (auto segment = segments.rbegin(); segment != segments.rend();
         ++segment) {
        path += *segment;
        if (segment + 1 != segments.rend()) path += '/';
    }
    return path;
}

/// One node that survived filtering, before it is placed in the tree.
struct LoweredNode {
    pulp::view::IRNode node;
    int node_index = -1;
    int paint_order = -1;
    CapturedBox box;              ///< absolute page coordinates, verbatim
    int parent_slot = -1;         ///< -1 means "child of the IR root"
    int dom_parent_slot = -1;     ///< where DOM parentage alone would put it
    std::vector<int> children;    ///< slots, already in Chrome's paint order
};

bool boxes_overlap(const CapturedBox& a, const CapturedBox& b) {
    return a.left < b.left + b.width && b.left < a.left + a.width &&
           a.top < b.top + b.height && b.top < a.top + a.height;
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

    const auto ordinal = sibling_ordinals(index);
    // Slots in Chrome's paint order (ties already broken by document order),
    // and the reverse map that turns DOM parentage into tree parentage.
    std::vector<LoweredNode> slots;
    std::unordered_map<int, int> node_to_slot;

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
        lowered.name = describe(index, node.node_index);
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

        // Geometry is stamped when the tree is assembled, because a node's
        // offsets are relative to WHERE IT LANDS. What is fixed here is that
        // Chrome's solved box is the placement authority and no computed
        // declaration may move it — its sub-pixel values are Blink's 1/64px
        // layout grid arriving as data, consumed verbatim, never rounded.
        lowered.style.position = "absolute";

        lowered.attributes["paint_class"] =
            std::string(to_string(paint_class));
        lowered.attributes["paint_order"] = std::to_string(node.paint_order);
        lowered.attributes["source_tag"] = node.tag_name;
        lowered.stable_anchor_id = anchor_path(index, ordinal, node.node_index);
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

        node_to_slot[node.node_index] = static_cast<int>(slots.size());
        LoweredNode slot;
        slot.node = std::move(lowered);
        slot.node_index = node.node_index;
        slot.paint_order = node.paint_order;
        slot.box = box;
        slots.push_back(std::move(slot));
    }

    // ── Parentage ───────────────────────────────────────────────────────────
    // The nearest EMITTED DOM ancestor, so a skipped wrapper (a zero-area box,
    // a collapsed whitespace run) elides rather than orphaning its subtree.
    for (size_t i = 0; i < slots.size(); ++i) {
        int parent = -1;
        for (int walk = index.parent_of(slots[i].node_index); walk >= 0;
             walk = index.parent_of(walk)) {
            const auto found = node_to_slot.find(walk);
            if (found != node_to_slot.end()) {
                parent = found->second;
                break;
            }
        }
        slots[i].dom_parent_slot = parent;
    }

    // Hoisting reads the whole parentage map, so it cannot share the pass that
    // builds it: the ancestor a node needs to skip past may paint LATER than
    // the node itself, which is exactly the case being handled, and its own
    // parent would still be unresolved.
    for (size_t i = 0; i < slots.size(); ++i) {
        int parent = slots[i].dom_parent_slot;
        // A nested painter draws a parent's own box before any descendant, so
        // a child that Chrome painted FIRST cannot be expressed in place. Walk
        // out until it can be, and say so — never reorder it quietly. Slots are
        // in paint order, so "paints first" is simply the lower slot index, and
        // each step moves to a strict DOM ancestor so the walk terminates.
        const bool hoisted = parent > static_cast<int>(i);
        while (parent > static_cast<int>(i))
            parent = slots[static_cast<size_t>(parent)].dom_parent_slot;
        slots[i].parent_slot = parent;
        if (!hoisted) continue;
        ++counts.hoisted_escapes;
        slots[i].node.attributes["paint_order_hoisted"] = "1";
        slots[i].node.attributes["hoisted_from"] =
            slots[static_cast<size_t>(slots[i].dom_parent_slot)]
                .node.stable_anchor_id.value_or("");
    }

    // Children in slot order, which IS Chrome's paint order among siblings.
    std::vector<int> root_slots;
    for (size_t i = 0; i < slots.size(); ++i) {
        const int parent = slots[i].parent_slot;
        if (parent < 0) {
            root_slots.push_back(static_cast<int>(i));
        } else {
            slots[static_cast<size_t>(parent)].children.push_back(
                static_cast<int>(i));
        }
    }

    // ── Materialize ─────────────────────────────────────────────────────────
    // Recursive over slot indices rather than over IRNode references: appending
    // to a parent's `children` vector reallocates it, so any pointer taken into
    // it beforehand is dangling by the time the next child arrives.
    //
    // `frame_left`/`frame_top` are the parent's own page origin. The IR root's
    // origin is the crop shift, so a root child's relative box is its page box
    // plus `dx`/`dy` — unchanged from the flat lowering — and every deeper
    // child subtracts one more solved box. The sums therefore telescope back to
    // Chrome's absolute box exactly.
    std::vector<int> composed_order;   // slots in composed pre-order
    composed_order.reserve(slots.size());
    const auto place = [&](auto&& self, int slot, double frame_left,
                           double frame_top, int depth,
                           IRNode& parent) -> void {
        auto& entry = slots[static_cast<size_t>(slot)];
        entry.node.style.left = static_cast<float>(entry.box.left - frame_left);
        entry.node.style.top = static_cast<float>(entry.box.top - frame_top);
        entry.node.style.width = static_cast<float>(entry.box.width);
        entry.node.style.height = static_cast<float>(entry.box.height);
        // Within a stacking context Chrome's paint order IS the sibling order,
        // so it maps to z-index directly. Carrying it explicitly keeps the
        // order a property of the node instead of an accident of the vector.
        int z = 0;
        for (const int child : entry.children)
            slots[static_cast<size_t>(child)].node.style.z_index = z++;
        counts.max_depth = std::max(counts.max_depth, depth);
        composed_order.push_back(slot);

        const auto children = entry.children;
        const double child_left = entry.box.left;
        const double child_top = entry.box.top;
        parent.children.push_back(std::move(entry.node));
        IRNode& placed = parent.children.back();
        for (const int child : children)
            self(self, child, child_left, child_top, depth + 1, placed);
    };

    int z = 0;
    for (const int slot : root_slots)
        slots[static_cast<size_t>(slot)].node.style.z_index = z++;
    for (const int slot : root_slots)
        place(place, slot, -dx, -dy, 1, root);
    counts.root_children = static_cast<int>(root_slots.size());

    // ── Fidelity audit ──────────────────────────────────────────────────────
    // Re-expressing one flat paint order as a hierarchy reorders nodes whose
    // subtrees interleave in Chrome's numbering. That is free where the boxes
    // are disjoint — the painter's algorithm cannot show it — and a real
    // regression where they overlap. Count only the visible half, so the claim
    // "nesting cost no fidelity" is a number rather than an assurance.
    for (size_t a = 0; a < composed_order.size(); ++a) {
        const auto& first = slots[static_cast<size_t>(composed_order[a])];
        for (size_t b = a + 1; b < composed_order.size(); ++b) {
            const auto& second = slots[static_cast<size_t>(composed_order[b])];
            if (first.paint_order <= second.paint_order) continue;
            if (boxes_overlap(first.box, second.box))
                ++counts.overlapping_reorders;
        }
    }
    return counts;
}

}  // namespace pulp::import_design
