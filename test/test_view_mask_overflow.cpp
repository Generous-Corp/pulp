// test_view_mask_overflow.cpp — extracted from test_view.cpp in the
// 2026-05 Phase 5 (P5-3 follow-up) refactor.
//
// Two coherent View paint/hit-test surfaces shipped together:
//
//   1. pulp #1148 slice (a) — symmetric overflow:visible hit-test
//      extension. PR #1297 added a 500px-radius extension so that
//      absolutely-positioned popovers / dropdowns / menus protrude
//      past their parent and still receive pointer hits.
//      View::hit_test extends overflow:visible 500px to LEFT / RIGHT
//      / UP / DOWN; does NOT extend past 500px LEFT.
//
//   2. pulp #1737 / #1515 — CSS mask-image + mask-size paint slice.
//      View::paint_all routes through save_layer_with_mask when
//      mask-image is set; does NOT route through when mask-image is
//      empty or set to 'none'.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/live_constant_editor.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ── pulp #1148 slice (a) — symmetric overflow:visible hit-test extension ──
//
// PR #1297 added overflow:visible hit-test extension so that absolutely-
// positioned popovers / dropdowns whose content escapes their bounds box
// still receive clicks. The original implementation only extended the box
// 500px DOWNWARD, which broke left-extending popovers (e.g. Spectr's bands
// picker that opens to the left of its trigger). This suite locks the
// extension to ±500px on all four sides.
//
// Each direction places a "popover" grandchild positioned outside its
// overflow:visible parent in the tested direction — overflow:visible
// passes the click through to the parent's hit_test recursion, which
// then matches the grandchild's local_bounds. Without symmetric slack,
// the parent rejects the click before recursion happens for left/right/up.
namespace {
struct PopoverFixture {
    View root;
    View* container{nullptr};   // overflow:visible host
    View* popover{nullptr};     // grandchild positioned outside container

    // dx/dy are the popover's offset *relative to the container's local
    // origin* — negative values escape the container in the −x/−y direction.
    PopoverFixture(float dx, float dy) {
        root.set_bounds({0, 0, 2000, 2000});
        auto c = std::make_unique<View>();
        c->set_bounds({600, 600, 100, 100});
        c->set_overflow(View::Overflow::visible);
        container = c.get();

        auto p = std::make_unique<View>();
        // 50x50 popover anchored at (dx, dy) inside container's local space.
        p->set_bounds({dx, dy, 50, 50});
        popover = p.get();
        c->add_child(std::move(p));
        root.add_child(std::move(c));
    }
};
} // namespace

TEST_CASE("View::hit_test extends overflow:visible 500px to the LEFT",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover at container-local (-200, 25) → root-space (400..450, 625..675).
    // 100px to the LEFT of container.x=600 covers root.x = 425.
    PopoverFixture f(-200, 25);
    REQUIRE(f.root.hit_test({425, 650}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px to the RIGHT",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover at container-local (200, 25) → root-space (800..850, 625..675).
    // 100px to the RIGHT of container.right=700 covers root.x = 825.
    PopoverFixture f(200, 25);
    REQUIRE(f.root.hit_test({825, 650}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px UPWARD",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover at container-local (25, -200) → root-space (625..675, 400..450).
    // 100px ABOVE container.y=600 covers root.y = 425.
    PopoverFixture f(25, -200);
    REQUIRE(f.root.hit_test({650, 425}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px DOWNWARD",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover at container-local (25, 200) → root-space (625..675, 800..850).
    // 100px BELOW container.bottom=700 covers root.y = 825.
    // This direction was already supported pre-#1148 — guards regression.
    PopoverFixture f(25, 200);
    REQUIRE(f.root.hit_test({650, 825}) == f.popover);
}

TEST_CASE("View::hit_test does NOT extend overflow:visible past 500px LEFT",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover anchored 600px LEFT of container — outside the symmetric
    // ±500px slack, so the click must miss the popover entirely. With
    // container x=600, popover at container-local x=-650 lands at
    // root.x = -50..0; we probe root.x = 0 → container-local x = -600,
    // beyond the -500 slack.
    PopoverFixture f(-650, 25);
    REQUIRE(f.root.hit_test({0, 650}) != f.popover);
}

// ── pulp #1737 / #1515 — CSS mask-image + mask-size paint slice ─────────
//
// Phase 1 ships linear-gradient mask shapes through the new
// Canvas::save_layer_with_mask virtual + SkiaCanvas's 2-saveLayer +
// kDstIn composite at restore() time. RecordingCanvas spy captures
// the API dispatch so we can pin the wiring without depending on Skia
// (which is the only backend that actually composites the mask alpha).
// Visual output is verified separately against the SkiaCanvas raster
// path in the [skia] tests below.

namespace {
struct MaskSpyCanvas : pulp::canvas::RecordingCanvas {
    struct MaskCall {
        float x, y, w, h, opacity;
        std::string mask_image;
        std::string mask_size;
    };
    std::vector<MaskCall> mask_calls;

    void save_layer_with_mask(float x, float y, float w, float h,
                               float opacity,
                               const std::string& mask_image,
                               const std::string& mask_size) override {
        mask_calls.push_back({x, y, w, h, opacity, mask_image, mask_size});
        // Don't fall through to save_layer — we only want to spy the
        // mask call, not double-record a save_layer command.
    }
};
}

TEST_CASE("View::paint_all routes through save_layer_with_mask when mask-image is set",
          "[view][issue-1515][issue-1737]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 50});
    v.set_mask_image("linear-gradient(to bottom, black, transparent)");
    v.set_mask_size("100% 100%");

    MaskSpyCanvas canvas;
    v.paint_all(canvas);

    REQUIRE(canvas.mask_calls.size() == 1);
    auto& m = canvas.mask_calls[0];
    REQUIRE(m.x == 0);
    REQUIRE(m.y == 0);
    REQUIRE(m.w == 100);
    REQUIRE(m.h == 50);
    REQUIRE(m.mask_image == "linear-gradient(to bottom, black, transparent)");
    REQUIRE(m.mask_size == "100% 100%");
}

TEST_CASE("View::paint_all does NOT route through save_layer_with_mask when mask-image is empty",
          "[view][issue-1515][issue-1737]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 50});
    // No mask_image set — paint_all should use the legacy path
    // (save_layer or just paint without a layer).

    MaskSpyCanvas canvas;
    v.paint_all(canvas);

    REQUIRE(canvas.mask_calls.empty());
}

TEST_CASE("View::paint_all does NOT route through save_layer_with_mask when mask-image is 'none'",
          "[view][issue-1515][issue-1737]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 50});
    v.set_mask_image("none");

    MaskSpyCanvas canvas;
    v.paint_all(canvas);

    // CSS `mask-image: none` is an explicit no-mask declaration —
    // treated as if no mask were set (no layer overhead, no composite).
    REQUIRE(canvas.mask_calls.empty());
}

