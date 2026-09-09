#include <pulp/view/appearance_defects.hpp>

#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>

using namespace pulp::view;

namespace {

int count_kind(const AppearanceReport& report, AppearanceDefectKind kind) {
    return static_cast<int>(std::count_if(
        report.findings.begin(), report.findings.end(),
        [kind](const AppearanceFinding& f) { return f.kind == kind; }));
}

bool names_pair(const AppearanceReport& report, const std::string& a,
                const std::string& b) {
    for (const auto& f : report.findings) {
        if ((f.a_id == a && f.b_id == b) || (f.a_id == b && f.b_id == a))
            return true;
    }
    return false;
}

/// A Label placed at an exact box, bypassing flex so the fixture geometry is
/// the thing under test rather than Yoga's answer to it.
Label* add_label(View& parent, const std::string& id, const std::string& text,
                 Rect box, bool multi_line = false) {
    auto label = std::make_unique<Label>(text);
    label->set_id(id);
    label->set_multi_line(multi_line);
    auto* raw = label.get();
    parent.add_child(std::move(label));
    raw->set_bounds(box);
    return raw;
}

CanvasDrawCmd text_cmd(const std::string& text, float x, float y, float size) {
    CanvasDrawCmd cmd;
    cmd.type = CanvasDrawCmd::Type::fill_text;
    cmd.text = text;
    cmd.x = x;
    cmd.y = y;
    cmd.extra = size;
    return cmd;
}

CanvasDrawCmd font_cmd(const std::string& family, float size) {
    CanvasDrawCmd cmd;
    cmd.type = CanvasDrawCmd::Type::set_font;
    cmd.text = family;
    cmd.extra = size;
    return cmd;
}

} // namespace

TEST_CASE("overlapping text is reported as a collision", "[appearance][layout]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    // Two labels asked to paint into the same place. A user sees one string
    // printed on top of the other.
    add_label(root, "left", "Precision", {40, 60, 160, 20});
    add_label(root, "right", "Latency", {40, 60, 160, 20});

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(count_kind(report, AppearanceDefectKind::text_overlap) == 1);
    REQUIRE(names_pair(report, "left", "right"));
    // The control: the same instrument on the same tree measured both runs.
    REQUIRE(report.coverage.text_runs_measured == 2);
    REQUIRE(report.coverage.trustworthy());
}

TEST_CASE("separated text is clean and the run is still measured",
          "[appearance][layout]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    add_label(root, "left", "Precision", {0, 0, 160, 20});
    add_label(root, "right", "Latency", {0, 120, 160, 20});

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    // A clean result only means something because the paired control below
    // proves the detector looked at both runs.
    REQUIRE(report.clean());
    REQUIRE(report.coverage.text_runs_measured == 2);
    REQUIRE(report.coverage.pairs_compared == 1);
    REQUIRE(report.coverage.trustworthy());
}

TEST_CASE("text painted wider than its box is reported as truncation",
          "[appearance][layout]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    add_label(root, "cramped",
              "The quick brown fox jumps over the lazy dog", {0, 0, 24, 18});

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(count_kind(report, AppearanceDefectKind::painted_wider_than_box) == 1);
    const auto& f = report.findings.front();
    REQUIRE(f.a_id == "cramped");
    REQUIRE(f.painted_width > f.box_width);
}

TEST_CASE("canvas text placed past its surface edge is reported",
          "[appearance][canvas]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    // The defect an axis caption has: narrower than the surface it is drawn on,
    // yet anchored so far right that most of it lands outside it. A detector
    // that only compares widths calls this clean.
    auto widget = std::make_unique<CanvasWidget>();
    widget->set_id("graph");
    auto* raw = widget.get();
    root.add_child(std::move(widget));
    raw->set_bounds({0, 0, 400, 200});
    raw->add_command(font_cmd("Inter", 16));
    raw->add_command(text_cmd("dBFS (analyzer)", 380, 40, 16));

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.coverage.canvas_text_measured == 1);
    REQUIRE(count_kind(report, AppearanceDefectKind::painted_wider_than_box) == 1);
    const auto& f = report.findings.front();
    REQUIRE(f.a_text == "dBFS (analyzer)");
    REQUIRE(f.painted_width < f.box_width);  // narrower than the surface
    REQUIRE(f.overflow_px > 0.0f);           // and still hanging off its edge

    // Control: the same caption anchored inside the surface is not a finding,
    // so what is reported is the placement and not the text.
    View clean_root;
    clean_root.set_bounds({0, 0, 400, 200});
    auto inside = std::make_unique<CanvasWidget>();
    inside->set_id("graph");
    auto* inside_raw = inside.get();
    clean_root.add_child(std::move(inside));
    inside_raw->set_bounds({0, 0, 400, 200});
    inside_raw->add_command(font_cmd("Inter", 16));
    inside_raw->add_command(text_cmd("dBFS (analyzer)", 20, 40, 16));

    const auto control = detect_appearance_defects(clean_root);
    INFO(control.to_string());
    REQUIRE(control.coverage.canvas_text_measured == 1);
    REQUIRE(control.clean());
}

TEST_CASE("the positive control turns a clean tree into findings",
          "[appearance][coverage]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    // Two labels with a small gap: genuinely clean, and close enough that a few
    // pixels of inflation must make them collide.
    add_label(root, "left", "Gain", {0, 0, 40, 18});
    add_label(root, "right", "Mix", {44, 0, 40, 18});

    const auto measured = detect_appearance_defects(root);
    INFO(measured.to_string());
    REQUIRE(measured.clean());
    REQUIRE(measured.coverage.text_runs_measured == 2);

    AppearanceOptions control_options;
    control_options.control_inflate_ink_px = 8.0f;
    const auto control = detect_appearance_defects(root, control_options);
    INFO(control.to_string());
    REQUIRE(count_kind(control, AppearanceDefectKind::text_overlap) == 1);
    REQUIRE(names_pair(control, "left", "right"));
    // The control must not invent coverage it did not already have.
    REQUIRE(control.coverage.text_runs_measured
            == measured.coverage.text_runs_measured);
}

TEST_CASE("a multi-line label is measured rather than skipped",
          "[appearance][layout]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    auto* wrapped = add_label(
        root, "subtitle",
        "Live is snappy and Precision is eased; the difference shows up on "
        "long gestures more than short ones.",
        {0, 0, 180, 60}, /*multi_line=*/true);

    // The Yoga contract this detector had to work around: a multi-line label
    // reports no intrinsic width so its parent drives wrapping. Anything that
    // used that number to find text saw nothing here.
    REQUIRE(wrapped->intrinsic_width() == 0.0f);

    const auto extents = wrapped->painted_text_extents(180.0f);
    REQUIRE(extents.measured);
    REQUIRE(extents.width > 0.0f);
    REQUIRE(extents.line_count > 1);

    const auto report = detect_appearance_defects(root);
    INFO(report.to_string());
    REQUIRE(report.coverage.text_runs_measured == 1);
    REQUIRE(report.coverage.skipped_unmeasurable == 0);
}

TEST_CASE("a multi-line label collides like any other text",
          "[appearance][layout]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    add_label(root, "subtitle",
              "Live is snappy and Precision is eased; the difference shows up "
              "on long gestures.",
              {20, 40, 180, 60}, /*multi_line=*/true);
    add_label(root, "badge", "NEW", {20, 48, 60, 16});

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(count_kind(report, AppearanceDefectKind::text_overlap) == 1);
    REQUIRE(names_pair(report, "subtitle", "badge"));
}

TEST_CASE("canvas text commands collide", "[appearance][canvas]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    auto widget = std::make_unique<CanvasWidget>();
    widget->set_id("graph");
    auto* raw = widget.get();
    root.add_child(std::move(widget));
    raw->set_bounds({0, 0, 400, 200});

    raw->add_command(font_cmd("Inter", 11));
    raw->add_command(text_cmd("dB (gain)", 60, 30, 11));
    raw->add_command(text_cmd("dBFS (analyzer)", 60, 30, 11));

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.coverage.canvas_text_commands == 2);
    REQUIRE(report.coverage.canvas_text_measured == 2);
    REQUIRE(count_kind(report, AppearanceDefectKind::canvas_text_overlap) == 1);
}

TEST_CASE("separated canvas text is clean and still measured",
          "[appearance][canvas]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    auto widget = std::make_unique<CanvasWidget>();
    widget->set_id("graph");
    auto* raw = widget.get();
    root.add_child(std::move(widget));
    raw->set_bounds({0, 0, 400, 200});

    raw->add_command(font_cmd("Inter", 11));
    raw->add_command(text_cmd("dB (gain)", 10, 10, 11));
    raw->add_command(text_cmd("dBFS (analyzer)", 10, 150, 11));

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.clean());
    // Control: an empty finding list from an instrument that measured nothing
    // would be indistinguishable from this without these two counts.
    REQUIRE(report.coverage.canvas_text_measured == 2);
}

TEST_CASE("canvas text honours textAlign when placing ink",
          "[appearance][canvas]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    auto widget = std::make_unique<CanvasWidget>();
    widget->set_id("graph");
    auto* raw = widget.get();
    root.add_child(std::move(widget));
    raw->set_bounds({0, 0, 400, 200});

    CanvasDrawCmd right_align;
    right_align.type = CanvasDrawCmd::Type::set_text_align;
    right_align.int_val = 2;

    raw->add_command(font_cmd("Inter", 11));
    raw->add_command(right_align);
    // Anchored at x=200 and right-aligned, so the ink lies to the LEFT of the
    // anchor. A detector that ignores textAlign puts this box in the wrong
    // half of the canvas.
    raw->add_command(text_cmd("dBFS (analyzer)", 200, 20, 11));

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.runs.size() == 1);
    const auto& run = report.runs.front();
    REQUIRE(run.ink.x < 200.0f);
    REQUIRE(run.ink.x + run.ink.width <= 200.5f);
}

TEST_CASE("text scrolled out of its container is not a collision",
          "[appearance][layout]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto scroller = std::make_unique<ScrollView>();
    scroller->set_id("panel");
    scroller->set_overflow(View::Overflow::scroll);
    auto* raw_scroller = scroller.get();
    root.add_child(std::move(scroller));
    raw_scroller->set_bounds({0, 0, 400, 120});
    raw_scroller->set_content_size({400, 900});

    // A row far down the scroll content. In unscrolled content space its y
    // lands on top of the fixed footer below; in screen space it is nowhere
    // near it. Comparing the two without resolving the scroll frame is the
    // false positive this fixture pins.
    auto* row = add_label(*raw_scroller, "row", "Live = snappy", {20, 700, 200, 20});
    (void)row;

    add_label(root, "footer", "PRESETS", {20, 700, 120, 20});

    raw_scroller->set_scroll(0.0f, 0.0f);
    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    // The row is clipped out of its own scroll viewport, so it is not on
    // screen and cannot collide with anything.
    REQUIRE(report.coverage.skipped_clipped == 1);
    REQUIRE(count_kind(report, AppearanceDefectKind::text_overlap) == 0);
}

TEST_CASE("scrolling a row into view makes it collidable again",
          "[appearance][layout]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    auto scroller = std::make_unique<ScrollView>();
    scroller->set_id("panel");
    scroller->set_overflow(View::Overflow::scroll);
    auto* raw_scroller = scroller.get();
    root.add_child(std::move(scroller));
    raw_scroller->set_bounds({0, 0, 400, 120});
    raw_scroller->set_content_size({400, 900});

    add_label(*raw_scroller, "row_a", "Live = snappy", {20, 700, 200, 20});
    add_label(*raw_scroller, "row_b", "Precision", {20, 700, 200, 20});

    raw_scroller->set_scroll(0.0f, 700.0f);
    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.coverage.text_runs_measured == 2);
    REQUIRE(count_kind(report, AppearanceDefectKind::text_overlap) == 1);
    REQUIRE(names_pair(report, "row_a", "row_b"));
}

TEST_CASE("hidden text is counted as unseen rather than clean",
          "[appearance][layout]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    auto* a = add_label(root, "a", "Precision", {40, 60, 160, 20});
    add_label(root, "b", "Latency", {40, 60, 160, 20});
    a->set_visible(false);

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.clean());
    REQUIRE(report.coverage.skipped_invisible == 1);
    REQUIRE(report.coverage.text_runs_measured == 1);
}

TEST_CASE("a run that measured nothing is not reported as trustworthy",
          "[appearance][coverage]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    // TextButton paints text but exposes no typography, so its ink cannot be
    // measured. A tree of nothing but buttons produces no findings and must
    // not read as a pass.
    for (int i = 0; i < 3; ++i) {
        auto button = std::make_unique<TextButton>();
        button->set_label("Apply");
        button->set_id("button" + std::to_string(i));
        auto* raw = button.get();
        root.add_child(std::move(button));
        raw->set_bounds({0, static_cast<float>(i * 30), 100, 24});
    }

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.clean());
    REQUIRE(report.coverage.skipped_unmeasurable == 3);
    REQUIRE_FALSE(report.coverage.trustworthy());
    REQUIRE(report.to_string().find("NOT TRUSTWORTHY") != std::string::npos);
}

TEST_CASE("a report is untrustworthy once blind skips outnumber what it saw",
          "[appearance][coverage]") {
    View root;
    root.set_bounds({0, 0, 400, 400});

    // One label the detector can measure, against a majority it cannot. The
    // finding list is empty either way, so the only thing separating "nothing
    // is wrong" from "I could not look" is this ratio.
    add_label(root, "seen", "Output", {0, 0, 120, 18});
    for (int i = 0; i < 3; ++i) {
        auto button = std::make_unique<TextButton>();
        button->set_label("Apply");
        button->set_id("blind" + std::to_string(i));
        auto* raw = button.get();
        root.add_child(std::move(button));
        raw->set_bounds({0, static_cast<float>(40 + i * 30), 100, 24});
    }

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.clean());
    REQUIRE(report.coverage.text_runs_measured == 1);
    REQUIRE(report.coverage.skipped_unmeasurable == 3);
    REQUIRE_FALSE(report.coverage.trustworthy());

    // The control: the same one measured run with the blind majority removed
    // must be trustworthy, otherwise the ratio is not what decided it.
    View clean_root;
    clean_root.set_bounds({0, 0, 400, 400});
    add_label(clean_root, "seen", "Output", {0, 0, 120, 18});
    const auto control = detect_appearance_defects(clean_root);
    REQUIRE(control.coverage.text_runs_measured == 1);
    REQUIRE(control.coverage.trustworthy());
}

TEST_CASE("an empty tree is never trustworthy", "[appearance][coverage]") {
    View root;
    root.set_bounds({0, 0, 400, 200});

    const auto report = detect_appearance_defects(root);

    INFO(report.to_string());
    REQUIRE(report.clean());
    REQUIRE_FALSE(report.coverage.trustworthy());
}

TEST_CASE("coverage is printed on every run", "[appearance][coverage]") {
    View root;
    root.set_bounds({0, 0, 400, 200});
    add_label(root, "only", "Rate", {0, 0, 120, 20});

    const auto text = detect_appearance_defects(root).to_string();

    REQUIRE(text.find("coverage:") != std::string::npos);
    REQUIRE(text.find("text runs measured") != std::string::npos);
    REQUIRE(text.find("skipped:") != std::string::npos);
    REQUIRE(text.find("shaping:") != std::string::npos);
}
