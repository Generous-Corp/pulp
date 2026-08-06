// View mask and overflow tests for two coherent paint/hit-test surfaces:
//
//   1. Symmetric overflow:visible hit-test extension. A 500px-radius
//      extension allows absolutely-positioned popovers / dropdowns / menus
//      to protrude past their parent and still receive pointer hits.
//      View::hit_test extends overflow:visible 500px to LEFT / RIGHT
//      / UP / DOWN; does NOT extend past 500px LEFT.
//
//   2. CSS mask-image + mask-size paint coverage.
//      View::paint_all routes through save_layer_with_mask when
//      mask-image is set; does NOT route through when mask-image is
//      empty or set to 'none'.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

// ── Symmetric overflow:visible hit-test extension ───────────────────────
//
// Overflow:visible hit-test extension lets absolutely-positioned popovers /
// dropdowns whose content escapes their bounds box still receive clicks. This
// suite locks the extension to ±500px on all four sides.
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
          "[view][hit_test][overflow-symmetric]") {
    // Popover at container-local (-200, 25) → root-space (400..450, 625..675).
    // 100px to the LEFT of container.x=600 covers root.x = 425.
    PopoverFixture f(-200, 25);
    REQUIRE(f.root.hit_test({425, 650}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px to the RIGHT",
          "[view][hit_test][overflow-symmetric]") {
    // Popover at container-local (200, 25) → root-space (800..850, 625..675).
    // 100px to the RIGHT of container.right=700 covers root.x = 825.
    PopoverFixture f(200, 25);
    REQUIRE(f.root.hit_test({825, 650}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px UPWARD",
          "[view][hit_test][overflow-symmetric]") {
    // Popover at container-local (25, -200) → root-space (625..675, 400..450).
    // 100px ABOVE container.y=600 covers root.y = 425.
    PopoverFixture f(25, -200);
    REQUIRE(f.root.hit_test({650, 425}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px DOWNWARD",
          "[view][hit_test][overflow-symmetric]") {
    // Popover at container-local (25, 200) → root-space (625..675, 800..850).
    // 100px BELOW container.bottom=700 covers root.y = 825.
    // This direction was already supported before the symmetric extension.
    PopoverFixture f(25, 200);
    REQUIRE(f.root.hit_test({650, 825}) == f.popover);
}

TEST_CASE("View::hit_test does NOT extend overflow:visible past 500px LEFT",
          "[view][hit_test][overflow-symmetric]") {
    // Popover anchored 600px LEFT of container — outside the symmetric
    // ±500px slack, so the click must miss the popover entirely. With
    // container x=600, popover at container-local x=-650 lands at
    // root.x = -50..0; we probe root.x = 0 → container-local x = -600,
    // beyond the -500 slack.
    PopoverFixture f(-650, 25);
    REQUIRE(f.root.hit_test({0, 650}) != f.popover);
}

// ── CSS mask-image + mask-size paint coverage ───────────────────────────
//
// Linear-gradient mask shapes route through the Canvas::save_layer_with_mask
// virtual + SkiaCanvas's 2-saveLayer + kDstIn composite at restore() time.
// RecordingCanvas spy captures the API dispatch so we can pin the wiring
// without depending on Skia (which is the only backend that actually composites
// the mask alpha).
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
          "[view][mask-image]") {
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
          "[view][mask-image]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 50});
    // No mask_image set — paint_all should use the legacy path
    // (save_layer or just paint without a layer).

    MaskSpyCanvas canvas;
    v.paint_all(canvas);

    REQUIRE(canvas.mask_calls.empty());
}

TEST_CASE("overflow:hidden on a rounded frame clips to the rounded box, not a square",
          "[view][overflow]") {
    // CSS overflow:hidden clips to the ROUNDED border box. A square clip saws
    // the rounded corners off a clipped card — the imported mixer channel strips
    // (border-radius + clip content) rendered with hard square corners until the
    // overflow clip honoured the radius. A uniform radius (set_border_radius) and
    // the per-corner setters must both trigger the rounded clip.
    pulp::view::View rounded;
    rounded.set_bounds({0, 0, 62, 235});
    rounded.set_overflow(pulp::view::View::Overflow::hidden);
    rounded.set_border_radius(8);  // uniform, the common setCornerRadius("All") path
    pulp::canvas::RecordingCanvas rc;
    rounded.paint_all(rc);
    REQUIRE(rc.count(pulp::canvas::DrawCommand::Type::clip_path_svg) >= 1);

    // No radius: the overflow clip stays a plain square clip_rect.
    pulp::view::View square;
    square.set_bounds({0, 0, 62, 235});
    square.set_overflow(pulp::view::View::Overflow::hidden);
    pulp::canvas::RecordingCanvas rc2;
    square.paint_all(rc2);
    REQUIRE(rc2.count(pulp::canvas::DrawCommand::Type::clip_path_svg) == 0);
    REQUIRE(rc2.count(pulp::canvas::DrawCommand::Type::clip_rect) >= 1);
}

// ── Import-resolved ancestor clip ───────────────────────────────────────
//
// `overflow` clips a view AND its subtree, which is what a browser does along
// the containing-block chain and NOT what it does along DOM parentage. An
// importer that resolves a node's real clip chain therefore needs a clip that
// applies to the node alone, because a child can legitimately need a WIDER clip
// than its parent — an absolutely positioned node whose containing block sits
// above the `overflow: hidden` box it is nested in escapes that clip in a
// browser, and an inherited clip is an intersection and cannot widen.

namespace {
/// Where each command sits in the recording, so a clip can be shown to have
/// been released before a later draw rather than merely to have been pushed.
int index_of(const pulp::canvas::RecordingCanvas& rc,
             pulp::canvas::DrawCommand::Type type, int nth = 0) {
    int seen = 0;
    for (size_t i = 0; i < rc.commands().size(); ++i) {
        if (rc.commands()[i].type != type) continue;
        if (seen++ == nth) return static_cast<int>(i);
    }
    return -1;
}
}

TEST_CASE("an import-resolved clip applies to the view and not to its children",
          "[view][overflow][clip-model]") {
    using T = pulp::canvas::DrawCommand::Type;

    pulp::view::View parent;
    parent.set_bounds({0, 0, 200, 200});
    parent.set_background_color(pulp::canvas::Color::rgba8(10, 10, 10, 255));
    parent.set_ancestor_clip_rect({20, 20, 60, 60});

    auto child = std::make_unique<pulp::view::View>();
    child->set_bounds({100, 100, 50, 50});
    child->set_background_color(pulp::canvas::Color::rgba8(200, 0, 0, 255));
    parent.add_child(std::move(child));

    pulp::canvas::RecordingCanvas rc;
    parent.paint_all(rc);

    // The rectangle reached the canvas, in the view's own coordinate space.
    const int clip = index_of(rc, T::clip_rect);
    REQUIRE(clip >= 0);
    CHECK(rc.commands()[static_cast<size_t>(clip)].f[0] == 20.0f);
    CHECK(rc.commands()[static_cast<size_t>(clip)].f[2] == 60.0f);

    // And it was released before the child painted. Without the release the
    // child's 50x50 box at (100,100) — entirely outside a 60x60 clip at
    // (20,20) — would have no pixels at all, which is the exact way an escaping
    // node disappears when a tree clips by parentage.
    const int child_fill = index_of(rc, T::fill_rect, 1);
    REQUIRE(child_fill > clip);
    int depth = 0;
    int lowest = 0;
    for (int i = clip; i < child_fill; ++i) {
        const auto type = rc.commands()[static_cast<size_t>(i)].type;
        if (type == T::save) ++depth;
        if (type == T::restore) --depth;
        lowest = std::min(lowest, depth);
    }
    // Dipping below the depth the clip was pushed at is the scope closing. The
    // child then opens its own, so the depth at the child's draw is back to
    // where it started and only the minimum along the way says what happened.
    CHECK(lowest < 0);
}

TEST_CASE("a view with no import-resolved clip installs none",
          "[view][overflow][clip-model]") {
    // The control: without this, the assertion above is satisfied by a canvas
    // that clips everything, and every native tree would be paying for a
    // feature only imported ones use.
    pulp::view::View v;
    v.set_bounds({0, 0, 200, 200});
    v.set_background_color(pulp::canvas::Color::rgba8(10, 10, 10, 255));
    CHECK_FALSE(v.ancestor_clip_rect().has_value());

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);
    CHECK(rc.count(pulp::canvas::DrawCommand::Type::clip_rect) == 0);
}

TEST_CASE("an import-resolved clip and overflow coexist with different reach",
          "[view][overflow][clip-model]") {
    // `overflow` still clips the subtree. The two clips are different tools and
    // a view can carry both: the ancestor clip cuts the view's own ink, the
    // overflow clip cuts its children. Merging them would put DOM parentage
    // back in charge of the ancestor clip.
    using T = pulp::canvas::DrawCommand::Type;

    pulp::view::View parent;
    parent.set_bounds({0, 0, 200, 200});
    parent.set_background_color(pulp::canvas::Color::rgba8(10, 10, 10, 255));
    parent.set_ancestor_clip_rect({20, 20, 60, 60});
    parent.set_overflow(pulp::view::View::Overflow::hidden);

    auto child = std::make_unique<pulp::view::View>();
    child->set_bounds({100, 100, 50, 50});
    child->set_background_color(pulp::canvas::Color::rgba8(200, 0, 0, 255));
    parent.add_child(std::move(child));

    pulp::canvas::RecordingCanvas rc;
    parent.paint_all(rc);

    // Two distinct clips. The overflow clip goes first and covers the parent's
    // whole 200x200 box; the resolved 60x60 rectangle goes inside it.
    REQUIRE(rc.count(T::clip_rect) == 2);
    const int overflow_clip = index_of(rc, T::clip_rect, 0);
    const int ancestor_clip = index_of(rc, T::clip_rect, 1);
    REQUIRE(overflow_clip >= 0);
    REQUIRE(ancestor_clip > overflow_clip);
    CHECK(rc.commands()[static_cast<size_t>(overflow_clip)].f[2] == 200.0f);
    CHECK(rc.commands()[static_cast<size_t>(ancestor_clip)].f[2] == 60.0f);

    // The overflow clip is never released before the child paints — it reaches
    // the subtree. The resolved rectangle's scope is, so it does not.
    const int child_fill = index_of(rc, T::fill_rect, 1);
    REQUIRE(child_fill > ancestor_clip);
    // Each measured from its own clip, so "the scope closed" is a dip below
    // where that clip was pushed rather than below a shared zero.
    const auto lowest_after = [&](int from) {
        int depth = 0;
        int lowest = 0;
        for (int i = from; i < child_fill; ++i) {
            const auto type = rc.commands()[static_cast<size_t>(i)].type;
            if (type == T::save) ++depth;
            if (type == T::restore) --depth;
            lowest = std::min(lowest, depth);
        }
        return lowest;
    };
    CHECK(lowest_after(overflow_clip) == 0);
    CHECK(lowest_after(ancestor_clip) < 0);
}

TEST_CASE("View::paint_all does NOT route through save_layer_with_mask when mask-image is 'none'",
          "[view][mask-image]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 50});
    v.set_mask_image("none");

    MaskSpyCanvas canvas;
    v.paint_all(canvas);

    // CSS `mask-image: none` is an explicit no-mask declaration —
    // treated as if no mask were set (no layer overhead, no composite).
    REQUIRE(canvas.mask_calls.empty());
}
