// Cross-widget text selection: the capability interface (SelectableText), the
// shared hit-test/rect arithmetic over it, and the document-level selection
// owner that walks a subtree in document order.
//
// ── Detection floors, stated up front ───────────────────────────────────────
//
// These are GEOMETRIC assertions over `RecordingCanvas`, whose text metrics are
// a deterministic 7.0px per byte (`RecordingCanvas::measure_text`) and whose
// `text_x_for_byte` is that measure applied to the UTF-8-clamped prefix. So:
//
//  * Horizontal resolution is ONE BYTE = 7.0px. A defect that moves a boundary
//    less than 3.5px (half a cell) cannot flip a hit-test result and is
//    invisible here. Sub-glyph placement, kerning and shaped-vs-summed advance
//    divergence are all below that floor — `ShapedOffsetCanvas` in
//    test/support/text_editor_test_utils.hpp exists for exactly that class of
//    defect and is used below to prove the Label path reads SHAPED offsets
//    rather than re-summing glyph widths.
//  * These assertions are over BOXES, not ink. A defect that leaves every box
//    correct and moves the glyphs inside them (pulp #8390's shape) is BELOW
//    THIS FLOOR and would pass every test in this file. Ink-extent coverage
//    for Label lives in the text-metrics suites; nothing here substitutes for
//    it, and no assertion here should be read as proving where ink landed.
//  * Vertical assertions are band-membership only, never exact baselines: the
//    ascent a Label resolves depends on whether Skia is present in the build.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/selectable_text.hpp>
#include <pulp/view/text_selection.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widgets.hpp>

#include "support/text_editor_test_utils.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::RecordingCanvas;

namespace {

/// Width of one byte under RecordingCanvas's text metric. See the floor note.
constexpr float kByteW = 7.0f;

/// A root with `n` selectable Labels stacked vertically, each 200x20.
struct Doc {
    View root;
    std::vector<Label*> labels;

    explicit Doc(const std::vector<std::string>& texts) {
        root.set_bounds({0, 0, 200, static_cast<float>(24 * texts.size())});
        // One declaration around the content. Nothing is set on the Labels —
        // that is the point of the region: text inside it is selectable by
        // default, and an author wires nothing per node.
        root.set_text_selection_region(true);
        float y = 0;
        for (const auto& t : texts) {
            auto l = std::make_unique<Label>(t);
            l->set_bounds({0, y, 200, 20});
            labels.push_back(l.get());
            root.add_child(std::move(l));
            y += 24;
        }
    }

    /// Paint every Label so each reports a measured layout. Nothing in this
    /// file may assert geometry before this runs — an unpainted widget
    /// correctly reports `measured == false`, and a test that skipped the
    /// paint would be asserting against the unmeasured fallback instead.
    void paint() {
        for (auto* l : labels) {
            RecordingCanvas c;
            l->paint(c);
        }
    }

    /// Root-space point at byte boundary `byte` of label `i`.
    Point at(std::size_t i, int byte, float dy = 10.0f) const {
        return {static_cast<float>(byte) * kByteW,
                labels[i]->bounds().y + dy};
    }
};

}  // namespace

// ── Document-order traversal ────────────────────────────────────────────────

TEST_CASE("document order is tree order and skips widgets that opted out",
          "[view][selection][document-order]") {
    Doc doc({"alpha", "bravo", "charlie"});
    REQUIRE(selectable_widgets_in_document_order(doc.root).size() == 3);

    doc.labels[1]->set_selection_policy(Label::SelectionPolicy::never);
    const auto order = selectable_widgets_in_document_order(doc.root);
    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == doc.labels[0]);
    REQUIRE(order[1] == doc.labels[2]);
}

TEST_CASE("a selection from A to C fully selects the B the pointer never touched",
          "[view][selection][document-order]") {
    Doc doc({"alpha", "bravo", "charlie"});
    doc.paint();

    // Press inside "alpha" at byte 2, release inside "charlie" at byte 4.
    auto a = doc.labels[0]->as_selectable_text();
    auto c = doc.labels[2]->as_selectable_text();
    selection_begin(doc.root, *a, 2);
    selection_extend(doc.root, *c, 4);
    selection_end_drag(doc.root);

    REQUIRE(selection_has_range(doc.root));
    REQUIRE(selection_spans_multiple_widgets(doc.root));
    // "pha" + "\n" + "bravo" + "\n" + "char" — the middle widget contributes
    // in FULL despite never being under the pointer. That is the whole
    // requirement; a naive walk over touched widgets emits "pha\nchar".
    REQUIRE(selection_text(doc.root) == "pha\nbravo\nchar");

    // And the middle widget's own highlight says the same thing.
    int lo = 0, hi = 0;
    REQUIRE(doc.labels[1]->selection_highlight(lo, hi));
    REQUIRE(lo == 0);
    REQUIRE(hi == 5);
}

// A Label opted out inside a region is skipped by the walk entirely, so a
// selection spanning it joins its neighbours directly rather than emitting a
// blank line where it sat.
TEST_CASE("a backwards selection yields the same text as a forwards one",
          "[view][selection][document-order]") {
    Doc doc({"alpha", "bravo", "charlie"});
    doc.paint();
    auto a = doc.labels[0]->as_selectable_text();
    auto c = doc.labels[2]->as_selectable_text();

    selection_begin(doc.root, *c, 4);
    selection_extend(doc.root, *a, 2);
    REQUIRE(selection_text(doc.root) == "pha\nbravo\nchar");
}

TEST_CASE("an empty widget between two others contributes only its separator",
          "[view][selection][document-order][edge]") {
    Doc doc({"alpha", "", "charlie"});
    doc.paint();

    // An empty Label still reports a MEASURED layout with one zero-width
    // boundary — it is selectable-but-empty, not unsupported. Without that a
    // document-order walk either skips it (losing the blank line) or treats it
    // as unmeasured and falls back to its whole string, which is also empty
    // but for the wrong reason.
    const auto layout = doc.labels[1]->selectable_layout();
    REQUIRE(layout.measured);
    REQUIRE(layout.lines.size() == 1);
    REQUIRE(layout.lines.front().byte_offsets.size() == 1);

    selection_begin(doc.root, *doc.labels[0]->as_selectable_text(), 2);
    selection_extend(doc.root, *doc.labels[2]->as_selectable_text(), 4);
    REQUIRE(selection_text(doc.root) == "pha\n\nchar");
}

TEST_CASE("a click with no drag selects nothing",
          "[view][selection][edge]") {
    Doc doc({"alpha", "bravo"});
    doc.paint();
    auto a = doc.labels[0]->as_selectable_text();

    selection_begin(doc.root, *a, 3);
    selection_extend(doc.root, *a, 3);
    selection_end_drag(doc.root);

    // A zero-length range is a caret, not a selection: it must not report a
    // range, must copy nothing, and must leave no highlight behind.
    REQUIRE_FALSE(selection_has_range(doc.root));
    REQUIRE(selection_text(doc.root).empty());
    int lo = 0, hi = 0;
    REQUIRE_FALSE(doc.labels[0]->selection_highlight(lo, hi));
}

TEST_CASE("selection survives a widget being unmounted mid-drag",
          "[view][selection][edge]") {
    Doc doc({"alpha", "bravo", "charlie"});
    doc.paint();
    selection_begin(doc.root, *doc.labels[0]->as_selectable_text(), 0);
    selection_extend(doc.root, *doc.labels[2]->as_selectable_text(), 7);
    REQUIRE(selection_has_range(doc.root));

    // Removing the FOCUS endpoint must not dangle. `ViewCapture` resolves it
    // to null and the selection reports no range rather than reading freed
    // memory or keeping a stale highlight on the survivors.
    auto removed = doc.root.remove_child(doc.labels[2]);
    REQUIRE_FALSE(selection_has_range(doc.root));
    REQUIRE(selection_text(doc.root).empty());
}

// ── Pointer-driven, through the real dispatch path ──────────────────────────

TEST_CASE("a pointer drag across Labels selects across them",
          "[view][selection][drag]") {
    Doc doc({"alpha", "bravo", "charlie"});
    doc.paint();

    // Press in "alpha" at byte 2, drag into "charlie" at byte 4. The press
    // target is latched by the dispatcher, so every drag tick is delivered to
    // the FIRST Label with a local point that has left its bounds — the case
    // a per-widget selection cannot handle and this feature exists for.
    doc.root.simulate_drag(doc.at(0, 2), doc.at(2, 4), 6);

    REQUIRE(selection_has_range(doc.root));
    REQUIRE(selection_text(doc.root) == "pha\nbravo\nchar");
    REQUIRE_FALSE(selection_is_dragging(doc.root));
}

TEST_CASE("a drag past the last widget selects to the end of the document",
          "[view][selection][drag][edge]") {
    Doc doc({"alpha", "bravo"});
    doc.paint();

    // Far below every widget. The hit-test has to resolve to the LAST widget
    // and clamp to its end; returning "no target" would freeze the selection
    // wherever the pointer left the text, which reads as the drag being stuck.
    doc.root.simulate_drag(doc.at(0, 1), {5000.0f, 4000.0f}, 4);
    REQUIRE(selection_text(doc.root) == "lpha\nbravo");
}

TEST_CASE("a fresh press collapses the previous selection",
          "[view][selection][drag]") {
    Doc doc({"alpha", "bravo"});
    doc.paint();
    doc.root.simulate_drag(doc.at(0, 0), doc.at(1, 5), 4);
    REQUIRE(selection_has_range(doc.root));

    doc.root.simulate_click(doc.at(1, 2));
    REQUIRE_FALSE(selection_has_range(doc.root));
}

// ── Copy + keyboard ─────────────────────────────────────────────────────────

TEST_CASE("Cmd-C copies the whole cross-widget selection",
          "[view][selection][clipboard]") {
    pulp::test::require_system_clipboard_text("sentinel");

    Doc doc({"alpha", "bravo", "charlie"});
    doc.paint();
    doc.root.simulate_drag(doc.at(0, 2), doc.at(2, 4), 6);
    REQUIRE(selection_text(doc.root) == "pha\nbravo\nchar");

    // Through the real key path on the focused Label, not by calling
    // selection_copy() directly — the wiring is the thing under test.
    REQUIRE(doc.labels[0]->has_focus());
    auto copy = pulp::test::key_event(KeyCode::c, pulp::test::main_modifier());
    REQUIRE(doc.labels[0]->on_key_event(copy));

    auto pasted = pulp::platform::Clipboard::get_text();
    REQUIRE(pasted.has_value());
    REQUIRE(*pasted == "pha\nbravo\nchar");
}

TEST_CASE("a read-only TextEditor in a document selection copies all of it",
          "[view][selection][clipboard][text_editor]") {
    pulp::test::require_system_clipboard_text("sentinel");

    View root;
    root.set_bounds({0, 0, 200, 80});
    root.set_text_selection_region(true);
    auto lab = std::make_unique<Label>("alpha");
    lab->set_bounds({0, 0, 200, 20});
    auto* label = lab.get();
    root.add_child(std::move(lab));

    auto ed = std::make_unique<TextEditor>();
    ed->set_text("bravo");
    ed->read_only = true;
    ed->set_bounds({0, 24, 200, 30});
    auto* editor = ed.get();
    root.add_child(std::move(ed));

    RecordingCanvas c1;
    label->paint(c1);
    RecordingCanvas c2;
    editor->paint(c2);

    selection_begin(root, *label->as_selectable_text(), 0);
    selection_extend(root, *editor->as_selectable_text(), 5);
    REQUIRE(selection_spans_multiple_widgets(root));

    // The editor's own Cmd-C would copy "bravo" alone. Inside a multi-widget
    // selection it has to yield the whole thing, or the user silently loses
    // everything above the field they happened to be focused in.
    auto copy = pulp::test::key_event(KeyCode::c, pulp::test::main_modifier());
    REQUIRE(editor->on_key_event(copy));
    auto pasted = pulp::platform::Clipboard::get_text();
    REQUIRE(pasted.has_value());
    REQUIRE(*pasted == "alpha\nbravo");
}

TEST_CASE("a read-only TextEditor alone still copies only its own selection",
          "[view][selection][clipboard][text_editor]") {
    pulp::test::require_system_clipboard_text("sentinel");

    TextEditor editor;
    editor.set_text("hello world");
    editor.read_only = true;
    editor.set_bounds({0, 0, 200, 30});
    RecordingCanvas canvas;
    editor.paint(canvas);
    editor.set_selection_highlight(0, 5);

    auto copy = pulp::test::key_event(KeyCode::c, pulp::test::main_modifier());
    REQUIRE(editor.on_key_event(copy));
    auto pasted = pulp::platform::Clipboard::get_text();
    REQUIRE(pasted.has_value());
    REQUIRE(*pasted == "hello");
}

TEST_CASE("Escape clears the selection and Cmd-A selects the document",
          "[view][selection][keyboard]") {
    Doc doc({"alpha", "bravo"});
    doc.paint();

    auto select_all = pulp::test::key_event(KeyCode::a, pulp::test::main_modifier());
    REQUIRE(doc.labels[0]->on_key_event(select_all));
    REQUIRE(selection_text(doc.root) == "alpha\nbravo");

    auto escape = pulp::test::key_event(KeyCode::escape);
    REQUIRE(doc.labels[0]->on_key_event(escape));
    REQUIRE_FALSE(selection_has_range(doc.root));
    int lo = 0, hi = 0;
    REQUIRE_FALSE(doc.labels[1]->selection_highlight(lo, hi));
}


TEST_CASE("a selection cannot reach text outside its own content region",
          "[view][selection][region]") {
    // Two sibling regions in one tree. A drag that starts in the first must
    // not extend into the second, and text outside every region is invisible
    // to both — the containment that lets an author drop a content region into
    // an existing editor without changing anything around it.
    View page;
    page.set_bounds({0, 0, 200, 200});

    auto make_region = [&](float y, const char* a, const char* b) {
        auto region = std::make_unique<View>();
        region->set_bounds({0, y, 200, 60});
        region->set_text_selection_region(true);
        for (int i = 0; i < 2; ++i) {
            auto l = std::make_unique<Label>(i == 0 ? a : b);
            l->set_bounds({0, static_cast<float>(24 * i), 200, 20});
            region->add_child(std::move(l));
        }
        auto* raw = region.get();
        page.add_child(std::move(region));
        return raw;
    };
    View* first = make_region(0, "alpha", "bravo");
    View* second = make_region(80, "charlie", "delta");

    auto readout = std::make_unique<Label>("-12.0 dB");
    readout->set_bounds({0, 170, 200, 20});
    auto* readout_raw = readout.get();
    page.add_child(std::move(readout));

    REQUIRE(selectable_widgets_in_document_order(*first).size() == 2);
    REQUIRE(selectable_widgets_in_document_order(*second).size() == 2);
    REQUIRE(readout_raw->as_selectable_text() == nullptr);

    for (auto* w : selectable_widgets_in_document_order(page)) {
        RecordingCanvas c;
        w->selectable_view()->paint(c);
    }

    // Select everything reachable from the first region. The second region's
    // text and the loose readout must not appear.
    selection_select_all(*first);
    REQUIRE(selection_text(*first) == "alpha\nbravo");
}

TEST_CASE("a drag selects correctly in a content region away from the origin",
          "[view][selection][drag][region][coords]") {
    // Every other case in this file puts the region AT the tree origin, where
    // a scope-relative and a root-relative coordinate conversion agree. They
    // diverge by exactly the region's offset everywhere else, so a region
    // pushed down and across is the only shape that can see the difference —
    // and a real About panel is never at (0,0).
    View page;
    page.set_bounds({0, 0, 400, 400});

    auto region = std::make_unique<View>();
    region->set_bounds({37, 91, 200, 120});
    region->set_text_selection_region(true);
    std::vector<Label*> labels;
    const char* texts[] = {"alpha", "bravo", "charlie"};
    for (int i = 0; i < 3; ++i) {
        auto l = std::make_unique<Label>(texts[i]);
        l->set_bounds({0, static_cast<float>(24 * i), 200, 20});
        labels.push_back(l.get());
        region->add_child(std::move(l));
    }
    page.add_child(std::move(region));

    for (auto* l : labels) {
        RecordingCanvas c;
        l->paint(c);
    }

    // Root-space points: the region's own offset plus the Label's offset in it.
    const float rx = 37.0f, ry = 91.0f;
    const Point start{rx + 2 * kByteW, ry + 10.0f};
    const Point end{rx + 4 * kByteW, ry + 48.0f + 10.0f};
    page.simulate_drag(start, end, 6);

    REQUIRE(selection_has_range(*labels[0]->enclosing_text_selection_region()));
    REQUIRE(selection_text(*labels[0]->enclosing_text_selection_region()) ==
            "pha\nbravo\nchar");
}

TEST_CASE("a never-painted widget in the middle still selects in full",
          "[view][selection][document-order][edge]") {
    // The fallback in `full_range`. Every other case in this file paints every
    // Label first, so the measured path always wins and this branch never
    // fires — which is how it reached 0% coverage while being the branch that
    // decides whether a selection copies a hole.
    //
    // A widget scrolled out of view, or mounted but not yet drawn, reports no
    // geometry. It is still BETWEEN the endpoints, so it must contribute its
    // whole string. Falling back to "no geometry, no text" would silently drop
    // the middle paragraph out of the copy.
    Doc doc({"alpha", "bravo", "charlie"});
    // Deliberately paint only the endpoints.
    for (std::size_t i : {std::size_t{0}, std::size_t{2}}) {
        RecordingCanvas c;
        doc.labels[i]->paint(c);
    }
    REQUIRE_FALSE(doc.labels[1]->selectable_layout().measured);

    selection_begin(doc.root, *doc.labels[0]->as_selectable_text(), 2);
    selection_extend(doc.root, *doc.labels[2]->as_selectable_text(), 4);

    REQUIRE(selection_text(doc.root) == "pha\nbravo\nchar");
    int lo = 0, hi = 0;
    REQUIRE(doc.labels[1]->selection_highlight(lo, hi));
    REQUIRE(lo == 0);
    REQUIRE(hi == 5);
}

TEST_CASE("a hit-test on a never-painted widget clamps instead of guessing",
          "[view][selection][edge]") {
    // `selection_hit_test`'s unmeasured branch, both directions. With no paint
    // there are no shaped advances, so an interior offset would have to be
    // invented from the estimated-advance path the capability deliberately
    // refuses to publish. Clamping to an end is the honest answer.
    //
    // NEITHER Label is painted on purpose. Painting one makes it take the
    // measured path, and the x then decides the offset — which is what this
    // case originally asserted against, wrongly: at x=40 over "alpha" the
    // nearest boundary is byte 5, not 0, and the measured answer was right.
    Doc doc({"alpha", "bravo"});
    REQUIRE_FALSE(doc.labels[0]->selectable_layout().measured);
    REQUIRE_FALSE(doc.labels[1]->selectable_layout().measured);

    // Below everything: the nearest widget is the last, and the point is past
    // its end, so the offset clamps to the end of its text.
    const auto below = selection_hit_test(doc.root, {40.0f, 4000.0f});
    REQUIRE(below.target == doc.labels[1]->as_selectable_text());
    REQUIRE(below.offset == static_cast<int>(std::string("bravo").size()));

    // Above everything: the nearest widget is the first, and the point is
    // before its start, so the offset clamps to 0.
    const auto above = selection_hit_test(doc.root, {40.0f, -4000.0f});
    REQUIRE(above.target == doc.labels[0]->as_selectable_text());
    REQUIRE(above.offset == 0);
}

TEST_CASE("an opted-in Label outside every region is its own selection scope",
          "[view][selection][region][drag]") {
    // `SelectionPolicy::always` with no enclosing content region. There is no
    // region to scope the selection to, so the Label scopes it to ITSELF —
    // a drag can select within it and cannot reach a sibling, which is the
    // conservative reading: opting one Label in is not a licence to start
    // selecting the controls around it.
    View root;                      // deliberately NOT a content region
    root.set_bounds({0, 0, 200, 60});

    auto lone = std::make_unique<Label>("hello world");
    lone->set_bounds({0, 0, 200, 20});
    lone->set_selection_policy(Label::SelectionPolicy::always);
    auto* lone_raw = lone.get();
    root.add_child(std::move(lone));

    auto neighbour = std::make_unique<Label>("untouched");
    neighbour->set_bounds({0, 24, 200, 20});
    neighbour->set_selection_policy(Label::SelectionPolicy::always);
    auto* neighbour_raw = neighbour.get();
    root.add_child(std::move(neighbour));

    for (auto* l : {lone_raw, neighbour_raw}) {
        RecordingCanvas c;
        l->paint(c);
    }
    REQUIRE(lone_raw->enclosing_text_selection_region() == nullptr);

    // Drag from byte 2 down INTO the neighbour's band and far to the right.
    // Past-the-end resolves by clamping to the nearest row and then to the
    // nearest x on it, so an unscoped selection would have run into the
    // neighbour; a scoped one stops at this Label's own last byte.
    root.simulate_drag({2 * 7.0f, 10.0f}, {5000.0f, 40.0f}, 6);

    // The Label scoped it to itself: the selection reaches the end of its own
    // text and stops, and the neighbour is untouched.
    REQUIRE(selection_has_range(*lone_raw));
    REQUIRE(selection_text(*lone_raw) == "llo world");
    int lo = 0, hi = 0;
    REQUIRE_FALSE(neighbour_raw->selection_highlight(lo, hi));
}
