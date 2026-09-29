// The root View paints a small "◉ TRACING" corner pill whenever the binary is
// built with PULP_TRACING=ON, so a developer can never forget that Perfetto
// tracing is compiled in while looking at the plugin UI. In the default OFF
// build the paint site is discarded by `if constexpr (kTracingEnabled)`, so no
// badge is ever emitted. These tests pin both halves with a RecordingCanvas
// (deterministic, no raster backend needed) and cover the runtime suppression
// hook golden-screenshot harnesses use, the placement hook applications use to
// put the pill on their own header line, and the centring of the label on its
// measured ink.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/trace.hpp>  // kTracingEnabled
#include <pulp/view/tracing_badge.hpp>
#include <pulp/view/view.hpp>

#include <cmath>
#include <memory>
#include <string>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

namespace {

// Number of fill_text commands whose text carries the badge label. The label is
// distinctive ("TRACING"), so a match is unambiguously the tracing badge and
// never incidental widget text.
int badge_label_draws(const RecordingCanvas& rec) {
    int n = 0;
    for (const auto& c : rec.commands())
        if (c.type == DrawCommand::Type::fill_text &&
            c.text.find("TRACING") != std::string::npos)
            ++n;
    return n;
}

// A rounded-rect fill sits behind the badge label as its pill background.
bool has_rounded_rect(const RecordingCanvas& rec) {
    for (const auto& c : rec.commands())
        if (c.type == DrawCommand::Type::fill_rounded_rect) return true;
    return false;
}

const DrawCommand* first_of(const RecordingCanvas& rec, DrawCommand::Type type) {
    for (const auto& c : rec.commands())
        if (c.type == type) return &c;
    return nullptr;
}

// Restores the default placement when a test that moved it ends.
struct DefaultPlacementGuard {
    ~DefaultPlacementGuard() { set_tracing_badge_placement(TracingBadgePlacement{}); }
};

}  // namespace

TEST_CASE("tracing badge predicate tracks the compile-time flag", "[view][tracing]") {
    // Default state: the badge is visible whenever tracing is compiled in.
    set_tracing_badge_visible(true);
    REQUIRE(tracing_badge_should_paint() == pulp::runtime::kTracingEnabled);
}

TEST_CASE("tracing badge suppression hook", "[view][tracing]") {
    set_tracing_badge_visible(false);
    // Suppressed: never paints, regardless of build configuration.
    REQUIRE_FALSE(tracing_badge_should_paint());
    // Restore the default so it cannot leak into other tests.
    set_tracing_badge_visible(true);
    REQUIRE(tracing_badge_should_paint() == pulp::runtime::kTracingEnabled);
}

TEST_CASE("root View paints the tracing badge only when compiled in", "[view][tracing]") {
    set_tracing_badge_visible(true);

    View root;
    root.set_bounds({0, 0, 400, 300});

    RecordingCanvas rec;
    root.paint_all(rec);

    if constexpr (pulp::runtime::kTracingEnabled) {
        // ON build: exactly one badge (the root paints it) with its pill.
        REQUIRE(badge_label_draws(rec) == 1);
        REQUIRE(has_rounded_rect(rec));
    } else {
        // OFF build: the paint site is discarded — nothing is emitted.
        REQUIRE(badge_label_draws(rec) == 0);
    }
}

TEST_CASE("suppressed tracing badge is absent even in an ON build", "[view][tracing]") {
    set_tracing_badge_visible(false);

    View root;
    root.set_bounds({0, 0, 400, 300});

    RecordingCanvas rec;
    root.paint_all(rec);
    REQUIRE(badge_label_draws(rec) == 0);

    // Re-enable and confirm it comes back only when tracing is compiled in.
    set_tracing_badge_visible(true);
    RecordingCanvas rec2;
    root.paint_all(rec2);
    if constexpr (pulp::runtime::kTracingEnabled)
        REQUIRE(badge_label_draws(rec2) == 1);
    else
        REQUIRE(badge_label_draws(rec2) == 0);
}

TEST_CASE("child View never paints the tracing badge", "[view][tracing]") {
    set_tracing_badge_visible(true);

    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 100, 80});
    View* child_ptr = child.get();
    root.add_child(std::move(child));

    // Painting the child directly (it has a parent) must not stamp a badge.
    RecordingCanvas rec;
    child_ptr->paint_all(rec);
    REQUIRE(badge_label_draws(rec) == 0);
}

TEST_CASE("tracing badge label is centred on its ink, not on a fraction of the pill",
          "[view][tracing]") {
    // A face whose ink is not the 72%-of-pill split the badge used to assume:
    // ascent 10, descent 3 in a 19-pixel pill.
    TracingBadgePlacement placement;
    placement.top = 5.0f;
    placement.height = 19.0f;
    const auto layout = tracing_badge_layout(400.0f, 60.0f, 10.0f, 3.0f, placement);
    const float above = (layout.baseline_y - 10.0f) - layout.pill_y;
    const float below = (layout.pill_y + layout.pill_height) - (layout.baseline_y + 3.0f);
    CHECK(above == below);
    CHECK(layout.baseline_y == 5.0f + 13.0f);
    // Negative control: the previous rule, 72% of the pill height, puts this
    // face's ink off centre, so the equal-gap check above can tell them apart.
    const float old_baseline = 5.0f + 19.0f * 0.72f;
    const float old_above = (old_baseline - 10.0f) - 5.0f;
    const float old_below = (5.0f + 19.0f) - (old_baseline + 3.0f);
    CHECK(std::abs(old_above - old_below) > 1.0f);
}

TEST_CASE("tracing badge layout follows its placement", "[view][tracing]") {
    TracingBadgePlacement placement;
    placement.right = 20.0f;
    placement.top = 11.5f;
    placement.height = 20.0f;
    placement.font_px = 10.0f;
    const auto layout = tracing_badge_layout(1320.0f, 54.0f, 8.0f, 2.0f, placement);
    CHECK(layout.pill_width == 54.0f + 16.0f);
    CHECK(layout.pill_x == 1320.0f - 70.0f - 20.0f);
    CHECK(layout.pill_y == 11.5f);
    CHECK(layout.pill_height == 20.0f);
    CHECK(layout.text_x == layout.pill_x + 8.0f);
    // The pill's centre line is the ink's centre line.
    CHECK(layout.baseline_y - 8.0f + 5.0f == layout.pill_y + 10.0f);
    // Height 0 keeps the historical font-plus-padding pill.
    placement.height = 0.0f;
    CHECK(tracing_badge_layout(1320.0f, 54.0f, 8.0f, 2.0f, placement).pill_height
          == 10.0f + 8.0f);
}

TEST_CASE("tracing badge placement round-trips", "[view][tracing]") {
    DefaultPlacementGuard restore;
    TracingBadgePlacement placement;
    placement.right = 12.0f;
    placement.top = 30.0f;
    placement.height = 24.0f;
    placement.font_px = 9.0f;
    set_tracing_badge_placement(placement);
    const auto read = tracing_badge_placement();
    CHECK(read.right == 12.0f);
    CHECK(read.top == 30.0f);
    CHECK(read.height == 24.0f);
    CHECK(read.font_px == 9.0f);
}

TEST_CASE("root View paints the tracing badge where its placement puts it",
          "[view][tracing]") {
    DefaultPlacementGuard restore;
    set_tracing_badge_visible(true);
    TracingBadgePlacement placement;
    placement.right = 20.0f;
    placement.top = 30.0f;
    placement.height = 24.0f;
    set_tracing_badge_placement(placement);

    View root;
    root.set_bounds({0, 0, 400, 300});
    RecordingCanvas rec;
    root.paint_all(rec);

    if constexpr (pulp::runtime::kTracingEnabled) {
        const auto* pill = first_of(rec, DrawCommand::Type::fill_rounded_rect);
        const DrawCommand* text = nullptr;
        for (const auto& c : rec.commands())
            if (c.type == DrawCommand::Type::fill_text
                && c.text.find("TRACING") != std::string::npos) text = &c;
        REQUIRE(pill != nullptr);
        REQUIRE(text != nullptr);
        // fill_rounded_rect records (x, y, w, h, radius) in f[0..4].
        CHECK(pill->f[1] == 30.0f);
        CHECK(pill->f[3] == 24.0f);
        CHECK(pill->f[0] + pill->f[2] == 400.0f - 20.0f);
        // Painted where the pure layout says, for the canvas's own metrics.
        // Copy the painted position first: measuring on the same canvas
        // records more commands and may move the vector `text` points into.
        const float painted_x = text->f[0];
        const float painted_y = text->f[1];
        rec.set_font("system", placement.font_px);
        const auto metrics = rec.measure_text_full(kTracingBadgeLabel);
        const auto layout = tracing_badge_layout(400.0f, metrics.width,
                                                 metrics.ascent, metrics.descent,
                                                 placement);
        // fill_text records (x, y) in f[0..1].
        CHECK(painted_x == layout.text_x);
        CHECK(painted_y == layout.baseline_y);
    } else {
        REQUIRE(badge_label_draws(rec) == 0);
    }
}
