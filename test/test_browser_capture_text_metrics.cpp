// SPDX-License-Identifier: MIT
//
// Chrome's per-line text boxes, read out of a real capture.
//
// A text run's `layout.bounds` is the UNION of the line boxes it broke across,
// so it is the paragraph's block and not any line. Every advance question — is
// our text the right width, did we break where Chrome broke — is a question
// about a line box, and the union answers it wrongly by exactly the amount the
// run wrapped.
//
// The fixture is a real Chromium capture, so the expected numbers below are
// Chrome's own measurements of its own render. They are deliberately not
// recomputed here from font metrics: an expectation derived from the same
// arithmetic as the code under test agrees with that code whether or not
// either is right.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "tools/import-design/browser_capture_ir.hpp"
#include "tools/import-design/browser_capture_styles.hpp"

#include <pulp/canvas/text_shaper.hpp>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <iostream>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

using namespace pulp::import_design;
using Catch::Matchers::WithinAbs;

namespace {

namespace fs = std::filesystem;

fs::path wrap_snapshot() {
    return fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) /
           "browser-capture-text-wrap" / "dom-snapshot.json";
}

CapturedStyleIndex load_wrap_fixture() {
    auto index = CapturedStyleIndex::load(wrap_snapshot());
    REQUIRE(index.has_value());
    return std::move(*index);
}

/// The layout index of the one painted node whose text begins with `prefix`.
///
/// Addressing by text rather than by a hardcoded index means a re-capture that
/// renumbers the layout does not silently retarget these assertions at a
/// neighbour.
int layout_of_text(const CapturedStyleIndex& index, const std::string& prefix,
                   size_t occurrence) {
    size_t seen = 0;
    for (const auto& node : index.painted_nodes()) {
        if (node.text.rfind(prefix, 0) != 0) continue;
        if (seen++ == occurrence) return node.layout_index;
    }
    FAIL("no painted node number " << occurrence << " whose text starts with "
                                   << prefix);
    return -1;
}


/// Horizontal scale a node inherits from `transform` on its ancestors.
///
/// Chrome reports a text box's bounds in the TRANSFORMED space while
/// `font-size` stays the untransformed computed value, so an advance shaped at
/// the computed size is only comparable to the box after the same scale is
/// applied. A panel that fits itself to a viewport with `transform: scale(.9)`
/// otherwise reads as every run inside it being 11% too wide.
double inherited_scale_x(const CapturedStyleIndex& index,
                         const std::map<int, int>& layout_of_node,
                         int node_index) {
    double scale = 1.0;
    for (int cursor = index.parent_of(node_index); cursor >= 0;
         cursor = index.parent_of(cursor)) {
        const auto layout = layout_of_node.find(cursor);
        if (layout == layout_of_node.end()) continue;
        const auto styles = index.styles_for_layout(layout->second);
        const auto it = styles.find("transform");
        if (it == styles.end() || it->second.empty() || it->second == "none")
            continue;
        const auto open = it->second.find('(');
        if (open == std::string::npos) continue;
        // matrix(a, b, ...) and matrix3d(a, b, ...) both start with the first
        // column of the linear part, so `a` is the x scale for any transform
        // without rotation or skew, which is what a fit-to-viewport wrapper is.
        scale *= std::atof(it->second.c_str() + open + 1);
    }
    return scale;
}

/// Lower the wrap fixture through the same entry point `pulp import-design`
/// uses, so what is asserted is what a real import produces.
BrowserCaptureIrResult lower_wrap_fixture() {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    return lower_browser_capture_to_ir(
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) /
            "browser-capture-text-wrap" / "capture.json",
        options);
}

/// The lowered root, or a hard failure — a lowering that produced no IR is a
/// different bug than the one under test and must not read as "node absent".
const pulp::view::IRNode& root_of(const BrowserCaptureIrResult& result) {
    REQUIRE(result.design_ir.has_value());
    return result.design_ir->root;
}

/// The Nth lowered text node whose content starts with `prefix`, anywhere in
/// the tree.
const pulp::view::IRNode* find_text_node(const pulp::view::IRNode& root,
                                         const std::string& prefix,
                                         size_t occurrence = 0) {
    size_t seen = 0;
    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            if (found) return;
            if (node.type == "text" && node.text_content.rfind(prefix, 0) == 0 &&
                seen++ == occurrence) {
                found = &node;
                return;
            }
            for (const auto& child : node.children) walk(child);
        };
    walk(root);
    return found;
}

/// The first text node anywhere in the tree whose content CONTAINS `needle`.
const pulp::view::IRNode* find_text_node_containing(
    const pulp::view::IRNode& root, const std::string& needle) {
    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            if (found) return;
            if (node.type == "text" &&
                node.text_content.find(needle) != std::string::npos) {
                found = &node;
                return;
            }
            for (const auto& child : node.children) walk(child);
        };
    walk(root);
    return found;
}

/// The first non-text node whose CHILDREN carry `needle` — the shape a
/// per-line-box run lowers to.
const pulp::view::IRNode* find_frame_containing(const pulp::view::IRNode& root,
                                                const std::string& needle) {
    const pulp::view::IRNode* found = nullptr;
    std::function<void(const pulp::view::IRNode&)> walk =
        [&](const pulp::view::IRNode& node) {
            if (found) return;
            if (node.type != "text") {
                for (const auto& child : node.children) {
                    if (child.type == "text" &&
                        child.text_content.find(needle) != std::string::npos) {
                        found = &node;
                        return;
                    }
                }
            }
            for (const auto& child : node.children) walk(child);
        };
    walk(root);
    return found;
}

}  // namespace

TEST_CASE("wrapped run reports one box per line, not its own bounds",
          "[browser-capture][text-metrics]") {
    const auto index = load_wrap_fixture();
    const int wrapped = layout_of_text(index, "Chrome breaks this", 0);
    const auto boxes = index.text_boxes_for_layout(wrapped);

    // Chrome broke this run across five lines. One box would mean the reader
    // collapsed them; the run's own bounds would also be one box, which is the
    // impostor this case exists to reject.
    REQUIRE(boxes.size() == 5);

    const std::vector<double> expected_widths{
        147.109375, 191.75, 219.5625, 197.34375, 45.78125};
    const std::vector<int> expected_starts{0, 19, 44, 73, 102};
    const std::vector<int> expected_lengths{18, 24, 28, 28, 6};
    for (size_t i = 0; i < boxes.size(); ++i) {
        CHECK_THAT(boxes[i].bounds.width,
                   WithinAbs(expected_widths[i], 0.001));
        CHECK(boxes[i].start == expected_starts[i]);
        CHECK(boxes[i].length == expected_lengths[i]);
        // Successive lines step down by the run's line-height and share a left
        // edge; a reader that mixed up the parallel arrays would scramble this.
        CHECK_THAT(boxes[i].bounds.top,
                   WithinAbs(24.0 + 20.0 * static_cast<double>(i), 0.001));
        CHECK_THAT(boxes[i].bounds.left, WithinAbs(24.0, 0.001));
    }

    // Four of the five lines are narrower than the block the run occupies, so
    // a reader that returned `layout.bounds` per line would be wrong on four of
    // five and right on the widest. That is precisely why a fixture whose runs
    // all fit on one line cannot tell the two implementations apart.
    const auto run_bounds = index.painted_nodes();
    const auto it = std::find_if(
        run_bounds.begin(), run_bounds.end(),
        [wrapped](const CapturedPaintNode& node) {
            return node.layout_index == wrapped;
        });
    REQUIRE(it != run_bounds.end());
    CHECK_THAT(it->bounds.width, WithinAbs(219.5625, 0.001));
    CHECK_THAT(it->bounds.height, WithinAbs(100.0, 0.001));
    const size_t narrower = static_cast<size_t>(std::count_if(
        boxes.begin(), boxes.end(),
        [&it](const CapturedTextBox& box) {
            return box.bounds.width < it->bounds.width - 0.001;
        }));
    CHECK(narrower == 4);
}

TEST_CASE("a nowrap run's single box can be wider than its block",
          "[browser-capture][text-metrics]") {
    const auto index = load_wrap_fixture();
    // Same text as the wrapped run, in a block of the same 220px width, but
    // forbidden to break.
    const int unwrapped = layout_of_text(index, "Chrome breaks this", 1);
    const auto boxes = index.text_boxes_for_layout(unwrapped);

    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].start == 0);
    CHECK(boxes[0].length == 108);
    // Wider than the 220px block it sits in — so "the box is the element's CSS
    // width" is wrong in this direction too, not only in the wrapped one.
    CHECK_THAT(boxes[0].bounds.width, WithinAbs(819.546875, 0.001));
}

TEST_CASE("a layout node that laid out no text reports no boxes",
          "[browser-capture][text-metrics]") {
    const auto index = load_wrap_fixture();
    bool checked_an_element = false;
    for (const auto& node : index.painted_nodes()) {
        if (!node.text.empty()) continue;
        CHECK(index.text_boxes_for_layout(node.layout_index).empty());
        checked_an_element = true;
    }
    CHECK(checked_an_element);

    // Out of range in both directions is empty rather than a read past the end.
    CHECK(index.text_boxes_for_layout(-1).empty());
    CHECK(index.text_boxes_for_layout(1 << 20).empty());
}

TEST_CASE("a capture recorded without DOM rects still loads",
          "[browser-capture][text-metrics]") {
    // The clip fixtures predate this reader. They carry `textBoxes`, so the
    // guarantee under test is the weaker one that matters for old captures:
    // asking a snapshot for boxes never fails the load, and a node that has
    // none answers empty.
    const auto index = CapturedStyleIndex::load(
        fs::path(PULP_BROWSER_CAPTURE_STYLE_FIXTURE_DIR) / "dom-snapshot.json");
    REQUIRE(index.has_value());
    const int drive = layout_of_text(*index, "DRIVE", 0);
    const auto boxes = index->text_boxes_for_layout(drive);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].length == 5);
    CHECK_THAT(boxes[0].bounds.width, WithinAbs(46.140625, 0.001));
}

TEST_CASE("weight changes the advance Chrome measures",
          "[browser-capture][text-metrics]") {
    const auto index = load_wrap_fixture();
    // Identical text, identical size, one weight apart, on a variable font
    // carrying real 400 and 700 instances.
    const auto regular =
        index.text_boxes_for_layout(layout_of_text(index, "Handgloves", 0));
    const auto bold =
        index.text_boxes_for_layout(layout_of_text(index, "Handgloves", 1));
    REQUIRE(regular.size() == 1);
    REQUIRE(bold.size() == 1);
    REQUIRE(regular[0].length == bold[0].length);

    CHECK_THAT(regular[0].bounds.width, WithinAbs(150.8125, 0.001));
    CHECK_THAT(bold[0].bounds.width, WithinAbs(154.203125, 0.001));
    // The gap is what a measurement that ignores weight throws away. It is
    // recorded here so the canvas-side test that consumes it is reading a
    // number the capture actually carries, not one written twice.
    CHECK(bold[0].bounds.width > regular[0].bounds.width + 3.0);
}

// ── The lowered IR must CARRY white-space, not merely behave as if it had ────
//
// `white-space: normal` is the value that turns wrapping on, and the shared
// `is_absent` predicate used to discard it as "contributes nothing". The
// failure was invisible from the render alone: an absent property and a
// permissive one produce the same single-line output, so a test that only
// asserted "a wrapping node renders" would have passed throughout. These cases
// assert the property's PRESENCE and VALUE on the lowered node, which is the
// only place the two states differ.

TEST_CASE("a wrapping run lowers with white-space present and permissive",
          "[browser-capture][text-metrics]") {
    const auto result = lower_wrap_fixture();
    const auto* wrapped = find_text_node(root_of(result), "Chrome breaks this");
    REQUIRE(wrapped != nullptr);

    // Present at all — this is the assertion the drop defeated.
    REQUIRE(wrapped->style.white_space.has_value());
    // And permissive, which is what the materializer's `!= "nowrap"` test
    // needs in order to enable multi-line.
    CHECK(*wrapped->style.white_space == "normal");
}

TEST_CASE("a nowrap run lowers with white-space present and restrictive",
          "[browser-capture][text-metrics]") {
    const auto result = lower_wrap_fixture();
    // Same text as the wrapped run; the second occurrence is the nowrap one.
    const auto* unwrapped =
        find_text_node(root_of(result), "Chrome breaks this", 1);
    REQUIRE(unwrapped != nullptr);
    REQUIRE(unwrapped->style.white_space.has_value());
    CHECK(*unwrapped->style.white_space == "nowrap");
}

TEST_CASE("a line-resuming run is cached with a per-line horizontal origin",
          "[browser-capture][text-metrics]") {
    // An inline <span> splits one visual paragraph into sibling runs, and the
    // run after the span resumes mid-line before returning to the left edge.
    // Chrome's boxes for it are correct, but drawing them needs a per-line
    // HORIZONTAL origin and the renderer stacks every line from the box's left
    // edge — so caching them today would draw the first line on top of the
    // sibling's text. Worse than not wrapping, and visibly so.
    //
    // Until the renderer carries per-line x, these runs are marked nowrap:
    // one line, incomplete, but never overprinted. Asserted rather than left
    // implicit, because "no cache here" is a deliberate exclusion and the next
    // reader should see that it was chosen.
    const auto result = lower_wrap_fixture();
    const auto* resumed = find_text_node_containing(root_of(result), "resumes");
    REQUIRE(resumed != nullptr);
    CHECK(resumed->children.empty());
    // The whole sentence stays on one node, so nothing is lost while the
    // renderer catches up.
    CHECK(resumed->text_content.find("for the rest.") != std::string::npos);

    // It IS cached, and its first line carries the horizontal origin that no
    // per-run wrap can produce: the line begins ~162px into the run's own
    // block because a sibling's text occupies the space before it.
    REQUIRE(resumed->text_line_boxes.size() == 4);
    REQUIRE(resumed->text_layout_basis.has_value());
    CHECK(resumed->text_line_boxes.front().left > 100.0f);
    for (size_t i = 1; i < resumed->text_line_boxes.size(); ++i)
        CHECK_THAT(static_cast<double>(resumed->text_line_boxes[i].left),
                   WithinAbs(0.0, 0.5));
}

TEST_CASE("the run that defeats a per-run model really does start mid-line",
          "[browser-capture][text-metrics]") {
    // The exclusion above is only justified if such a run exists in the
    // fixture; otherwise the case passes vacuously and would keep passing if
    // the fixture lost its inline span.
    const auto index = load_wrap_fixture();
    bool found_continuation = false;
    for (const auto& node : index.painted_nodes()) {
        const auto boxes = index.text_boxes_for_layout(node.layout_index);
        if (boxes.size() <= 1) continue;
        if (boxes.front().bounds.left > node.bounds.left + 1.0) {
            found_continuation = true;
            // ~162px into its own block — an offset no per-run wrap produces.
            CHECK(boxes.front().bounds.left - node.bounds.left > 100.0);
        }
    }
    CHECK(found_continuation);
}

TEST_CASE("a run that owns its block keeps reflowing text",
          "[browser-capture][text-metrics]") {
    // Per-line-box lowering pins a node to Chrome's break positions, so it is
    // applied ONLY where reflow cannot be correct. A paragraph that starts its
    // own line stays one text node that wraps.
    const auto result = lower_wrap_fixture();
    const auto* wrapped = find_text_node(root_of(result), "Chrome breaks this");
    REQUIRE(wrapped != nullptr);
    CHECK(wrapped->children.empty());
    REQUIRE(wrapped->style.white_space.has_value());
    CHECK(*wrapped->style.white_space == "normal");
}

// ── Chrome's line breaking, cached beside the paragraph ─────────────────────
//
// The node stays one semantic paragraph — full text, one style, one box — and
// the browser's own breaking rides alongside it as a cache. That keeps
// accessibility, selection, translation and the tweak layer addressing "this
// paragraph" as one thing, and it keeps reflow available, while still letting a
// renderer draw Chrome's exact breaks when the basis still holds.

TEST_CASE("a wrapped run keeps its whole text and carries the line boxes",
          "[browser-capture][text-metrics]") {
    const auto result = lower_wrap_fixture();
    const auto* wrapped = find_text_node(root_of(result), "Chrome breaks this");
    REQUIRE(wrapped != nullptr);

    // Still ONE node with the WHOLE paragraph — not shredded into fragments.
    CHECK(wrapped->text_content.find("holds.") != std::string::npos);
    CHECK(wrapped->children.empty());

    REQUIRE(wrapped->text_line_boxes.size() == 5);
    const std::vector<float> expected_width{
        147.109375f, 191.75f, 219.5625f, 197.34375f, 45.78125f};
    for (size_t i = 0; i < wrapped->text_line_boxes.size(); ++i) {
        const auto& box = wrapped->text_line_boxes[i];
        CHECK_THAT(static_cast<double>(box.width),
                   WithinAbs(static_cast<double>(expected_width[i]), 0.001));
        // Relative to the run's own box, and stepping by its line height.
        CHECK_THAT(static_cast<double>(box.top),
                   WithinAbs(20.0 * static_cast<double>(i), 0.001));
    }
}

TEST_CASE("cached line boxes carry the basis that makes them checkable",
          "[browser-capture][text-metrics]") {
    // Boxes without a basis are not a cache: nothing can decide whether they
    // still apply, and adopting a foreign layout on trust is the failure this
    // design exists to avoid.
    const auto result = lower_wrap_fixture();
    const auto* wrapped = find_text_node(root_of(result), "Chrome breaks this");
    REQUIRE(wrapped != nullptr);
    REQUIRE(wrapped->text_layout_basis.has_value());

    // The width the text was broken at — the auto-width case depends on this.
    CHECK_THAT(static_cast<double>(wrapped->text_layout_basis->width),
               WithinAbs(219.5625, 0.001));
    // The face Blink SHAPED with, not the family it was asked for. The fixture
    // serves its own Inter, so this is the repository's own file.
    CHECK(wrapped->text_layout_basis->resolved_face == "Inter-Regular");
}

TEST_CASE("a single-line run is cached too", "[browser-capture][text-metrics]") {
    // A one-box run is not a trivial case to skip — it is the assertion "this
    // text did not break". An auto-width label's box was sized BY its text, so
    // it has no slack and any positive epsilon in a re-derived advance pushes
    // the last word onto a second line.
    const auto result = lower_wrap_fixture();
    const auto* unwrapped =
        find_text_node(root_of(result), "Chrome breaks this", 1);
    REQUIRE(unwrapped != nullptr);
    REQUIRE(unwrapped->text_line_boxes.size() == 1);
    CHECK(unwrapped->text_line_boxes[0].length == 108);
    REQUIRE(unwrapped->text_layout_basis.has_value());
    CHECK(unwrapped->text_layout_basis->resolved_face == "Inter-Regular");
}

// ── Advance census over a real captured design ─────────────────────────────

// Chrome's per-line box width is the only oracle for "is our text the right
// width", and the question it answers is a distribution, not an example: a
// systematic overhang across every run is a different defect from a handful of
// outliers, and they need different fixes. This walks a capture directory,
// shapes each line's exact substring through the same TextShaper the layout
// path measures with, and prints the error against Chrome's box.
//
// The per-gap convention is reported alongside the shipping per-character one,
// because one trailing step is the difference between a run that fits and a run
// that overruns its box.
//
// Hidden by default: it needs a capture that is not committed, and selecting
// it without one FAILS rather than passing quietly.
//   PULP_TEXT_ADVANCE_CENSUS_CAPTURE=<capture-dir> \
//     pulp-test-browser-capture-import "[.advance-census]" -s
TEST_CASE("advance census against Chrome's line boxes", "[.advance-census]") {
    const char* directory = std::getenv("PULP_TEXT_ADVANCE_CENSUS_CAPTURE");
    REQUIRE(directory != nullptr);
    const auto snapshot = fs::path(directory) / "dom-snapshot.json";
    REQUIRE(fs::is_regular_file(snapshot));
    auto loaded = CapturedStyleIndex::load(snapshot);
    REQUIRE(loaded.has_value());
    const auto& index = *loaded;

    struct Sample {
        std::string text, family, face;
        double size = 0, spacing = 0;
        int weight = 400, glyphs = 0;
        double chrome = 0, ours = 0, ours_per_gap = 0, scale = 1.0;
    };
    std::vector<Sample> samples;

    // A transformed wrapper is itself laid out, so the painted set carries the
    // layout index every ancestor scale is read from.
    std::map<int, int> layout_of_node;
    for (const auto& node : index.painted_nodes())
        layout_of_node.emplace(node.node_index, node.layout_index);

    pulp::canvas::TextShaper shaper;
    for (const auto& node : index.painted_nodes()) {
        if (node.node_type != 3 || node.text.empty()) continue;
        const auto boxes = index.text_boxes_for_layout(node.layout_index);
        if (boxes.empty()) continue;
        const auto styles = index.styles_for_layout(node.layout_index);
        const auto value = [&](const char* key) -> std::string {
            auto it = styles.find(key);
            return it == styles.end() ? std::string() : it->second;
        };
        const double size = std::atof(value("font-size").c_str());
        if (size <= 0) continue;
        const std::string family = value("font-family");
        if (family.empty()) continue;
        int weight = std::atoi(value("font-weight").c_str());
        if (weight <= 0) weight = 400;
        const std::string spacing_text = value("letter-spacing");
        const double spacing =
            spacing_text == "normal" ? 0.0 : std::atof(spacing_text.c_str());
        const double scale =
            inherited_scale_x(index, layout_of_node, node.node_index);
        if (scale <= 0.0) continue;

        for (const auto& box : boxes) {
            if (box.length <= 0 || box.bounds.width <= 0) continue;
            if (box.start < 0 ||
                box.start + box.length > static_cast<int>(node.text.size()))
                continue;
            // Chrome reports `start`/`length` in UTF-16 code units, so on a
            // run with non-ASCII text a byte-indexed substring lands mid
            // sequence. Snap both ends out to codepoint boundaries; a split
            // sequence is not text and shaping it proves nothing.
            const auto snap = [&](size_t at) {
                while (at > 0 && at < node.text.size() &&
                       (static_cast<unsigned char>(node.text[at]) & 0xC0) == 0x80)
                    --at;
                return at;
            };
            const size_t begin = snap(static_cast<size_t>(box.start));
            const size_t end = snap(static_cast<size_t>(box.start + box.length));
            if (end <= begin) continue;
            std::string line = node.text.substr(begin, end - begin);

            // Chrome's box excludes a line's trailing collapsed whitespace;
            // measuring it would report an overhang that is not there.
            while (!line.empty() &&
                   std::isspace(static_cast<unsigned char>(line.back())))
                line.pop_back();
            if (line.empty()) continue;

            int glyphs = 0;
            for (unsigned char c : line)
                if ((c & 0xC0) != 0x80) ++glyphs;

            const double base = shaper
                                    .prepare(line, family,
                                             static_cast<float>(size), weight)
                                    .total_width();
            samples.push_back(Sample{
                line, family, index.resolved_face_for_layout(node.layout_index),
                size, spacing, weight, glyphs, box.bounds.width / scale,
                base + spacing * glyphs, base + spacing * (glyphs - 1), scale});
        }
    }
    REQUIRE_FALSE(samples.empty());

    const auto report = [](const char* label, std::vector<double> errors) {
        if (errors.empty()) return;
        std::sort(errors.begin(), errors.end());
        const auto at = [&](double q) {
            return errors[std::min(errors.size() - 1,
                                   static_cast<size_t>(q * errors.size()))];
        };
        WARN(label << "  n=" << errors.size()
                   << "  p05=" << at(0.05) << "%  median=" << at(0.50)
                   << "%  p95=" << at(0.95) << "%  max=" << errors.back() << "%");
    };
    const auto errors_of = [&](bool per_gap,
                               const std::function<bool(const Sample&)>& keep) {
        std::vector<double> out;
        for (const auto& s : samples) {
            if (!keep(s)) continue;
            const double ours = per_gap ? s.ours_per_gap : s.ours;
            out.push_back(100.0 * (ours / s.chrome - 1.0));
        }
        return out;
    };
    const auto all = [](const Sample&) { return true; };
    const auto spaced = [](const Sample& s) { return s.spacing != 0.0; };
    const auto plain = [](const Sample& s) { return s.spacing == 0.0; };

    WARN("lines measured: " << samples.size());
    report("all                      ", errors_of(false, all));
    report("no letter-spacing        ", errors_of(false, plain));
    report("letter-spaced            ", errors_of(false, spaced));
    report("letter-spaced, per-gap   ", errors_of(true, spaced));

    // Per-line detail as TSV, because Catch2 wraps a WARN to the console width
    // and a wrapped row cannot be parsed. Worst first, so an outlier can be
    // named rather than inferred from a percentile.
    std::sort(samples.begin(), samples.end(), [](const auto& a, const auto& b) {
        return std::abs(a.ours / a.chrome - 1.0) >
               std::abs(b.ours / b.chrome - 1.0);
    });
    if (const char* out = std::getenv("PULP_TEXT_ADVANCE_CENSUS_OUT")) {
        std::ofstream tsv(out, std::ios::binary);
        REQUIRE(tsv.good());
        tsv << "err_pct\terr_pct_per_gap\tchrome\tours\tweight\tsize"
               "\tspacing\tglyphs\tscale\tface\tfamily\ttext\n";
        for (const auto& s2 : samples) {
            std::string flat = s2.text;
            for (auto& c : flat)
                if (c == '\t' || c == '\n') c = ' ';
            tsv << 100.0 * (s2.ours / s2.chrome - 1.0) << '\t'
                << 100.0 * (s2.ours_per_gap / s2.chrome - 1.0) << '\t'
                << s2.chrome << '\t' << s2.ours << '\t' << s2.weight << '\t'
                << s2.size << '\t' << s2.spacing << '\t' << s2.glyphs << '\t'
                << s2.scale << '\t'
                << (s2.face.empty() ? "<none>" : s2.face) << '\t' << s2.family
                << '\t' << flat << '\n';
        }
    }

    // A capture with no platform-fonts sidecar cannot say which face Blink
    // shaped with, and an advance compared against an unknown face measures
    // nothing about our shaping. Say so rather than reporting the number.
    size_t with_face = 0;
    for (const auto& s2 : samples)
        if (!s2.face.empty()) ++with_face;
    WARN("lines with a resolved face: " << with_face << " of "
                                        << samples.size());
}

// ── Type under an ancestor transform ───────────────────────────────────────

namespace {

fs::path type_scale_dir() {
    return fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) /
           "browser-capture-type-scale";
}

CapturedStyleIndex load_type_scale_fixture() {
    auto index = CapturedStyleIndex::load(type_scale_dir() / "dom-snapshot.json");
    REQUIRE(index.has_value());
    return std::move(*index);
}

BrowserCaptureIrResult lower_type_scale_fixture() {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    return lower_browser_capture_to_ir(type_scale_dir() / "capture.json",
                                       options);
}

/// The node index of the Nth painted text run whose text starts with `prefix`.
int node_of_text(const CapturedStyleIndex& index, const std::string& prefix,
                 size_t occurrence = 0) {
    size_t seen = 0;
    for (const auto& node : index.painted_nodes()) {
        if (node.node_type != 3 || node.text.rfind(prefix, 0) != 0) continue;
        if (seen++ == occurrence) return node.node_index;
    }
    FAIL("no painted text run number " << occurrence << " starting " << prefix);
    return -1;
}

} // namespace

// The fixture is a real capture of one string rendered twice at an identical
// computed style — `font-size: 40px`, `letter-spacing: 2px` — where the only
// difference is that one copy sits inside `transform: scale(0.5)`. Chrome
// reports 143.188px for it and 286.375px for the copy outside, exactly half,
// because the browser scales the glyphs along with the box.
//
// That control is what makes the assertion unfakeable: a lowering that ignores
// the transform gives both runs the same font-size, and no arithmetic over the
// computed style alone can tell them apart.
TEST_CASE("Chrome halves a run's line box under scale(0.5) at one font-size",
          "[browser-capture][text-metrics][transform]") {
    const auto index = load_type_scale_fixture();
    const auto boxes_for = [&](size_t occurrence) {
        for (const auto& node : index.painted_nodes()) {
            if (node.node_type != 3 ||
                node.text.rfind("Hamburgefons", 0) != 0)
                continue;
            if (occurrence-- == 0)
                return index.text_boxes_for_layout(node.layout_index);
        }
        FAIL("fixture lost a Hamburgefons run");
        return std::vector<CapturedTextBox>{};
    };
    const auto inside = boxes_for(0);
    const auto outside = boxes_for(1);
    REQUIRE(inside.size() == 1);
    REQUIRE(outside.size() == 1);
    CHECK_THAT(inside[0].bounds.width * 2.0,
               WithinAbs(outside[0].bounds.width, 0.01));
}

TEST_CASE("An ancestor transform chain reduces to one uniform type scale",
          "[browser-capture][text-metrics][transform]") {
    const auto index = load_type_scale_fixture();

    const auto scaled =
        index.inherited_type_scale(node_of_text(index, "Hamburgefons", 0));
    CHECK(scaled.ok());
    CHECK_THAT(scaled.scale, WithinAbs(0.5, 1e-6));

    const auto plain =
        index.inherited_type_scale(node_of_text(index, "Hamburgefons", 1));
    CHECK(plain.ok());
    CHECK_THAT(plain.scale, WithinAbs(1.0, 1e-6));
}

// Refusals, from both shapes that cannot become a font-size. A wrong-but-
// plausible number here would be invisible; the refusal names the value.
TEST_CASE("A transform type cannot carry is refused, not approximated",
          "[browser-capture][text-metrics][transform]") {
    const auto index = load_type_scale_fixture();
    const auto squashed =
        index.inherited_type_scale(node_of_text(index, "Squashed"));
    CHECK_FALSE(squashed.ok());
    CHECK(squashed.refused.find("matrix(0.5, 0, 0, 1.5") == 0);
    // The scale is left at identity rather than half-applied on one axis.
    CHECK_THAT(squashed.scale, WithinAbs(1.0, 1e-6));

    // A rotation is the other shape, and its own fixture already carries one.
    auto rotated = CapturedStyleIndex::load(
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) /
        "browser-capture-clip-transform" / "dom-snapshot.json");
    REQUIRE(rotated.has_value());
    const auto label = node_of_text(*rotated, "ROT");
    const auto spun = rotated->inherited_type_scale(label);
    CHECK_FALSE(spun.ok());
    CHECK(spun.refused.rfind("matrix(0.707107", 0) == 0);
}

TEST_CASE("Lowering folds the ancestor scale into the type lengths",
          "[browser-capture][text-metrics][transform]") {
    const auto result = lower_type_scale_fixture();
    const auto& root = root_of(result);

    const auto* inside = find_text_node(root, "Hamburgefons", 0);
    const auto* outside = find_text_node(root, "Hamburgefons", 1);
    REQUIRE(inside != nullptr);
    REQUIRE(outside != nullptr);
    REQUIRE(inside->style.font_size.has_value());
    REQUIRE(outside->style.font_size.has_value());

    // The control keeps the authored size; the scaled copy carries the factor
    // its box already carried. Both are asserted, so a lowering that scaled
    // EVERYTHING would fail here rather than look correct on one node.
    CHECK_THAT(*outside->style.font_size, WithinAbs(40.0f, 0.01f));
    CHECK_THAT(*inside->style.font_size, WithinAbs(20.0f, 0.01f));
    REQUIRE(inside->style.letter_spacing.has_value());
    REQUIRE(outside->style.letter_spacing.has_value());
    CHECK_THAT(*outside->style.letter_spacing, WithinAbs(2.0f, 0.01f));
    CHECK_THAT(*inside->style.letter_spacing, WithinAbs(1.0f, 0.01f));

    // And the refused node keeps its authored size, with the reason recorded
    // on the node rather than only in an aggregate count.
    const auto* squashed = find_text_node(root, "Squashed");
    REQUIRE(squashed != nullptr);
    REQUIRE(squashed->style.font_size.has_value());
    CHECK_THAT(*squashed->style.font_size, WithinAbs(20.0f, 0.01f));
    const auto refused = squashed->attributes.find("type_scale_refused");
    REQUIRE(refused != squashed->attributes.end());
    CHECK(refused->second.rfind("matrix(0.5, 0, 0, 1.5", 0) == 0);
}

TEST_CASE("a capture with no resolved face says so out loud",
          "[browser-capture][native-lowering][text-metrics]") {
    // The same capture, byte-identical, with ONLY platform-fonts.json removed —
    // the shape of a snapshot taken before the capture collected resolved
    // faces. The runs still carry their line boxes, so nothing about the output
    // looks incomplete; but the renderer refuses a basis with no face, so every
    // run re-derives its own line breaking and a run resuming mid-line after an
    // inline <span> prints over its sibling. That happened on the delay panel
    // and read as a text-layout bug for a whole debugging round.
    //
    // Held against the LIVE fixture in the same case: the claim is a difference
    // between two inputs, and asserting one alone would pass just as well if
    // the signal had stopped working entirely.
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    const auto stale = lower_browser_capture_to_ir(
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) /
            "browser-capture-text-stale-face" / "capture.json",
        options);
    REQUIRE(stale.design_ir);
    const auto live = lower_wrap_fixture();
    REQUIRE(live.design_ir);

    // Live: no signal, and nothing warned about.
    CHECK(live.design_ir->root.attributes.count("native_text_stale_capture") ==
          0);

    // Stale: counted, and counted per RUN rather than as a bare flag, so a
    // reader can tell one unlucky node from a whole panel.
    const auto& attrs = stale.design_ir->root.attributes;
    REQUIRE(attrs.count("native_text_stale_capture") == 1);
    CHECK(std::stoi(attrs.at("native_text_stale_capture")) > 0);

    // The warning has to name the ACTION. Nothing about the design tells a
    // reader that re-capturing is the fix.
    const bool warned = std::any_of(
        stale.warnings.begin(), stale.warnings.end(),
        [](const std::string& w) {
            return w.find("resolved-font-face") != std::string::npos &&
                   w.find("Re-run the browser capture") != std::string::npos;
        });
    CHECK(warned);
}

TEST_CASE("the line-break basis is the face that shaped the text, not the first listed",
          "[browser-capture][native-lowering][text-metrics]") {
    // Chrome does not order `resolved` by primacy. Across the corpus, every run
    // mixing a primary family with a fallback glyph lists the FALLBACK first:
    // a Jost paragraph containing one `→` reports LucidaGrande with one glyph
    // ahead of Jost-Regular with the other eighty.
    //
    // Reading resolved[0] stored the one-glyph face as the basis for the whole
    // paragraph. Native resolution of Jost can never equal LucidaGrande, so the
    // captured line breaking was rejected on every render and the run reflowed —
    // which on delay drew two lines of an annotation on top of each other.
    //
    // The two fixtures differ in exactly one file, so a changed verdict can be
    // attributed to the ordering and to nothing else.
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    const auto fallback_first = lower_browser_capture_to_ir(
        fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) /
            "browser-capture-text-fallback-first" / "capture.json",
        options);
    REQUIRE(fallback_first.design_ir);
    const auto normal = lower_wrap_fixture();
    REQUIRE(normal.design_ir);

    std::vector<std::string> expected, actual;
    const std::function<void(const pulp::view::IRNode&,
                             std::vector<std::string>&)> collect =
        [&](const pulp::view::IRNode& node, std::vector<std::string>& out) {
            if (node.text_layout_basis)
                out.push_back(node.text_layout_basis->resolved_face);
            for (const auto& child : node.children) collect(child, out);
        };

    collect(normal.design_ir->root, expected);
    collect(fallback_first.design_ir->root, actual);
    REQUIRE_FALSE(expected.empty());
    REQUIRE(actual.size() == expected.size());

    // Same answer from both orderings: the rule reads glyph counts, not
    // position. Asserting only "not LucidaGrande" would pass for an empty
    // string too, which is the failure this whole basis exists to avoid.
    for (std::size_t i = 0; i < actual.size(); ++i) {
        INFO("run " << i);
        CHECK(actual[i] == expected[i]);
        CHECK(actual[i] != "LucidaGrande");
        CHECK_FALSE(actual[i].empty());
    }
}
