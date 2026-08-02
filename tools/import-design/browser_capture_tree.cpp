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
/// Slice a UTF-8 string by a UTF-16 offset and length.
///
/// Chrome's `textBoxes` `start` / `length` count UTF-16 code units, because
/// that is how CDP indexes strings; the run's text arrives as UTF-8. Treating
/// one as the other splits a multi-byte sequence the moment a run contains a
/// dash, a curly apostrophe or a multiplication sign — which produces bytes
/// that are not valid UTF-8 at all, not merely an off-by-one.
std::string utf16_slice(const std::string& text, int start, int length) {
    if (start < 0 || length <= 0) return {};
    const auto begin = static_cast<size_t>(start);
    const auto count = static_cast<size_t>(length);
    size_t units = 0;
    size_t byte_begin = std::string::npos;
    size_t i = 0;
    while (i <= text.size()) {
        if (units == begin && byte_begin == std::string::npos) byte_begin = i;
        if (byte_begin != std::string::npos && units == begin + count)
            return text.substr(byte_begin, i - byte_begin);
        if (i >= text.size()) break;
        const auto lead = static_cast<unsigned char>(text[i]);
        size_t bytes = 1;
        if      ((lead & 0x80u) == 0x00u) bytes = 1;
        else if ((lead & 0xE0u) == 0xC0u) bytes = 2;
        else if ((lead & 0xF0u) == 0xE0u) bytes = 3;
        else if ((lead & 0xF8u) == 0xF0u) bytes = 4;
        // A codepoint outside the BMP is a surrogate PAIR in UTF-16.
        units += (bytes == 4) ? 2 : 1;
        i += bytes;
    }
    if (byte_begin == std::string::npos) return {};
    return text.substr(byte_begin);
}


using pulp::view::IRNode;

constexpr int kElementNode = 1;
constexpr int kTextNode = 3;

/// Stands in for "this edge does not clip". Far outside any page coordinate a
/// capture can hold, and finite so an unclipped edge still survives the
/// arithmetic that intersects, offsets and rounds a clip.
constexpr double kUnbounded = 1e7;

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
    /// The slot that OWNS this one when DOM parentage cannot say so: a
    /// pseudo-element's generated run maps back to the pseudo's own node, so
    /// walking its `parentIndex` reaches the pseudo's host and would make the
    /// run a sibling of the box it belongs inside. -1 for every other node.
    int owner_slot = -1;
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
    /// `clip-path` is not `none`. A clip-path clips the whole painted subtree
    /// regardless of position, and its region is a shape rather than a
    /// rectangle — so it reaches nodes the `overflow` chain does not, and a
    /// rectangle cannot stand in for it.
    bool shape_clips = false;
    /// Border widths, so a clip can be taken against the PADDING box.
    /// `overflow` clips descendants to the padding box, not the border box: a
    /// 1px-bordered card that clips is one pixel tighter on every side than the
    /// box the snapshot reports for it.
    double border_left = 0.0;
    double border_top = 0.0;
    double border_right = 0.0;
    double border_bottom = 0.0;
};

bool boxes_overlap(const CapturedBox& a, const CapturedBox& b) {
    return a.left < b.left + b.width && b.left < a.left + a.width &&
           a.top < b.top + b.height && b.top < a.top + a.height;
}

/// A clip region in page coordinates, held by edge so intersection is a max/min
/// per side and an unclipped region is simply the whole plane.
struct ClipRect {
    double left = -kUnbounded;
    double top = -kUnbounded;
    double right = kUnbounded;
    double bottom = kUnbounded;

    bool empty() const { return right <= left || bottom <= top; }
};

ClipRect intersect(const ClipRect& a, const ClipRect& b) {
    return ClipRect{std::max(a.left, b.left), std::max(a.top, b.top),
                    std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

ClipRect rect_of(const CapturedBox& box) {
    return ClipRect{box.left, box.top, box.left + box.width,
                    box.top + box.height};
}

/// Whether `outer` holds every pixel of `inner`, allowing for the sub-pixel
/// values Blink's 1/64px layout grid produces. An empty `inner` is contained by
/// anything: there is no ink to lose.
bool contains(const ClipRect& outer, const ClipRect& inner) {
    if (inner.empty()) return true;
    if (outer.empty()) return false;
    constexpr double kGrid = 1.0 / 64.0;
    return outer.left <= inner.left + kGrid && outer.top <= inner.top + kGrid &&
           outer.right >= inner.right - kGrid &&
           outer.bottom >= inner.bottom - kGrid;
}

/// A parsed CSS length in px, or 0 for anything else. Chrome serializes
/// computed border widths as `<n>px`, so this is the whole grammar that
/// reaches here.
double px_value(const std::string& value) {
    if (value.empty()) return 0.0;
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        return 0.0;
    }
}

/// Where a browser would actually clip a node, resolved from the snapshot.
///
/// A pure function of the captured document — it holds no opinion about the
/// tree being emitted, which is the point: the emitted clip has to be compared
/// against something derived independently of it, or the comparison agrees with
/// itself by construction.
///
/// Two chains, because CSS clips along two different ones. `overflow` clips
/// along the CONTAINING-BLOCK chain, so an absolutely positioned node whose
/// containing block sits above an `overflow: hidden` ancestor is not clipped by
/// it — the escape DOM parentage gets wrong in both directions. `clip-path`
/// clips the painted subtree, so it reaches every DOM descendant whatever its
/// position, and its region is a shape one rectangle cannot stand in for.
class CssClipChain {
public:
    struct Result {
        ClipRect rect;              ///< intersection of every rect clipper
        bool clipped = false;       ///< at least one rect clipper applies
        bool inexpressible = false; ///< a shape clipper this model cannot carry
    };

    explicit CssClipChain(const CapturedStyleIndex& index) : index_(index) {
        for (const auto& node : index.painted_nodes()) {
            layout_of_[node.node_index] = node.layout_index;
            box_of_[node.node_index] = node.bounds;
        }
    }

    Result resolve(int node_index) {
        Result out;
        // The shape chain first: plain DOM ancestry, because a `clip-path`
        // applies to everything painted inside the element.
        walk_ancestry(index_, index_.parent_of(node_index), [&](int node) {
            if (facts_of(node).shape_clips) {
                out.inexpressible = true;
                return false;
            }
            return true;
        });
        const int total = index_.node_count();
        int current = node_index;
        for (int steps = 0; steps < total; ++steps) {
            const auto self = facts_of(current);
            int ancestor = -1;
            if (self.out_of_flow) {
                // Its containing block is the viewport: nothing in the document
                // clips it.
                if (self.viewport_relative) break;
                walk_ancestry(index_, index_.parent_of(current), [&](int node) {
                    if (!facts_of(node).containing_block) return true;
                    ancestor = node;
                    return false;
                });
            } else {
                ancestor = index_.parent_of(current);
            }
            if (ancestor < 0) break;
            if (facts_of(ancestor).clips) {
                out.rect = intersect(out.rect, clip_box_of(ancestor));
                out.clipped = true;
            }
            current = ancestor;
        }
        return out;
    }

private:
    ClipFacts facts_of(int node_index) {
        const auto seen = facts_.find(node_index);
        if (seen != facts_.end()) return seen->second;
        ClipFacts facts;
        // A text run's computed style row IS its parent element's, so reading
        // `position` off it would report the parent's — and a text run is
        // always in flow inside the element it belongs to.
        if (index_.node_type_of(node_index) == kElementNode) {
            const auto layout = layout_of_.find(node_index);
            if (layout != layout_of_.end()) {
                const auto computed = index_.styles_for_layout(layout->second);
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
                const auto clip_path = value("clip-path");
                facts.shape_clips = !clip_path.empty() && clip_path != "none";
                facts.border_left = px_value(value("border-left-width"));
                facts.border_top = px_value(value("border-top-width"));
                facts.border_right = px_value(value("border-right-width"));
                facts.border_bottom = px_value(value("border-bottom-width"));
            }
        }
        facts_[node_index] = facts;
        return facts;
    }

    /// The rectangle an `overflow` clip confines descendants to: the element's
    /// PADDING box. CSS clips overflow inside the border, so taking the box the
    /// snapshot reports verbatim leaks one border width on each side — visible
    /// as a hairline of content drawn over a card's own frame.
    ClipRect clip_box_of(int node_index) {
        const auto found = box_of_.find(node_index);
        if (found == box_of_.end()) return ClipRect{};
        const auto facts = facts_of(node_index);
        auto rect = rect_of(found->second);
        rect.left += facts.border_left;
        rect.top += facts.border_top;
        rect.right -= facts.border_right;
        rect.bottom -= facts.border_bottom;
        return rect;
    }

    const CapturedStyleIndex& index_;
    std::unordered_map<int, int> layout_of_;
    std::unordered_map<int, CapturedBox> box_of_;
    std::unordered_map<int, ClipFacts> facts_;
};

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
    // How many generated text runs a pseudo-element has already contributed.
    // A `content: counter(n) ". "` renders as TWO runs, so the count is what
    // separates their anchors — and it is a position among that pseudo's own
    // runs, which is content order, not a position in the serialization.
    std::unordered_map<int, int> generated_runs;

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
        // A layout row that carries text IS a text run, whatever DOM node it
        // maps back to. `content` reaches the capture only this way: Chrome
        // gives a pseudo-element one row for its box and one more per generated
        // run, and every one of them maps back to the pseudo's ELEMENT node —
        // so a text run identified by its DOM node type misses all of them and
        // the string lowers as an empty frame. Reading the rows also resolves
        // `counter()` and `attr()` for free, because Chrome evaluated both
        // before serializing. No element row in the corpus carries text, so
        // this widens the predicate over generated content alone.
        const bool generated_text =
            node.node_type == kElementNode && !node.text.empty();
        const bool is_text = node.node_type == kTextNode || generated_text;
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

        // A run whose FIRST line box starts to the right of its own block is
        // continuing a line an earlier inline sibling began — an inline `<span>`
        // splits one visual paragraph into several sibling runs, and the run
        // after the span resumes mid-line before returning to the left edge.
        // Its `bounds` is the UNION of the lines it shares with those siblings,
        // so no single box describes where its text goes: wrapping inside that
        // union lays the first line on top of the sibling's text, and refusing
        // to wrap freezes it at one line. Both are wrong, in opposite ways.
        const auto line_boxes =
            is_text ? index.text_boxes_for_layout(node.layout_index)
                    : std::vector<CapturedTextBox>{};
        const bool resumes_a_line =
            line_boxes.size() > 1 &&
            line_boxes.front().bounds.left > box.left + 1.0;

        PaintClass paint_class = PaintClass::native;
        if (capture_only) {
            paint_class = PaintClass::element_capture_fallback;
            lowered.type = "frame";
            // Nothing attaches a raster to this node, so the frame emitted here
            // paints only whatever background the element's own styles carry —
            // for a `<canvas>` or an `<svg>`, nothing at all. Saying so on the
            // node is the difference between a hole a consumer can find and a
            // classification that reads as "handled by a capture".
            lowered.attributes["unpainted"] = "true";
            counts.unpainted_fallback_area +=
                node.bounds.width * node.bounds.height;
            lowered.attributes["capture_fallback_element"] = node.tag_name;
            // WHY it cannot be drawn, because the two reasons have different
            // fixes: a `<canvas>` is the permanent answer, a rotation is a
            // transform the IR does not carry yet.
            lowered.attributes["capture_fallback_reason"] =
                is_capture_only_element(node.tag_name) ? "element" : "transform";
        } else if (is_text && resumes_a_line) {
            // Emit ONE NODE PER LINE BOX rather than one per run. Each line
            // carries the substring Chrome put on it, at the rect Chrome put it
            // at, so a line beginning at x=1336 inside a block whose left edge
            // is 1168 simply IS at 1336 — the case that defeats any per-run
            // model regardless of how it wraps. The run's own node becomes a
            // frame holding them, so parentage, paint order and the anchor path
            // are unchanged.
            //
            // The tradeoff, stated because it is permanent for these nodes:
            // a per-line node is pinned to Chrome's break positions and cannot
            // reflow. Runs that own their block keep the reflowing text path
            // above, so only the runs that CANNOT reflow correctly give it up.
            lowered.type = "frame";
            for (const auto& line : line_boxes) {
                if (line.length <= 0 || line.start < 0) continue;
                pulp::view::IRNode fragment;
                fragment.type = "text";
                fragment.name = "#text";
                fragment.text_content =
                    utf16_slice(node.text, line.start, line.length);
                if (fragment.text_content.empty()) continue;
                // Typography only — the box half belongs to the run, and a
                // fragment repeating it would paint the parent's surfaces once
                // per line.
                apply_computed_styles(computed, box, fragment.style,
                                      ComputedStyleScope::text_only);
                // A fragment never wraps: its width IS one of Chrome's lines,
                // so any reflow inside it would be Pulp disagreeing with a
                // measurement it already has.
                fragment.style.white_space = "nowrap";
                fragment.style.position = "absolute";
                // Relative to the run's union box, which is where this frame
                // lands. Fragments are not slots, so the assembly pass that
                // stamps slot geometry leaves these alone — they are already
                // expressed against their parent.
                fragment.style.left =
                    static_cast<float>(line.bounds.left - box.left);
                fragment.style.top =
                    static_cast<float>(line.bounds.top - box.top);
                fragment.style.width = static_cast<float>(line.bounds.width);
                fragment.style.height = static_cast<float>(line.bounds.height);
                lowered.children.push_back(std::move(fragment));
            }
            ++counts.text;
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
        // `overflow` is NOT carried onto a lowered node. A renderer applies it
        // to whatever the node's children turn out to be, which makes DOM
        // parentage the clip authority — and DOM parentage is not the chain CSS
        // clips along. The per-node clip rectangle resolved below is the
        // authority instead; leaving `overflow` here too would re-impose the
        // parentage clip underneath it and clip an escaping node twice.
        lowered.style.overflow.reset();

        lowered.attributes["paint_class"] =
            std::string(to_string(paint_class));
        lowered.attributes["paint_order"] = std::to_string(node.paint_order);
        lowered.attributes["source_tag"] = node.tag_name;
        lowered.stable_anchor_id = anchor_path(index, ordinal, node.node_index);
        if (generated_text) {
            // The pseudo's box already claims the path that names the pseudo,
            // so its runs need a segment of their own or every run of one
            // pseudo shares an anchor and a stored edit lands on whichever the
            // walk reached first. `#text[k]` is the segment a real text child
            // would carry, which also keeps the pseudo selectable by prefix.
            *lowered.stable_anchor_id +=
                "/#text[" +
                std::to_string(generated_runs[node.node_index]++) + "]";
        }
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
        if (is_text && !resumes_a_line) ++counts.text;
        ++counts.lowered;

        LoweredNode slot;
        // A pseudo-element occupies several slots — its box and each of its
        // generated runs. The FIRST is the one parentage should resolve to:
        // it is the box, and the runs belong inside it. `emplace` keeps it;
        // `operator[]` would leave the map pointing at the last run.
        const auto claim =
            node_to_slot.emplace(node.node_index,
                                 static_cast<int>(slots.size()));
        if (generated_text && !claim.second)
            slot.owner_slot = claim.first->second;
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
        int parent = slots[i].owner_slot;
        if (parent < 0)
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

    // Where a browser would actually clip each node, resolved from the
    // snapshot alone.
    CssClipChain css_chain(index);

    // ── The clip each node carries ──────────────────────────────────────────
    // Stored ON the node, relative to the node, so it travels with the node
    // instead of with its position in the tree. That is what makes hoisting
    // safe: a node regrafted onto a new ancestor keeps the clip CSS gives it,
    // and a node nested under an ancestor CSS does not clip is not clipped by
    // being there. The renderer applies it to the node's own ink only —
    // descendants carry their own, resolved the same way — so the rectangle
    // never has to be monotone down the tree, which is exactly the constraint
    // an inherited clip could not satisfy.
    std::vector<CssClipChain::Result> css_clip(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        css_clip[i] = css_chain.resolve(slots[i].node_index);
        if (!css_clip[i].clipped) continue;
        const auto& box = slots[i].box;
        // A clip that already holds everything the node draws is a rectangle
        // the renderer would install once a frame to change nothing. Skipped —
        // but only for a node whose ink IS its box: a shadow or a blur paints
        // outside the border box, and CSS clips that overspill too.
        const bool draws_outside_box =
            !slots[i].node.style.box_shadow.empty() ||
            slots[i].node.style.filter.has_value();
        if (!draws_outside_box && contains(css_clip[i].rect, rect_of(box)))
            continue;
        pulp::view::IRStyle::ClipRect rect{};
        rect.x = static_cast<float>(css_clip[i].rect.left - box.left);
        rect.y = static_cast<float>(css_clip[i].rect.top - box.top);
        // Two clippers that do not overlap leave nothing, which IS the answer —
        // the node draws no pixels. Held at zero rather than passed on as a
        // negative extent, which a canvas clip is not defined for.
        rect.width = static_cast<float>(std::max(
            0.0, css_clip[i].rect.right - css_clip[i].rect.left));
        rect.height = static_cast<float>(std::max(
            0.0, css_clip[i].rect.bottom - css_clip[i].rect.top));
        slots[i].node.style.clip_rect = rect;
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
    //
    // The clip audit rides along here, because this is where the ARTIFACT
    // exists: the rectangle a consumer will read off the node, composed with
    // the clips its emitted ancestors impose, against the page coordinates the
    // offsets telescope back to. Deriving the emitted side from the model that
    // produced it would agree with itself by construction; reading it back is
    // what makes a rectangle written in the wrong space, dropped, or silently
    // re-inherited from a parent's `overflow` show up as a number.
    const auto overflow_clip_of = [](const IRNode& node, double left,
                                     double top) {
        ClipRect clip;
        if (!node.style.overflow) return clip;
        const auto& value = *node.style.overflow;
        if (value.empty() || value == "visible") return clip;
        // A clip needs a box. A node that says it clips but never says how big
        // it is would otherwise clip to a point and read as every descendant
        // being over-clipped — a counter storm from a missing field.
        if (!node.style.width || !node.style.height) return clip;
        clip.left = left;
        clip.top = top;
        clip.right = left + *node.style.width;
        clip.bottom = top + *node.style.height;
        return clip;
    };

    // The panel frame's own clip. The caller sizes the frame to the crop it
    // asked for, so this is the window every lowered node is drawn through.
    const ClipRect root_clip = overflow_clip_of(root, -dx, -dy);

    std::vector<int> composed_order;   // slots in composed pre-order
    composed_order.reserve(slots.size());
    const auto place = [&](auto&& self, int slot, double frame_left,
                           double frame_top, int depth, IRNode& parent,
                           ClipRect ancestor_clip) -> void {
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

        // What the emitted tree clips this node's own ink to: whatever its
        // emitted ancestors impose, plus the rectangle the node itself carries.
        // The node's own rectangle deliberately does NOT descend — a child
        // resolves its own chain, and a child can legitimately need a WIDER
        // clip than its parent when it escapes a clipper the parent is inside.
        ClipRect emitted = ancestor_clip;
        if (const auto& own = entry.node.style.clip_rect) {
            emitted = intersect(
                emitted, ClipRect{entry.box.left + own->x,
                                  entry.box.top + own->y,
                                  entry.box.left + own->x + own->width,
                                  entry.box.top + own->y + own->height});
        }
        const auto& css = css_clip[static_cast<size_t>(slot)];
        if (css.inexpressible) {
            // A shape clip the rectangle model cannot carry, so the node keeps
            // ink the browser cuts away. Named as the reason rather than folded
            // into the geometric verdict, which would report it as an ordinary
            // missing rectangle someone could go "fix".
            ++counts.clip_lost;
            entry.node.attributes["clip_lost"] = "1";
            entry.node.attributes["clip_inexpressible"] = "clip-path";
        } else {
            const auto box = rect_of(entry.box);
            // The panel frame is on BOTH sides of the comparison. A frame is a
            // window onto the page and the crop is its definition, so a node
            // the frame cuts is not a node the tree got wrong — counting it
            // would report `<html>` on every cropped capture and bury the one
            // node an ancestor inside the panel clipped by mistake.
            const auto ink_css = intersect(intersect(box, css.rect), root_clip);
            const auto ink_emitted = intersect(box, emitted);
            if (!contains(ink_emitted, ink_css)) {
                ++counts.clip_over_applied;
                entry.node.attributes["clip_over_applied"] = "1";
            }
            if (!contains(ink_css, ink_emitted)) {
                ++counts.clip_lost;
                entry.node.attributes["clip_lost"] = "1";
            }
        }

        const auto children = entry.children;
        const double child_left = entry.box.left;
        const double child_top = entry.box.top;
        const ClipRect child_clip =
            intersect(ancestor_clip,
                      overflow_clip_of(entry.node, entry.box.left,
                                       entry.box.top));
        parent.children.push_back(std::move(entry.node));
        IRNode& placed = parent.children.back();
        for (const int child : children)
            self(self, child, child_left, child_top, depth + 1, placed,
                 child_clip);
    };

    int z = 0;
    for (const int slot : root_slots)
        slots[static_cast<size_t>(slot)].node.style.z_index = z++;
    for (const int slot : root_slots)
        place(place, slot, -dx, -dy, 1, root, root_clip);
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
