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

TEST_CASE("a run continuing an earlier sibling's line refuses to wrap",
          "[browser-capture][text-metrics]") {
    // On `delay` this class is 8 of 277 runs. Wrapping one lays its first line
    // where the sibling's text already is, so the two overprint; a single Label
    // cannot express "start mid-line, then return to the left edge". The
    // lowering marks them nowrap until per-line-box lowering exists.
    //
    // The wrap fixture has no inline continuation — every run starts its own
    // line — so the guard must leave all of its wrapping runs alone. That is
    // the half of the contract this fixture can prove: no false positives.
    const auto result = lower_wrap_fixture();
    const auto* wrapped = find_text_node(root_of(result), "Chrome breaks this");
    REQUIRE(wrapped != nullptr);
    // REQUIRE, not CHECK: dereferencing an unset optional below is undefined
    // and prints a plausible-looking garbage string rather than failing here.
    REQUIRE(wrapped->style.white_space.has_value());
    CHECK(*wrapped->style.white_space == "normal");

    const auto index = load_wrap_fixture();
    for (const auto& node : index.painted_nodes()) {
        const auto boxes = index.text_boxes_for_layout(node.layout_index);
        if (boxes.size() <= 1) continue;
        CHECK(boxes.front().bounds.left <= node.bounds.left + 1.0);
    }
}
