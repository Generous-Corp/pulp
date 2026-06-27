// View z-index and overflow tests for two coherent paint-order surfaces:
//
//   1. z-index paint order
//      `sorted_children_by_z_index` returns insertion order at default
//      z=0, sorts ascending so higher-z comes last, and is stable for
//      equal z. `View::paint_all` and `View::hit_test` honour the
//      sorted order so higher-z children paint on top and win
//      pointer-hit at overlapping bounds, with insertion-order
//      fallback at equal z.
//
//   2. overflow:visible default for absolute-positioned popovers plus
//      symmetric overflow:visible hit-test extension.
//      Default overflow is visible (matches CSS); `View::paint_all`
//      only emits `clip_rect` when overflow is explicitly hidden;
//      absolute children positioned outside parent bounds still paint;
//      overflow:hidden parents clip-rect their children correctly
//      (including translated absolute children).

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

// ── Z-index paint order ─────────────────────────────────────────────────

TEST_CASE("sorted_children_by_z_index returns insertion order at default z=0",
          "[view][z-index]") {
    View parent;
    auto a = std::make_unique<View>(); auto* a_ptr = a.get();
    auto b = std::make_unique<View>(); auto* b_ptr = b.get();
    auto c = std::make_unique<View>(); auto* c_ptr = c.get();
    parent.add_child(std::move(a));
    parent.add_child(std::move(b));
    parent.add_child(std::move(c));

    auto order = parent.sorted_children_by_z_index();
    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == a_ptr);
    REQUIRE(order[1] == b_ptr);
    REQUIRE(order[2] == c_ptr);
}

TEST_CASE("sorted_children_by_z_index sorts ascending — higher z comes last",
          "[view][z-index]") {
    // Spectr's bandsMenu repro: insertion order is content, content, popover,
    // but popover has zIndex=20. Sorted order must paint popover last.
    View parent;
    auto content_a = std::make_unique<View>(); auto* a_ptr = content_a.get();
    auto popover  = std::make_unique<View>(); auto* p_ptr = popover.get();
    auto content_b = std::make_unique<View>(); auto* b_ptr = content_b.get();
    popover->set_z_index(20);

    parent.add_child(std::move(content_a));
    parent.add_child(std::move(popover));
    parent.add_child(std::move(content_b));

    auto order = parent.sorted_children_by_z_index();
    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == a_ptr);    // z=0, inserted 1st
    REQUIRE(order[1] == b_ptr);    // z=0, inserted 3rd — keeps insertion order at equal z
    REQUIRE(order[2] == p_ptr);    // z=20 — last → topmost
}

TEST_CASE("sorted_children_by_z_index is stable for equal z (insertion order)",
          "[view][z-index]") {
    View parent;
    auto a = std::make_unique<View>(); auto* a_ptr = a.get();
    auto b = std::make_unique<View>(); auto* b_ptr = b.get();
    auto c = std::make_unique<View>(); auto* c_ptr = c.get();
    a->set_z_index(5);
    b->set_z_index(5);
    c->set_z_index(5);
    parent.add_child(std::move(a));
    parent.add_child(std::move(b));
    parent.add_child(std::move(c));

    auto order = parent.sorted_children_by_z_index();
    REQUIRE(order[0] == a_ptr);
    REQUIRE(order[1] == b_ptr);
    REQUIRE(order[2] == c_ptr);
}

TEST_CASE("View::paint_all paints higher-z child last so it lands on top",
          "[view][z-index]") {
    using namespace pulp::canvas;

    // Three siblings stacked at the same bounds with distinct backgrounds.
    // Popover sits in the MIDDLE of insertion order with z_index=10. The
    // last full-bounds fill in the recorded stream must be popover's
    // colour; otherwise it would be content_b's (last by insertion).
    View parent;
    parent.set_bounds({0, 0, 100, 100});

    auto content_a = std::make_unique<View>();
    content_a->set_bounds({0, 0, 100, 100});
    content_a->set_background_color(Color::rgba8(255, 0, 0, 255));   // red

    auto popover = std::make_unique<View>();
    popover->set_bounds({0, 0, 100, 100});
    popover->set_background_color(Color::rgba8(0, 200, 0, 255));     // green
    popover->set_z_index(10);

    auto content_b = std::make_unique<View>();
    content_b->set_bounds({0, 0, 100, 100});
    content_b->set_background_color(Color::rgba8(0, 0, 255, 255));   // blue

    parent.add_child(std::move(content_a));
    parent.add_child(std::move(popover));
    parent.add_child(std::move(content_b));

    RecordingCanvas rc;
    parent.paint_all(rc);

    // Walk full-bounds fill_rects and capture the last one's set_fill_color.
    Color last_fill{};
    Color last_full_bounds_fill{};
    bool saw_full_bounds = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            last_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 100.0f) {
            last_full_bounds_fill = last_fill;
            saw_full_bounds = true;
        }
    }
    REQUIRE(saw_full_bounds);
    INFO("last_full_bounds rgba=("
         << int(last_full_bounds_fill.r8()) << ","
         << int(last_full_bounds_fill.g8()) << ","
         << int(last_full_bounds_fill.b8()) << ","
         << int(last_full_bounds_fill.a8()) << ")");
    REQUIRE(last_full_bounds_fill.r8() == 0);
    REQUIRE(last_full_bounds_fill.g8() == 200);
    REQUIRE(last_full_bounds_fill.b8() == 0);
}

TEST_CASE("children_in_z_order predicate gates the paint-all alloc fast path",
          "[view][perf][zorder-fastpath]") {
    // paint_all skips the sorted_children_by_z_index() heap allocation when
    // children are already in non-decreasing z order (the dominant case). The
    // predicate must be true exactly in those cases and false only when a
    // stable_sort would actually reorder.
    SECTION("default z=0 → in order (fast path)") {
        View parent;
        parent.add_child(std::make_unique<View>());
        parent.add_child(std::make_unique<View>());
        parent.add_child(std::make_unique<View>());
        REQUIRE(parent.children_in_z_order());
    }
    SECTION("ascending z → in order (fast path)") {
        View parent;
        auto a = std::make_unique<View>(); a->set_z_index(0);
        auto b = std::make_unique<View>(); b->set_z_index(5);
        auto c = std::make_unique<View>(); c->set_z_index(10);
        parent.add_child(std::move(a));
        parent.add_child(std::move(b));
        parent.add_child(std::move(c));
        REQUIRE(parent.children_in_z_order());
    }
    SECTION("equal non-zero z → in order (fast path)") {
        View parent;
        for (int i = 0; i < 3; ++i) {
            auto v = std::make_unique<View>(); v->set_z_index(7);
            parent.add_child(std::move(v));
        }
        REQUIRE(parent.children_in_z_order());
    }
    SECTION("a later child with lower z → needs sort (slow path)") {
        View parent;
        auto a = std::make_unique<View>();             // z=0
        auto popover = std::make_unique<View>(); popover->set_z_index(20);
        auto b = std::make_unique<View>();             // z=0 after a z=20 → out of order
        parent.add_child(std::move(a));
        parent.add_child(std::move(popover));
        parent.add_child(std::move(b));
        REQUIRE_FALSE(parent.children_in_z_order());
    }
    SECTION("empty / single child are trivially in order") {
        View empty;
        REQUIRE(empty.children_in_z_order());
        View one;
        one.add_child(std::make_unique<View>());
        REQUIRE(one.children_in_z_order());
    }
}

TEST_CASE("paint_all fast path preserves insertion order for default-z children",
          "[view][perf][zorder-fastpath]") {
    using namespace pulp::canvas;
    // With all-default z (fast path), children must paint in insertion order —
    // identical to the stable-sorted order — so the last-inserted full-bounds
    // fill is the topmost. Locks behavioral equivalence of the alloc-free path
    // against the sorted path exercised by the issue-972 paint test above.
    View parent;
    parent.set_bounds({0, 0, 100, 100});

    auto first = std::make_unique<View>();
    first->set_bounds({0, 0, 100, 100});
    first->set_background_color(Color::rgba8(255, 0, 0, 255));   // red

    auto last = std::make_unique<View>();
    last->set_bounds({0, 0, 100, 100});
    last->set_background_color(Color::rgba8(0, 0, 255, 255));    // blue

    parent.add_child(std::move(first));
    parent.add_child(std::move(last));

    REQUIRE(parent.children_in_z_order());  // confirms we exercise the fast path

    RecordingCanvas rc;
    parent.paint_all(rc);

    Color last_fill{};
    Color last_full_bounds_fill{};
    bool saw_full_bounds = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            last_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 100.0f) {
            last_full_bounds_fill = last_fill;
            saw_full_bounds = true;
        }
    }
    REQUIRE(saw_full_bounds);
    REQUIRE(last_full_bounds_fill.r8() == 0);
    REQUIRE(last_full_bounds_fill.g8() == 0);
    REQUIRE(last_full_bounds_fill.b8() == 255);  // last-inserted (blue) on top
}

TEST_CASE("View::hit_test returns the highest-z child for overlapping bounds",
          "[view][z-index]") {
    // Three siblings at the same bounds: content (z=0, inserted last),
    // popover (z=10, inserted middle), content (z=0, inserted first).
    // Click in the middle should hit the popover, not whichever content
    // happened to be inserted last.
    View parent;
    parent.set_bounds({0, 0, 100, 100});

    auto a = std::make_unique<View>();
    a->set_bounds({0, 0, 100, 100});
    auto* a_ptr = a.get();

    auto popover = std::make_unique<View>();
    popover->set_bounds({0, 0, 100, 100});
    popover->set_z_index(10);
    auto* p_ptr = popover.get();

    auto b = std::make_unique<View>();
    b->set_bounds({0, 0, 100, 100});
    auto* b_ptr = b.get();

    parent.add_child(std::move(a));
    parent.add_child(std::move(popover));
    parent.add_child(std::move(b));

    auto* hit = parent.hit_test({50.0f, 50.0f});
    REQUIRE(hit == p_ptr);
    REQUIRE(hit != a_ptr);
    REQUIRE(hit != b_ptr);
}

TEST_CASE("View::hit_test falls back to insertion-order topmost at equal z",
          "[view][z-index]") {
    // All siblings at same z=0 (default) — last inserted is visually topmost,
    // matching legacy behaviour. This locks in the no-regression contract.
    View parent;
    parent.set_bounds({0, 0, 100, 100});

    auto a = std::make_unique<View>();
    a->set_bounds({0, 0, 100, 100});
    parent.add_child(std::move(a));

    auto b = std::make_unique<View>();
    b->set_bounds({0, 0, 100, 100});
    auto* b_ptr = b.get();
    parent.add_child(std::move(b));

    auto* hit = parent.hit_test({50.0f, 50.0f});
    REQUIRE(hit == b_ptr);
}

TEST_CASE("hit_test z-order fast path matches the sorted path on both branches",
          "[view][perf][zorder-fastpath]") {
    // hit_test reverse-walks children_ directly when children_in_z_order() (the
    // common default-z case, no sorted-copy allocation), else uses the sorted
    // copy. Both must return the same topmost child. Mirrors the paint_all
    // fast-path; pins behavioral parity for both branches.
    SECTION("in-order (fast path): topmost = last inserted at equal z") {
        View parent;
        parent.set_bounds({0, 0, 100, 100});
        auto a = std::make_unique<View>(); a->set_bounds({0, 0, 100, 100});
        auto b = std::make_unique<View>(); b->set_bounds({0, 0, 100, 100});
        auto* b_ptr = b.get();
        parent.add_child(std::move(a));
        parent.add_child(std::move(b));
        REQUIRE(parent.children_in_z_order());            // exercises the fast path
        REQUIRE(parent.hit_test({50.0f, 50.0f}) == b_ptr);
    }
    SECTION("out-of-order z (slow path): topmost = highest z") {
        View parent;
        parent.set_bounds({0, 0, 100, 100});
        auto a = std::make_unique<View>(); a->set_bounds({0, 0, 100, 100});
        auto popover = std::make_unique<View>(); popover->set_bounds({0, 0, 100, 100});
        popover->set_z_index(10);
        auto* p_ptr = popover.get();
        auto b = std::make_unique<View>(); b->set_bounds({0, 0, 100, 100});
        parent.add_child(std::move(a));
        parent.add_child(std::move(popover));   // high-z in the middle → out of order
        parent.add_child(std::move(b));
        REQUIRE_FALSE(parent.children_in_z_order());       // exercises the slow path
        REQUIRE(parent.hit_test({50.0f, 50.0f}) == p_ptr);
    }
}

// ── Overflow:visible default for absolute-positioned popovers ───────────
// Symptom on Spectr's bandsMenu: a `position:absolute; top:28; right:0`
// popover declared inside a 24px-tall flex parent renders nowhere because
// Pulp previously defaulted overflow to hidden and clipped paint to the
// parent's content rect. CSS default is `overflow: visible` — children
// that extend past the parent should still paint. Z-index sorting alone
// (above) is necessary but not sufficient; the clip was eating the
// popover's fill regardless of paint order.

TEST_CASE("View default overflow is visible (matches CSS default)",
          "[view][z-index]") {
    View v;
    REQUIRE(v.overflow() == View::Overflow::visible);
}

TEST_CASE("View::paint_all does not emit clip_rect when overflow is visible",
          "[view][z-index]") {
    using namespace pulp::canvas;
    View v;
    v.set_bounds({0, 0, 100, 24});
    REQUIRE(v.overflow() == View::Overflow::visible);

    RecordingCanvas rc;
    v.paint_all(rc);

    for (const auto& cmd : rc.commands()) {
        // The compositing-layer save_layer / save_backdrop_filter may
        // emit their own clip-shaped commands as part of layer setup,
        // but a plain clip_rect at the view's content bounds is the
        // overflow:hidden marker we explicitly want absent.
        if (cmd.type == DrawCommand::Type::clip_rect) {
            const bool covers_bounds = (cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
                                        cmd.f[2] == 100.0f && cmd.f[3] == 24.0f);
            INFO("Unexpected clip_rect at (" << cmd.f[0] << "," << cmd.f[1]
                 << "," << cmd.f[2] << "," << cmd.f[3] << ")");
            REQUIRE_FALSE(covers_bounds);
        }
    }
}

TEST_CASE("View::paint_all emits clip_rect when overflow is explicitly hidden",
          "[view][z-index]") {
    using namespace pulp::canvas;
    View v;
    v.set_bounds({0, 0, 100, 24});
    v.set_overflow(View::Overflow::hidden);

    RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_bounds_clip = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::clip_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 24.0f) {
            saw_bounds_clip = true;
            break;
        }
    }
    REQUIRE(saw_bounds_clip);
}

TEST_CASE("Absolute child positioned outside parent's bounds still paints",
          "[view][z-index]") {
    // Spectr bandsMenu repro: 24px-tall parent with a popover-like child
    // at top:50, left:50 — completely outside the parent's content rect.
    // With overflow:visible default, the child's translate-from-bounds.x/y
    // and its own paint commands must reach the canvas without a parent
    // clip_rect blocking them.
    using namespace pulp::canvas;

    View parent;
    parent.set_bounds({0, 0, 100, 24});

    auto popover = std::make_unique<View>();
    popover->set_bounds({50, 50, 100, 50});
    popover->set_background_color(Color::rgba8(255, 0, 255, 255));   // magenta
    parent.add_child(std::move(popover));

    RecordingCanvas rc;
    parent.paint_all(rc);

    // Verify a magenta fill_rect lands in the recorded stream — the
    // popover's bg fill is at the child's local origin, which becomes
    // (50, 50) in parent coords after the translate inside paint_all.
    Color last_fill{};
    bool saw_magenta_fill = false;
    bool saw_parent_clip = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            last_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect &&
            last_fill.r8() == 255 && last_fill.g8() == 0 &&
            last_fill.b8() == 255 && last_fill.a8() == 255) {
            saw_magenta_fill = true;
        }
        if (cmd.type == DrawCommand::Type::clip_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 24.0f) {
            saw_parent_clip = true;
        }
    }
    REQUIRE(saw_magenta_fill);
    REQUIRE_FALSE(saw_parent_clip);
}

// Paint-time probe series for the Spectr "preset items render outside overlay"
// report. The framework already pushes a parent clip_rect before children paint
// when the parent has overflow:hidden, so the symptom is consumer-side, not
// framework-side. These tests stand as a regression guard: if any of them flips
// to a fail, paint-time clipping has regressed.
TEST_CASE("Probe: overflow:hidden parent clip-rect precedes child paint",
          "[view][probe][overlay-clip]") {
    using namespace pulp::canvas;

    View parent;
    parent.set_bounds({0, 0, 100, 100});
    parent.set_overflow(View::Overflow::hidden);

    auto child = std::make_unique<View>();
    child->set_bounds({0, 0, 200, 100});
    child->set_background_color(Color::rgba8(0, 255, 0, 255));
    parent.add_child(std::move(child));

    RecordingCanvas rc;
    parent.paint_all(rc);

    int parent_clip_idx = -1;
    int child_fill_idx = -1;
    Color last_fill{};
    for (int i = 0; i < (int)rc.commands().size(); ++i) {
        const auto& cmd = rc.commands()[i];
        if (cmd.type == DrawCommand::Type::clip_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 100.0f &&
            parent_clip_idx == -1) {
            parent_clip_idx = i;
        }
        if (cmd.type == DrawCommand::Type::set_fill_color) last_fill = cmd.color;
        if (cmd.type == DrawCommand::Type::fill_rect &&
            cmd.f[2] == 200.0f && cmd.f[3] == 100.0f &&
            last_fill.g8() == 255 && last_fill.r8() == 0 &&
            child_fill_idx == -1) {
            child_fill_idx = i;
        }
    }
    INFO("parent_clip_idx=" << parent_clip_idx << " child_fill_idx=" << child_fill_idx
         << " total_cmds=" << rc.commands().size());
    REQUIRE(parent_clip_idx >= 0);
    REQUIRE(child_fill_idx >= 0);
    REQUIRE(parent_clip_idx < child_fill_idx);
}

// Paint-time probe: an absolutely-positioned child translated past the parent's
// right edge. Mirrors a Spectr preset-menu item scenario where a row's
// `position: absolute; left: 80px` puts it past a 100×40 dropdown's content
// rect. The clip must STILL precede the child's paint command in the recording;
// translates do not reset the active clip.
TEST_CASE("Probe: overflow:hidden parent clips translated absolute child too",
          "[view][probe][overlay-clip]") {
    using namespace pulp::canvas;

    View parent;
    parent.set_bounds({0, 0, 100, 40});
    parent.set_overflow(View::Overflow::hidden);

    auto child = std::make_unique<View>();
    // Bounds-x positions the child at parent-x=80, so its 60px width
    // extends 40px past the parent's right edge.
    child->set_bounds({80, 0, 60, 40});
    child->set_position(View::Position::absolute);
    child->set_background_color(Color::rgba8(0, 0, 255, 255));
    parent.add_child(std::move(child));

    RecordingCanvas rc;
    parent.paint_all(rc);

    int parent_clip_idx = -1;
    int child_fill_idx = -1;
    Color last_fill{};
    for (int i = 0; i < (int)rc.commands().size(); ++i) {
        const auto& cmd = rc.commands()[i];
        if (cmd.type == DrawCommand::Type::clip_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 40.0f &&
            parent_clip_idx == -1) {
            parent_clip_idx = i;
        }
        if (cmd.type == DrawCommand::Type::set_fill_color) last_fill = cmd.color;
        if (cmd.type == DrawCommand::Type::fill_rect &&
            cmd.f[2] == 60.0f && cmd.f[3] == 40.0f &&
            last_fill.b8() == 255 && last_fill.r8() == 0 && last_fill.g8() == 0 &&
            child_fill_idx == -1) {
            child_fill_idx = i;
        }
    }
    REQUIRE(parent_clip_idx >= 0);
    REQUIRE(child_fill_idx >= 0);
    REQUIRE(parent_clip_idx < child_fill_idx);
}
