// SPDX-License-Identifier: MIT
#include "browser_capture_tree.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
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

constexpr std::string_view kCssWhitespace = " \t\r\n\f\v";

/// Visit `start`, then each of its ancestors, nearest first, until `visit`
/// returns false or the root is reached.
///
/// Bounded by the node count as well as by `parent_of` returning -1. The DOM
/// snapshot is an untrusted sidecar of the capture bundle, and a `parentIndex`
/// entry that points at itself — or into a longer cycle — would otherwise spin
/// forever while the caller's accumulator grew without bound.
template <typename Visit>
void walk_ancestry(const CapturedStyleIndex& index, int start, Visit&& visit) {
    const int limit = index.node_count();
    int current = start;
    for (int steps = 0; current >= 0 && steps < limit; ++steps) {
        if (!visit(current)) return;
        current = index.parent_of(current);
    }
}

/// The first class in a `class` attribute.
///
/// Split on ANY whitespace, not only a space: every HTML formatter wraps a long
/// class list across lines, so a tab or a newline routinely sits between the
/// tokens — and one left inside would put a line break inside the node's
/// signature and inside every anchor derived from it. Leading whitespace is
/// skipped for the same reason: a template that renders an empty slot
/// (`class=" panel"`) still names the node `div.panel`, not `div.`.
std::string_view first_class(std::string_view classes) {
    const auto begin = classes.find_first_not_of(kCssWhitespace);
    if (begin == std::string_view::npos) return {};
    const auto end = classes.find_first_of(kCssWhitespace, begin);
    return classes.substr(begin, end == std::string_view::npos
                                     ? std::string_view::npos
                                     : end - begin);
}

/// One anchor path segment with the path's own delimiters escaped.
///
/// `id` and `class` text is author-controlled and routinely contains the
/// characters the path is built out of — Tailwind's `w-1/2` is the everyday
/// case — so an unescaped segment yields an anchor that cannot be split back
/// into segments and that two different nodes can spell the same way. A quote
/// or a newline additionally breaks `find_anchored_tag`, which locates
/// `data-pulp-anchor="<anchor>"` by literal scan.
std::string escape_segment(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '/': out += "\\/"; break;
            case '[': out += "\\["; break;
            case ']': out += "\\]"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
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
    // The first class is the identifying one in every design-system idiom we
    // import; appending all of them makes the name unreadable. The attribute is
    // held by value because the view below points into it.
    const auto classes = index.attribute(node_index, "class");
    const auto first = first_class(classes);
    if (first.empty()) return name;
    return name + "." + std::string(first);
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
    walk_ancestry(index, node_index, [&](int node) {
        const auto key = describe(index, node);
        if (!key.empty()) {
            const int position =
                node < static_cast<int>(ordinal.size())
                    ? ordinal[static_cast<size_t>(node)]
                    : 0;
            segments.push_back(escape_segment(key) + "[" +
                               std::to_string(position) + "]");
        }
        return true;
    });
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

/// Whether a computed `transform` leaves the element's painted shape equal to
/// the box the snapshot reports for it.
///
/// DOMSnapshot bounds ARE post-transform, but for anything that rotates or
/// skews, that box is the axis-aligned BOUNDING box rather than the shape: a
/// 100×20 bar at 45° is reported as an 85×85 square, and painting the box fills
/// roughly 3.7× the ink in the wrong outline. A scale is safe precisely because
/// its bounding box IS its shape, which is why the assumption reads as true
/// until something rotates.
bool is_axis_preserving_transform(const std::string& value) {
    if (value.empty() || value == "none") return true;
    // Chrome serializes computed `transform` as a matrix, so the numbers are
    // the whole answer. Any other spelling is treated as non-preserving rather
    // than assumed harmless — including `matrix3d`, whose out-of-plane terms
    // this two-dimensional test cannot speak to.
    if (value.rfind("matrix(", 0) != 0 || value.back() != ')') return false;
    std::vector<double> numbers;
    const std::string body = value.substr(7, value.size() - 8);
    size_t start = 0;
    while (start <= body.size()) {
        const auto comma = body.find(',', start);
        const auto piece = body.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start);
        try {
            numbers.push_back(std::stod(piece));
        } catch (const std::exception&) {
            return false;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (numbers.size() != 6) return false;
    constexpr double kFlat = 1e-6;
    // b and c zero is a translation, a scale, or a flip. a and d zero is a
    // quarter turn, whose bounding box is still the shape. Anything else has
    // put the element's outline off the axes.
    return (std::abs(numbers[1]) < kFlat && std::abs(numbers[2]) < kFlat) ||
           (std::abs(numbers[0]) < kFlat && std::abs(numbers[3]) < kFlat);
}

/// The computed facts about one element that decide what clips what.
struct ClipFacts {
    bool clips = false;             ///< `overflow` is not `visible`
    bool out_of_flow = false;       ///< `position` is `absolute` or `fixed`
    bool viewport_relative = false; ///< `position: fixed`
    /// Establishes a containing block for out-of-flow descendants: any
    /// non-`static` position, and also a `transform` or `filter`, which take
    /// over as the containing block even on a statically positioned element.
    bool containing_block = false;
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

    // Which layout row carries each painted node's computed style. Ancestry
    // questions have to reach a node's declarations, not only its tag.
    std::unordered_map<int, int> layout_of;
    for (const auto& node : painted) layout_of[node.node_index] = node.layout_index;

    // "Are this element's pixels something style can reproduce at all?" Two
    // separate reasons say no: the tag draws from outside CSS, or a transform
    // has taken the element's outline off the axes so the box the snapshot
    // reports is its bounding box rather than its shape.
    std::unordered_map<int, bool> capture_only_memo;
    const auto capture_only_node = [&](int node_index) {
        const auto seen = capture_only_memo.find(node_index);
        if (seen != capture_only_memo.end()) return seen->second;
        bool answer = is_capture_only_element(index.tag_name(node_index));
        if (!answer) {
            const auto layout = layout_of.find(node_index);
            if (layout != layout_of.end()) {
                const auto computed = index.styles_for_layout(layout->second);
                const auto transform = computed.find("transform");
                answer = transform != computed.end() &&
                         !is_axis_preserving_transform(transform->second);
            }
        }
        capture_only_memo[node_index] = answer;
        return answer;
    };

    // "Is some ancestor of this node captured as one element?" — answered from
    // the DOCUMENT tree, never from what the walk has already visited. Paint
    // order is not document order, so a node inside an `<svg>` can be reached
    // before the `<svg>` itself; a memo seeded by visit order would record
    // "not under a capture" for that whole chain and then keep answering it.
    std::unordered_map<int, bool> under_capture_only_memo;
    const auto under_capture_only = [&](int node_index) {
        // The chain is back-filled so each node is walked once.
        std::vector<int> chain;
        bool answer = false;
        walk_ancestry(index, index.parent_of(node_index), [&](int current) {
            const auto seen = under_capture_only_memo.find(current);
            if (seen != under_capture_only_memo.end()) {
                answer = seen->second;
                return false;
            }
            if (capture_only_node(current)) {
                answer = true;
                return false;
            }
            chain.push_back(current);
            return true;
        });
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

        const bool capture_only = node.node_type == kElementNode &&
                                  capture_only_node(node.node_index);
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
            // WHY it cannot be drawn, because the two reasons have different
            // fixes: a `<canvas>` is the permanent answer, a rotation is a
            // transform the IR does not carry yet.
            lowered.attributes["capture_fallback_reason"] =
                is_capture_only_element(node.tag_name) ? "element" : "transform";
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
        // NOT `path`: that name is already owned by `AnchorStrategy::path`,
        // which is `Type[idx]` over the IR's own node types with no prefix. A
        // consumer re-deriving one of those against an anchor built here would
        // compute `frame[0]/frame[0]` and match nothing.
        lowered.anchor_strategy = "capture-path";

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
        walk_ancestry(index, index.parent_of(slots[i].node_index),
                      [&](int walk) {
                          const auto found = node_to_slot.find(walk);
                          if (found == node_to_slot.end()) return true;
                          parent = found->second;
                          return false;
                      });
        // A node that reaches itself is a `parentIndex` cycle, not parentage.
        // Left alone it becomes its own child, and materializing the tree then
        // recurses until the stack is gone.
        if (parent == static_cast<int>(i)) parent = -1;
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
        // Each step moves to a strict DOM ancestor, so this terminates on any
        // well-formed document; the slot count bounds it on a malformed one,
        // where the parentage above can have been cut short mid-cycle.
        for (size_t step = 0;
             parent > static_cast<int>(i) && step < slots.size(); ++step)
            parent = slots[static_cast<size_t>(parent)].dom_parent_slot;
        if (parent > static_cast<int>(i)) parent = -1;
        slots[i].parent_slot = parent;
        if (!hoisted) continue;
        ++counts.hoisted_escapes;
        slots[i].node.attributes["paint_order_hoisted"] = "1";
        slots[i].node.attributes["hoisted_from"] =
            slots[static_cast<size_t>(slots[i].dom_parent_slot)]
                .node.stable_anchor_id.value_or("");
    }

    // ── Clip audit ──────────────────────────────────────────────────────────
    // The tree carries `overflow` on a node and the renderer clips that node's
    // CHILDREN, so the emitted clip follows DOM parentage. CSS follows the
    // containing-block chain instead, and the two disagree in both directions.
    // Neither is fixable by re-parenting — the honest fix is a clip rectangle
    // carried on the node — so what lowering can do today is say how often it
    // is wrong rather than report those nodes as faithfully drawn.
    const int node_total = index.node_count();
    std::unordered_map<int, ClipFacts> clip_facts;
    const auto facts_of = [&](int node_index) {
        const auto seen = clip_facts.find(node_index);
        if (seen != clip_facts.end()) return seen->second;
        ClipFacts facts;
        // A text run's computed style row IS its parent element's, so reading
        // `position` off it would report the parent's — and a text run is
        // always in flow inside the element it belongs to.
        if (index.node_type_of(node_index) == kElementNode) {
            const auto layout = layout_of.find(node_index);
            if (layout != layout_of.end()) {
                const auto computed = index.styles_for_layout(layout->second);
                const auto value = [&](const char* name) {
                    const auto found = computed.find(name);
                    return found == computed.end() ? std::string{}
                                                   : found->second;
                };
                const auto overflow = value("overflow");
                facts.clips = !overflow.empty() && overflow != "visible";
                const auto position = value("position");
                facts.viewport_relative = position == "fixed";
                facts.out_of_flow =
                    position == "absolute" || facts.viewport_relative;
                const auto transform = value("transform");
                const auto filter = value("filter");
                facts.containing_block =
                    (!position.empty() && position != "static") ||
                    (!transform.empty() && transform != "none") ||
                    (!filter.empty() && filter != "none");
            }
        }
        clip_facts[node_index] = facts;
        return facts;
    };

    // The ancestors a browser would actually clip this node against.
    const auto css_clippers = [&](int node_index) {
        std::vector<int> clippers;
        int current = node_index;
        for (int steps = 0; steps < node_total; ++steps) {
            const auto self = facts_of(current);
            int ancestor = -1;
            if (self.out_of_flow) {
                // Its containing block is the viewport: nothing in the document
                // clips it.
                if (self.viewport_relative) break;
                // Ancestors BETWEEN an absolutely positioned node and its
                // containing block are not on its clip chain at all — this is
                // the escape the nested tree cannot express.
                walk_ancestry(index, index.parent_of(current), [&](int node) {
                    if (!facts_of(node).containing_block) return true;
                    ancestor = node;
                    return false;
                });
            } else {
                ancestor = index.parent_of(current);
            }
            if (ancestor < 0) break;
            if (facts_of(ancestor).clips) clippers.push_back(ancestor);
            current = ancestor;
        }
        return clippers;
    };

    for (size_t i = 0; i < slots.size(); ++i) {
        const auto clippers = css_clippers(slots[i].node_index);
        std::unordered_set<int> emitted;
        int walk = slots[i].parent_slot;
        for (size_t step = 0; walk >= 0 && step < slots.size(); ++step) {
            emitted.insert(slots[static_cast<size_t>(walk)].node_index);
            walk = slots[static_cast<size_t>(walk)].parent_slot;
        }
        const auto on_css_chain = [&clippers](int node) {
            return std::find(clippers.begin(), clippers.end(), node) !=
                   clippers.end();
        };
        const bool over = std::any_of(
            emitted.begin(), emitted.end(), [&](int node) {
                return facts_of(node).clips && !on_css_chain(node);
            });
        const bool lost = std::any_of(
            clippers.begin(), clippers.end(),
            [&](int node) { return emitted.count(node) == 0; });
        if (over) {
            ++counts.clip_over_applied;
            slots[i].node.attributes["clip_over_applied"] = "1";
        }
        if (lost) {
            ++counts.clip_lost;
            slots[i].node.attributes["clip_lost"] = "1";
        }
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
