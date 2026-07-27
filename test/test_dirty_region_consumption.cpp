// Host-side consumption of the accumulated dirty region: WindowHost::paint_root
// clips the canvas to pending_dirty_bounds() ONLY when the caller affirms its
// surface preserves content outside the clip AND a bounded (non-full) repaint is
// pending; otherwise it paints the whole tree (the safe default). It always
// clears the pending region afterward. These pin that behavior with a
// RecordingCanvas (deterministic, no raster backend).

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>
#include <memory>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

namespace {

class TestWindowHost : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

// Count clip_rect commands whose rect equals (x,y,w,h). The tests use distinctive
// bounds a plain root's own painting never emits, so a match is unambiguously the
// host's dirty clip.
int count_clip_rects_at(const RecordingCanvas& rec, float x, float y, float w, float h) {
    int n = 0;
    for (const auto& c : rec.commands())
        if (c.type == DrawCommand::Type::clip_rect &&
            c.f[0] == x && c.f[1] == y && c.f[2] == w && c.f[3] == h)
            ++n;
    return n;
}

// paint_root's outer clip save() must be matched by its restore(), and paint_all
// must leave the canvas depth balanced. A plain root paints with balanced
// plain save/restore (no opacity/backdrop layers), so the whole recording must
// carry equal save and restore counts — a dropped restore in paint_root shows up
// here as an imbalance.
bool save_restore_balanced(const RecordingCanvas& rec) {
    int depth = 0;
    for (const auto& c : rec.commands()) {
        if (c.type == DrawCommand::Type::save) ++depth;
        else if (c.type == DrawCommand::Type::restore) --depth;
        if (depth < 0) return false;  // restore without a matching save
    }
    return depth == 0;
}

constexpr bool kSurfacePreserved = true;

}  // namespace

TEST_CASE("paint_root clips to the bounded dirty region when the surface is preserved",
          "[view][partial-render]") {
    TestWindowHost host;
    View root;
    root.set_bounds({0, 0, 400, 300});

    host.clear_pending_dirty();  // past the always-full first frame
    host.mark_dirty(Rect{40, 30, 20, 120});  // a live sub-view invalidated its rect
    REQUIRE_FALSE(host.pending_repaint_is_full());

    RecordingCanvas rec;
    host.paint_root(rec, root, kSurfacePreserved);

    REQUIRE(count_clip_rects_at(rec, 40, 30, 20, 120) == 1);
    REQUIRE(save_restore_balanced(rec));       // outer clip save/restore balanced
    REQUIRE_FALSE(host.has_pending_dirty_bounds());  // pending region cleared
    REQUIRE_FALSE(host.pending_repaint_is_full());
}

TEST_CASE("paint_root defaults to a full paint even with a bounded region pending",
          "[view][partial-render]") {
    // Without the surface-preserved affirmation, a bounded pending region must NOT
    // clip — clipping a non-preserving surface would blank the static chrome.
    TestWindowHost host;
    View root;
    root.set_bounds({0, 0, 400, 300});

    host.clear_pending_dirty();
    host.mark_dirty(Rect{40, 30, 20, 120});
    REQUIRE_FALSE(host.pending_repaint_is_full());

    RecordingCanvas rec;
    host.paint_root(rec, root);  // default: surface NOT affirmed as preserving

    REQUIRE(count_clip_rects_at(rec, 40, 30, 20, 120) == 0);  // painted full, no clip
    REQUIRE(save_restore_balanced(rec));
    REQUIRE_FALSE(host.has_pending_dirty_bounds());            // still cleared
}

TEST_CASE("paint_root paints unclipped for a full repaint", "[view][partial-render]") {
    TestWindowHost host;
    View root;
    root.set_bounds({0, 0, 400, 300});

    host.mark_dirty();  // full (theme change / first frame)
    REQUIRE(host.pending_repaint_is_full());

    RecordingCanvas rec;
    host.paint_root(rec, root, kSurfacePreserved);  // affirmed, but pending is full

    REQUIRE(count_clip_rects_at(rec, 0, 0, 400, 300) == 0);
    REQUIRE(save_restore_balanced(rec));
    REQUIRE_FALSE(host.has_pending_dirty_bounds());
}

TEST_CASE("paint_root coalesced bounds clip covers two invalidated sub-views",
          "[view][partial-render]") {
    TestWindowHost host;
    View root;
    root.set_bounds({0, 0, 400, 300});

    host.clear_pending_dirty();  // past the always-full first frame
    host.mark_dirty(Rect{10, 10, 20, 20});
    host.mark_dirty(Rect{100, 100, 20, 20});  // bounding box → {10,10,110,110}
    REQUIRE_FALSE(host.pending_repaint_is_full());

    RecordingCanvas rec;
    host.paint_root(rec, root, kSurfacePreserved);

    REQUIRE(count_clip_rects_at(rec, 10, 10, 110, 110) == 1);
    REQUIRE(save_restore_balanced(rec));
}

TEST_CASE("paint_root after a clean frame (no marks) paints full",
          "[view][partial-render]") {
    TestWindowHost host;
    View root;
    root.set_bounds({0, 0, 400, 300});

    // First frame consumes the initial full mark.
    RecordingCanvas first;
    host.paint_root(first, root, kSurfacePreserved);
    REQUIRE_FALSE(host.has_pending_dirty_bounds());

    // With nothing marked since, the next frame is still a full (unclipped) paint,
    // not an empty-region clip that would paint nothing.
    RecordingCanvas second;
    host.paint_root(second, root, kSurfacePreserved);
    REQUIRE(count_clip_rects_at(second, 0, 0, 400, 300) == 0);
    REQUIRE(save_restore_balanced(second));
}

// ── WindowHost's take/restore pair (WAH-8) ──────────────────────────────────
//
// Both hosts now share ONE PendingDamage. WindowHost's half of the consume
// contract is these two accessors: read-and-clear in one step, and hand the
// snapshot back when the frame did not reach its output. Before the split,
// WindowHost carried its own dirty_full_/have_dirty_bounds_/dirty_bounds_ trio
// plus a copy of the union logic, which is how the two hosts drifted apart on
// what a bounded mark even means.

TEST_CASE("WindowHost::take_pending_dirty reads and clears in one step",
          "[dirty-region][wah-8]") {
    TestWindowHost host;
    host.clear_pending_dirty();  // past the always-full first frame
    host.mark_dirty(Rect{10, 20, 30, 40});
    REQUIRE(host.has_pending_dirty_bounds());

    const auto frame = host.take_pending_dirty();

    REQUIRE(frame.is_bounded());
    REQUIRE(frame.bounds().x == 10.0f);
    REQUIRE(frame.bounds().width == 30.0f);
    // Cleared, so a mark arriving now belongs to the NEXT frame.
    REQUIRE_FALSE(host.pending_repaint_is_full());
    REQUIRE_FALSE(host.has_pending_dirty_bounds());
}

TEST_CASE("WindowHost::restore_pending_dirty puts a failed frame's damage back",
          "[dirty-region][wah-8]") {
    TestWindowHost host;
    host.clear_pending_dirty();
    host.mark_dirty(Rect{5, 5, 10, 10});
    const auto frame = host.take_pending_dirty();

    host.restore_pending_dirty(frame);

    REQUIRE(host.has_pending_dirty_bounds());
    REQUIRE(host.pending_dirty_bounds().x == 5.0f);
    REQUIRE_FALSE(host.pending_repaint_is_full());
}

TEST_CASE("WindowHost restore unions with damage marked meanwhile",
          "[dirty-region][wah-8]") {
    // A repaint request arriving while the frame was in flight must survive the
    // restore — overwriting instead of unioning would drop it.
    TestWindowHost host;
    host.clear_pending_dirty();
    host.mark_dirty(Rect{0, 0, 10, 10});
    const auto frame = host.take_pending_dirty();
    host.mark_dirty(Rect{100, 100, 10, 10});

    host.restore_pending_dirty(frame);

    const Rect b = host.pending_dirty_bounds();
    REQUIRE(b.x == 0.0f);
    REQUIRE(b.x + b.width == 110.0f);
    REQUIRE(b.y + b.height == 110.0f);
}

TEST_CASE("WindowHost restore of a full snapshot stays full",
          "[dirty-region][wah-8]") {
    TestWindowHost host;
    const auto frame = host.take_pending_dirty();  // the always-full first frame
    REQUIRE(frame.is_full());
    host.clear_pending_dirty();
    host.mark_dirty(Rect{1, 1, 2, 2});

    host.restore_pending_dirty(frame);

    // Sticky-full survives a restore: the failed frame was repainting
    // everything, so its retry must too.
    REQUIRE(host.pending_repaint_is_full());
}
