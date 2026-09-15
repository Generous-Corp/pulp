// The SelectableText capability: the painted-line geometry a text-bearing
// widget exposes, and the shared hit-test / rect arithmetic over it.
//
// ── Detection floors, stated up front ───────────────────────────────────────
//
// These are GEOMETRIC assertions over `RecordingCanvas`, whose text metric is a
// deterministic 7.0px per byte (`RecordingCanvas::measure_text`) and whose
// `text_x_for_byte` is that metric applied to the UTF-8-clamped prefix. So:
//
//  * Horizontal resolution is ONE BYTE = 7.0px. A defect that moves a boundary
//    by less than 3.5px (half a cell) cannot flip a hit-test result and is
//    invisible here. `ShapedOffsetCanvas` (test/support/text_editor_test_utils.hpp)
//    covers the one sub-cell class that matters — shaped offsets vs re-summed
//    isolated glyph advances — and is used below for exactly that.
//  * These assertions are over BOXES, never ink. A defect that leaves every box
//    correct and moves the glyphs inside it is BELOW THIS FLOOR and would
//    pass every case in this file. Nothing here should be read
//    as evidence about where ink landed; that is the text-metrics suites' job.
//  * Vertical assertions are band-membership only, never exact baselines: the
//    ascent a Label resolves depends on whether Skia is present in the build.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/selectable_text.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widgets.hpp>

#include "support/text_editor_test_utils.hpp"

#include <string>

using namespace pulp::view;
using pulp::canvas::RecordingCanvas;

namespace {
/// Width of one byte under RecordingCanvas's text metric. See the floor note.
constexpr float kByteW = 7.0f;
}  // namespace

TEST_CASE("Label exposes painted line geometry through SelectableText",
          "[view][widget][label][selection]") {
    Label label("hello world");
    label.set_bounds({0, 0, 200, 20});

    SECTION("a Label outside any content region advertises no capability") {
        REQUIRE(label.as_selectable_text() == nullptr);
    }

    label.set_selection_policy(Label::SelectionPolicy::always);
    REQUIRE(label.as_selectable_text() == &label);

    SECTION("before any paint the layout is UNMEASURED and not empty") {
        const auto layout = label.selectable_layout();
        REQUIRE_FALSE(layout.measured);
        // The distinction the `measured` flag exists for: an unmeasured layout
        // must not answer a hit-test with a plausible-looking 0.
        REQUIRE(selectable_index_at_point(layout, {50.0f, 10.0f}) == -1);
        REQUIRE(selectable_first_index(layout) == -1);
        REQUIRE(selectable_last_index(layout) == -1);
    }

    RecordingCanvas canvas;
    label.paint(canvas);

    SECTION("after paint the layout carries one boundary per cluster plus the end") {
        const auto layout = label.selectable_layout();
        REQUIRE(layout.measured);
        REQUIRE(layout.lines.size() == 1);
        const auto& line = layout.lines.front();
        REQUIRE(line.byte_offsets.size() == std::string("hello world").size() + 1);
        REQUIRE(line.byte_offsets.front() == 0);
        REQUIRE(line.byte_offsets.back() == 11);
        REQUIRE(line.x_offsets.front() == Catch::Approx(0.0f));
        REQUIRE(line.x_offsets.back() == Catch::Approx(11 * kByteW));
    }

    SECTION("pixel to byte offset resolves to the nearest cluster boundary") {
        const auto layout = label.selectable_layout();
        REQUIRE(selectable_index_at_point(layout, {6 * kByteW, 10.0f}) == 6);
        REQUIRE(selectable_index_at_point(layout, {6 * kByteW + 3.0f, 10.0f}) == 6);
        REQUIRE(selectable_index_at_point(layout, {6 * kByteW + 4.0f, 10.0f}) == 7);
        // Past either end clamps rather than failing — a drag that has left the
        // widget still has to resolve to an offset in it.
        REQUIRE(selectable_index_at_point(layout, {-500.0f, 10.0f}) == 0);
        REQUIRE(selectable_index_at_point(layout, {5000.0f, 10.0f}) == 11);
        REQUIRE(selectable_index_at_point(layout, {3 * kByteW, -900.0f}) == 3);
        REQUIRE(selectable_index_at_point(layout, {3 * kByteW, 900.0f}) == 3);
    }

    SECTION("rects for a range cover exactly the selected cells") {
        const auto layout = label.selectable_layout();
        const auto rects = selectable_rects_for_range(layout, 2, 5);
        REQUIRE(rects.size() == 1);
        REQUIRE(rects.front().x == Catch::Approx(2 * kByteW));
        REQUIRE(rects.front().width == Catch::Approx(3 * kByteW));
        // A collapsed range paints nothing — a caret is not a selection.
        REQUIRE(selectable_rects_for_range(layout, 4, 4).empty());
        // A reversed range is the same range.
        REQUIRE(selectable_rects_for_range(layout, 5, 2).size() == 1);
    }
}

TEST_CASE("Label selection geometry reads shaped offsets not summed glyph widths",
          "[view][widget][label][selection][text-metrics]") {
    // The control for the one-byte floor above. `ShapedOffsetCanvas`
    // deliberately makes `text_x_for_byte` disagree with the sum of isolated
    // `measure_text` calls, so a Label that re-derived its boundaries from
    // per-glyph advances would land on 8px cells at x=0 instead of 1px cells at
    // x=500. This is what asserts a kerned run is positioned as it is drawn.
    Label label("AVA");
    label.set_bounds({0, 0, 200, 20});
    label.set_selection_policy(Label::SelectionPolicy::always);

    pulp::test::ShapedOffsetCanvas canvas;
    label.paint(canvas);

    const auto layout = label.selectable_layout();
    REQUIRE(layout.measured);
    const auto& line = layout.lines.front();
    REQUIRE(line.x_offsets.size() == 4);
    REQUIRE(line.x_offsets[0] == Catch::Approx(500.0f));
    REQUIRE(line.x_offsets[3] == Catch::Approx(503.0f));
}

TEST_CASE("an empty Label is selectable-but-empty and not unmeasured",
          "[view][widget][label][selection][edge]") {
    // The distinction matters to any walk over several widgets: an empty
    // widget that reported `measured == false` would be indistinguishable from
    // one that has never painted, and the two call for opposite handling.
    Label label("");
    label.set_bounds({0, 0, 200, 20});
    label.set_selection_policy(Label::SelectionPolicy::always);

    RecordingCanvas canvas;
    label.paint(canvas);

    const auto layout = label.selectable_layout();
    REQUIRE(layout.measured);
    REQUIRE(layout.lines.size() == 1);
    REQUIRE(layout.lines.front().byte_offsets.size() == 1);
    REQUIRE(selectable_first_index(layout) == 0);
    REQUIRE(selectable_last_index(layout) == 0);
    REQUIRE(selectable_index_at_point(layout, {50.0f, 10.0f}) == 0);
}

TEST_CASE("a wrapped Label reports one entry per visual line",
          "[view][widget][label][selection][wrap]") {
    Label label("the quick brown fox jumps over the lazy dog");
    label.set_multi_line(true);
    label.set_bounds({0, 0, 80, 120});
    label.set_selection_policy(Label::SelectionPolicy::always);

    RecordingCanvas canvas;
    label.paint(canvas);

    const auto layout = label.selectable_layout();
    REQUIRE(layout.measured);
    // Break positions depend on the shaper, so assert the invariant rather
    // than the pixels: more than one line, non-overlapping ascending byte
    // spans, and every line's boundaries inside its own span.
    REQUIRE(layout.lines.size() > 1);
    int previous_end = -1;
    for (const auto& line : layout.lines) {
        REQUIRE(line.start_utf8 >= previous_end);
        REQUIRE(line.end_utf8 >= line.start_utf8);
        REQUIRE_FALSE(line.byte_offsets.empty());
        REQUIRE(line.byte_offsets.front() == line.start_utf8);
        REQUIRE(line.byte_offsets.back() == line.end_utf8);
        previous_end = line.end_utf8;
    }

    // A range crossing the first break paints a band on BOTH lines. A
    // single-rect answer here is the classic wrapped-selection bug.
    const int first_break = layout.lines.front().end_utf8;
    const auto rects = selectable_rects_for_range(layout, 1, first_break + 2);
    REQUIRE(rects.size() == 2);
    REQUIRE(rects[0].y < rects[1].y);
}

TEST_CASE("a multi-byte Label never offers a boundary inside a codepoint",
          "[view][widget][label][selection][utf8]") {
    // Every offset this capability publishes is a cluster boundary, so a range
    // sliced from it cannot cut a UTF-8 sequence in half on its way to the
    // clipboard.
    Label label("aé漢z");
    label.set_bounds({0, 0, 200, 20});
    label.set_selection_policy(Label::SelectionPolicy::always);

    RecordingCanvas canvas;
    label.paint(canvas);

    const auto layout = label.selectable_layout();
    REQUIRE(layout.measured);
    const std::string text = "aé漢z";
    for (int offset : layout.lines.front().byte_offsets) {
        REQUIRE(offset >= 0);
        REQUIRE(offset <= static_cast<int>(text.size()));
        if (offset < static_cast<int>(text.size())) {
            const auto byte = static_cast<unsigned char>(text[static_cast<std::size_t>(offset)]);
            INFO("offset " << offset << " byte 0x" << std::hex << int(byte));
            REQUIRE((byte & 0xC0) != 0x80);  // never a continuation byte
        }
    }
    // 'a'(1) + 'é'(2) + '漢'(3) + 'z'(1) = 4 clusters, 5 boundaries.
    REQUIRE(layout.lines.front().byte_offsets.size() == 5);
}

TEST_CASE("TextEditor publishes the same layout its own hit-test uses",
          "[view][text_editor][selection]") {
    TextEditor editor;
    editor.set_text("hello world");
    editor.set_bounds({0, 0, 200, 30});

    SECTION("an editable editor advertises no capability") {
        // An editable field owns its own selection the way an HTML <input>
        // does; a document selection must not swallow it.
        REQUIRE(editor.as_selectable_text() == nullptr);
    }

    editor.read_only = true;
    REQUIRE(editor.as_selectable_text() == &editor);

    SECTION("before paint the editor reports unmeasured rather than estimates") {
        // `char_index_at_point` falls back to an ESTIMATED advance with no
        // paint snapshot. Publishing those numbers would put a selection band
        // visibly off the glyphs, so the capability declines instead.
        REQUIRE_FALSE(editor.selectable_layout().measured);
    }

    RecordingCanvas canvas;
    editor.paint(canvas);

    SECTION("the two hit-tests agree across the whole run") {
        // The cross-check that matters: publishing the editor's private layout
        // snapshot through SelectableText must not fork the arithmetic, or a
        // Label and a read-only editor in one document would disagree about
        // where the pointer is.
        const auto layout = editor.selectable_layout();
        REQUIRE(layout.measured);
        for (float x = -20.0f; x < 140.0f; x += 1.0f) {
            INFO("x = " << x);
            REQUIRE(selectable_index_at_point(layout, {x, 15.0f}) ==
                    editor.char_index_at_point(x, 15.0f));
        }
    }

    SECTION("the highlight routes through the editor's own selection") {
        // One range, not two: `selected_text()` and `copy_to_clipboard()` must
        // answer for whatever the highlight shows.
        editor.set_selection_highlight(2, 7);
        REQUIRE(editor.has_selection());
        REQUIRE(editor.selected_text() == "llo w");
        int lo = 0, hi = 0;
        REQUIRE(editor.selection_highlight(lo, hi));
        REQUIRE(lo == 2);
        REQUIRE(hi == 7);
        editor.set_selection_highlight(3, 3);
        REQUIRE_FALSE(editor.selection_highlight(lo, hi));
    }
}

TEST_CASE("a Label paints a selection band only over the highlighted range",
          "[view][widget][label][selection][paint]") {
    Label label("hello world");
    label.set_bounds({0, 0, 200, 20});
    label.set_selection_policy(Label::SelectionPolicy::always);

    // Paint once so the layout is measured, then highlight and repaint.
    RecordingCanvas warm;
    label.paint(warm);
    label.set_selection_highlight(1, 4);

    RecordingCanvas canvas;
    label.paint(canvas);

    // Band GEOMETRY, not appearance: a filled rect spanning exactly bytes 1..4.
    // This says nothing about colour, nor about where the glyphs inside it
    // landed (see the ink-extent floor note at the top of this file).
    bool found = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != pulp::canvas::DrawCommand::Type::fill_rect) continue;
        if (cmd.f[0] == Catch::Approx(1 * kByteW) &&
            cmd.f[2] == Catch::Approx(3 * kByteW))
            found = true;
    }
    REQUIRE(found);

    // The negative control for that assertion, which would otherwise pass on
    // any stray rect: with the highlight cleared the Label fills nothing.
    label.set_selection_highlight(0, 0);
    RecordingCanvas clean;
    label.paint(clean);
    for (const auto& cmd : clean.commands())
        REQUIRE(cmd.type != pulp::canvas::DrawCommand::Type::fill_rect);
}

// ── Region scoping ──────────────────────────────────────────────────────────

TEST_CASE("text is selectable by default inside a declared content region",
          "[view][widget][label][selection][region]") {
    // The shape an author is meant to use: declare the region once, put prose
    // in it, and every Label inside is selectable with no per-widget wiring.
    auto make_label = [](const char* text, float y) {
        auto l = std::make_unique<Label>(text);
        l->set_bounds({0, y, 200, 20});
        return l;
    };

    View root;
    root.set_bounds({0, 0, 200, 100});

    auto region = std::make_unique<View>();
    region->set_bounds({0, 0, 200, 60});
    region->set_text_selection_region(true);
    auto prose = make_label("prose", 0);
    auto* prose_raw = prose.get();
    region->add_child(std::move(prose));
    root.add_child(std::move(region));

    auto readout = make_label("-12.0 dB", 70);
    auto* readout_raw = readout.get();
    root.add_child(std::move(readout));

    // Inside the region: selectable, with nothing set on the Label itself.
    REQUIRE(prose_raw->is_selectable());
    REQUIRE(prose_raw->as_selectable_text() == prose_raw);
    // Outside it: unchanged. This is the assertion that protects every
    // existing plugin UI from a drag on a readout becoming a text selection.
    REQUIRE_FALSE(readout_raw->is_selectable());
    REQUIRE(readout_raw->as_selectable_text() == nullptr);
}

TEST_CASE("a Label can opt out inside a region and in outside one",
          "[view][widget][label][selection][region]") {
    View root;
    root.set_bounds({0, 0, 200, 60});
    root.set_text_selection_region(true);

    auto prose = std::make_unique<Label>("prose");
    prose->set_bounds({0, 0, 200, 20});
    auto* prose_raw = prose.get();
    root.add_child(std::move(prose));
    REQUIRE(prose_raw->is_selectable());

    // A live readout sitting inside a prose panel is not prose.
    prose_raw->set_selection_policy(Label::SelectionPolicy::never);
    REQUIRE_FALSE(prose_raw->is_selectable());
    REQUIRE(prose_raw->as_selectable_text() == nullptr);

    Label lone("standalone");
    lone.set_bounds({0, 0, 200, 20});
    REQUIRE_FALSE(lone.is_selectable());
    lone.set_selection_policy(Label::SelectionPolicy::always);
    REQUIRE(lone.is_selectable());
}

TEST_CASE("region membership follows a reparent with no notification",
          "[view][widget][label][selection][region]") {
    // `is_selectable()` resolves by walking to the nearest region rather than
    // caching a flag, so moving a Label into or out of content cannot leave a
    // stale answer behind — the failure mode a cached bool would have.
    View region;
    region.set_bounds({0, 0, 200, 60});
    region.set_text_selection_region(true);
    View plain;
    plain.set_bounds({0, 0, 200, 60});

    auto label = std::make_unique<Label>("text");
    label->set_bounds({0, 0, 200, 20});
    auto* raw = label.get();
    plain.add_child(std::move(label));
    REQUIRE_FALSE(raw->is_selectable());

    auto moved = plain.remove_child(raw);
    region.add_child(std::move(moved));
    REQUIRE(raw->is_selectable());
}

TEST_CASE("an unselectable Label records no selection geometry at all",
          "[view][widget][label][selection][region][perf]") {
    // Outside a region the recorder must not run: it calls
    // `canvas.text_x_for_byte()` once per cluster, which is real per-paint
    // work on every Label in every Pulp UI if it is not gated.
    Label label("hello world");
    label.set_bounds({0, 0, 200, 20});

    RecordingCanvas canvas;
    label.paint(canvas);
    REQUIRE_FALSE(label.selectable_layout().measured);
    REQUIRE(label.selectable_layout().lines.empty());

    // The positive control: the same Label, opted in, does record.
    label.set_selection_policy(Label::SelectionPolicy::always);
    RecordingCanvas warm;
    label.paint(warm);
    REQUIRE(label.selectable_layout().measured);
    REQUIRE_FALSE(label.selectable_layout().lines.empty());
}

TEST_CASE("row resolution is defined for gapped and zero-height bands",
          "[view][selection][edge]") {
    // `selectable_index_at_point` is a public free function over a
    // caller-supplied `SelectableLayout`, so a layout Label and TextEditor
    // never produce is still inside its contract — a future text widget, or a
    // caller assembling a layout by hand, can hand it these.
    //
    // This is the case an earlier three-branch implementation left to no
    // branch at all: its guards established only `front().top < y < last
    // bottom`, which a gap satisfies while matching no row, and that path fell
    // off the end of a non-void function.
    SelectableLayout layout;
    layout.measured = true;

    SelectableLine top;
    top.start_utf8 = 0; top.end_utf8 = 2;
    top.top = 0.0f; top.height = 10.0f;
    top.byte_offsets = {0, 1, 2};
    top.x_offsets = {0.0f, 10.0f, 20.0f};

    SelectableLine bottom;
    bottom.start_utf8 = 3; bottom.end_utf8 = 5;
    bottom.top = 20.0f; bottom.height = 10.0f;   // a 10px GAP above it
    bottom.byte_offsets = {3, 4, 5};
    bottom.x_offsets = {0.0f, 10.0f, 20.0f};

    layout.lines = {top, bottom};

    SECTION("a point inside the gap resolves to the nearer band") {
        REQUIRE(selectable_index_at_point(layout, {0.0f, 12.0f}) == 0);
        REQUIRE(selectable_index_at_point(layout, {0.0f, 18.0f}) == 3);
        // Dead centre of the gap is a tie, and ties resolve downward.
        REQUIRE(selectable_index_at_point(layout, {0.0f, 15.0f}) == 3);
    }

    SECTION("a point on the seam belongs to the lower band") {
        SelectableLayout contiguous = layout;
        contiguous.lines[1].top = 10.0f;
        REQUIRE(selectable_index_at_point(contiguous, {0.0f, 10.0f}) == 3);
    }

    SECTION("a zero-height band is still reachable") {
        SelectableLayout flat = layout;
        flat.lines[0].height = 0.0f;
        // y == 0 is exactly on the zero-height band, and 0 is nearer to it
        // than to the band starting at 20. Containment alone could never
        // return this row: no y satisfies `y >= top && y < top + 0`.
        REQUIRE(selectable_index_at_point(flat, {0.0f, 0.0f}) == 0);
    }

    SECTION("clamping past both ends still works") {
        REQUIRE(selectable_index_at_point(layout, {0.0f, -500.0f}) == 0);
        REQUIRE(selectable_index_at_point(layout, {20.0f, 5000.0f}) == 5);
    }
}
