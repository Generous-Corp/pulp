// pulp-internal #70 — design-tool viewport resolver: probe-layout path.
//
// The design-tool example's viewport resolver runs a real Yoga layout
// pass at (fallback_width, 99999) and reads the JSX root's measured
// height to pick the window size. This is more accurate than
// View::intrinsic_height(), which returns 0 for flex:row and
// display:grid containers — the exact mix runtime-imported React
// components (e.g. Chainer) routinely use.
//
// These tests lock in the probe-layout invariant for the two
// problematic container shapes so a regression in yoga_layout or
// View::layout_children that drops these heights can't silently
// re-introduce the "huge black bottom" letterbox bug.
//
// Tag [issue-pulp-internal-70] for coverage attribution.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/view.hpp>

using pulp::view::View;
using pulp::view::FlexDirection;
using pulp::view::LayoutMode;
using pulp::view::GridTrack;
using Catch::Matchers::WithinAbs;

namespace {

// Build a Chainer-shaped tree: outer flex:column root with one child
// that contains a header (fixed 30px), a display:grid middle band
// with an explicit preferred_height (matching what the JS bridge sets
// when it sees grid-template-rows summing to a known total), and a
// footer (fixed 28px). The grid child's height has to come from the
// View tree (preferred_height or intrinsic_height) — yoga treats
// grid children as leaves and doesn't recurse into grid_template_rows.
std::unique_ptr<View> build_chainer_like_root() {
    auto root = std::make_unique<View>();
    root->flex().direction = FlexDirection::column;

    auto outer = std::make_unique<View>();
    outer->flex().direction = FlexDirection::column;
    outer->flex().preferred_width = 1320.0f;

    auto header = std::make_unique<View>();
    header->flex().preferred_height = 30.0f;

    auto middle = std::make_unique<View>();
    middle->set_layout_mode(LayoutMode::grid);
    // JS bridge sets preferred_height on grid containers from the sum
    // of grid-template-rows pixel values so yoga can pack the column
    // flex parent correctly. 220 + 240 = 460.
    middle->flex().preferred_height = 460.0f;
    middle->grid().template_columns = {GridTrack::fractional(1),
                                       GridTrack::fractional(1)};
    middle->grid().template_rows = {GridTrack::fixed_px(220.0f),
                                    GridTrack::fixed_px(240.0f)};
    for (int i = 0; i < 4; ++i) {
        middle->add_child(std::make_unique<View>());
    }

    auto footer = std::make_unique<View>();
    footer->flex().preferred_height = 28.0f;

    outer->add_child(std::move(header));
    outer->add_child(std::move(middle));
    outer->add_child(std::move(footer));
    root->add_child(std::move(outer));
    return root;
}

} // namespace

TEST_CASE("probe-layout measures real child height for grid+column tree",
          "[view][design-viewport][issue-pulp-internal-70]") {
    auto root = build_chainer_like_root();

    // Probe-layout: huge available height; yoga resolves the grid's
    // fixed rows and the column flex packs header + middle + footer.
    constexpr float kProbeHeight = 99999.0f;
    constexpr float kProbeWidth  = 1320.0f;
    root->set_bounds({0.0f, 0.0f, kProbeWidth, kProbeHeight});
    root->layout_children();

    REQUIRE(root->child_count() == 1);
    const auto* outer = root->child_at(0);
    REQUIRE(outer != nullptr);

    // Expected: 30 (header) + 460 (grid preferred_height) + 28 (footer)
    // = 518. Must be much smaller than the probe height (proving the
    // tree didn't blow up to fill 99999) and much larger than just the
    // fixed band heights without the grid (58, what intrinsic_height
    // would naively return for a row/grid-mixed column).
    const float h = outer->local_bounds().height;
    INFO("Measured outer height: " << h);
    REQUIRE_THAT(h, WithinAbs(518.0f, 1.0f));
    REQUIRE(h < kProbeHeight); // probe-bound respected
}

TEST_CASE("intrinsic_height alone returns 0 for grid middle band",
          "[view][design-viewport][issue-pulp-internal-70]") {
    // Confirms the gap that probe-layout closes: a display:grid View
    // has no children for which intrinsic_height knows how to sum
    // (it only sums for flex:column containers).
    View grid;
    grid.set_layout_mode(LayoutMode::grid);
    grid.grid().template_columns = {GridTrack::fractional(1)};
    grid.grid().template_rows    = {GridTrack::fixed_px(200.0f)};
    grid.add_child(std::make_unique<View>());

    // intrinsic_height returns 0 because View::intrinsic_height only
    // sums children for flex:column / column_reverse containers — grid
    // and flex:row both fall into the early-return 0 branch.
    REQUIRE(grid.intrinsic_height() == 0.0f);

    // But probe-layout at a non-trivial size resolves the grid row
    // correctly:
    grid.set_bounds({0.0f, 0.0f, 800.0f, 99999.0f});
    grid.layout_children();
    REQUIRE(grid.child_count() == 1);
    const auto* cell = grid.child_at(0);
    REQUIRE(cell != nullptr);
    REQUIRE_THAT(cell->local_bounds().height, WithinAbs(200.0f, 1e-3f));
}

TEST_CASE("probe-scan binary-search finds the layout's settling width",
          "[view][design-viewport][issue-pulp-internal-70]") {
    // Build a Chainer-style responsive tree: parent uses flex-wrap and
    // children declare minWidth. The natural width is where all
    // children fit on the designed number of rows; below that, height
    // grows because content wraps into more rows.
    auto root = std::make_unique<View>();
    root->flex().direction = FlexDirection::column;

    auto wrap_parent = std::make_unique<View>();
    wrap_parent->flex().direction = FlexDirection::row;
    wrap_parent->flex().flex_wrap = pulp::view::FlexWrap::wrap;
    wrap_parent->flex().gap = 8.0f;

    // 4 cells, each min-width 200, height 100 → total min row width
    // = 4*200 + 3*8 = 824. Below 824 they wrap; at >= 824 they fit.
    for (int i = 0; i < 4; ++i) {
        auto cell = std::make_unique<View>();
        cell->flex().min_width = 200.0f;
        cell->flex().preferred_height = 100.0f;
        wrap_parent->add_child(std::move(cell));
    }
    root->add_child(std::move(wrap_parent));

    // Replicate the resolver's probe helper inline so the test
    // verifies the same arithmetic the design-tool uses.
    auto probe = [&](float w) -> float {
        root->set_bounds({0, 0, w, 99999.0f});
        root->layout_children();
        return root->child_at(0)->local_bounds().height;
    };

    const float h_wide = probe(4000.0f);
    const float h_narrow = probe(200.0f);

    INFO("h_wide=" << h_wide << " h_narrow=" << h_narrow);
    // Wide: one row → height 100.
    // Narrow (200): one cell per row → 4 rows × 100 + 3*8 gaps = 424.
    REQUIRE(h_narrow > h_wide + 50.0f);  // responsive: clearly wraps

    // Binary-search settling width — same loop as the resolver.
    float lo = 200.0f, hi = 4000.0f;
    while (hi - lo > 16.0f) {
        float mid = (lo + hi) * 0.5f;
        if (probe(mid) > h_wide + 4.0f) lo = mid;
        else                            hi = mid;
    }
    INFO("settling width = " << hi);
    // Should converge near 824 (4 cells * 200 + 3 gaps * 8).
    REQUIRE(hi >= 820.0f);
    REQUIRE(hi <= 880.0f);
}

TEST_CASE("probe-layout returns sensible width when no preferred_width set",
          "[view][design-viewport][issue-pulp-internal-70]") {
    // For trees where the outer child has no preferred_width, the
    // resolver falls back to the probe width itself (kFallbackDesignWidth).
    // Verify that yoga at least gives us a deterministic non-zero
    // measurement we can then clamp.
    auto root = std::make_unique<View>();
    root->flex().direction = FlexDirection::column;

    auto child = std::make_unique<View>();
    child->flex().direction = FlexDirection::column;
    child->flex().preferred_height = 400.0f;
    root->add_child(std::move(child));

    root->set_bounds({0.0f, 0.0f, 1320.0f, 99999.0f});
    root->layout_children();

    const auto* c = root->child_at(0);
    REQUIRE(c != nullptr);
    // Child has no preferred_width and parent is column-direction →
    // align-items:stretch (default) makes width fill the cross axis.
    REQUIRE_THAT(c->local_bounds().width, WithinAbs(1320.0f, 1e-3f));
    REQUIRE_THAT(c->local_bounds().height, WithinAbs(400.0f, 1e-3f));
}
