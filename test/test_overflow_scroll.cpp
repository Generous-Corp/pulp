// Scrollable overflow: an `overflow: scroll` (CSS `scroll` / `auto`) container
// offsets its children, consumes the wheel, clamps at both ends, and clips to
// its box — and, critically, HIT-TESTS where it PAINTS.
//
// The load-bearing test here is "every row owns its painted centre at a
// non-zero scroll offset". It reads each row's painted rectangle out of a
// RecordingCanvas command stream — replaying save/restore/translate to
// recover the real composed transform — and then asserts hit_test() at that
// measured centre returns that same row. Deriving the expected centre
// arithmetically instead would only restate the offset the code already
// applied, and would pass even if paint and hit-testing disagreed.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/view.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <vector>

using namespace pulp::view;
using pulp::canvas::Color;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

namespace {

constexpr float kViewportW = 120.0f;
constexpr float kViewportH = 100.0f;
constexpr float kRowH = 40.0f;

/// Distinct, easily-recognised background per row: red channel = row index+1.
Color row_color(int index) {
    return Color::rgba8(static_cast<uint8_t>(index + 1), 0, 0, 255);
}

/// A scroll container at the origin with `rows` stacked children, each given
/// explicit bounds and a unique background so paint can be attributed.
std::unique_ptr<View> make_scroller(int rows, float viewport_h = kViewportH) {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, kViewportW, viewport_h});
    root->set_overflow(View::Overflow::scroll);
    for (int i = 0; i < rows; ++i) {
        auto row = std::make_unique<View>();
        row->set_bounds({0, static_cast<float>(i) * kRowH, kViewportW, kRowH});
        row->set_background_color(row_color(i));
        root->add_child(std::move(row));
    }
    return root;
}

MouseEvent wheel_by(float dy) {
    MouseEvent e;
    e.is_wheel = true;
    e.scroll_delta_y = dy;
    return e;
}

/// Row index → the rectangle that row ACTUALLY painted, in root coordinates.
///
/// Replays the recorded command stream maintaining the translate + fill-colour
/// state the way a real canvas would, so the rect reflects every transform
/// paint composed — including the scroll offset under test.
std::map<int, pulp::view::Rect> painted_row_rects(const RecordingCanvas& canvas, int rows) {
    struct State {
        float tx = 0.0f, ty = 0.0f;
        Color fill{};
    };
    std::vector<State> stack{State{}};
    std::map<int, pulp::view::Rect> out;

    for (const auto& cmd : canvas.commands()) {
        State& top = stack.back();
        switch (cmd.type) {
        case DrawCommand::Type::save:
            stack.push_back(top);
            break;
        case DrawCommand::Type::restore:
            if (stack.size() > 1)
                stack.pop_back();
            break;
        case DrawCommand::Type::translate:
            top.tx += cmd.f[0];
            top.ty += cmd.f[1];
            break;
        case DrawCommand::Type::set_fill_color:
            top.fill = cmd.color;
            break;
        case DrawCommand::Type::fill_rect:
        case DrawCommand::Type::fill_rounded_rect: {
            for (int i = 0; i < rows; ++i) {
                const Color want = row_color(i);
                // Tolerance well below one 8-bit step (1/255), so rows stay
                // distinguishable while surviving any float round-trip.
                constexpr float kEps = 1.0f / 1024.0f;
                if (std::abs(top.fill.r - want.r) > kEps || std::abs(top.fill.g - want.g) > kEps ||
                    std::abs(top.fill.b - want.b) > kEps)
                    continue;
                // First fill wins: a row paints its background once.
                if (out.count(i))
                    break;
                out[i] = pulp::view::Rect{top.tx + cmd.f[0], top.ty + cmd.f[1], cmd.f[2], cmd.f[3]};
                break;
            }
            break;
        }
        default:
            break;
        }
    }
    return out;
}

} // namespace

// ── Range + the negative control for the whole feature ───────────────────

TEST_CASE("overflow:scroll container with overflowing content is scrollable",
          "[view][overflow-scroll]") {
    auto root = make_scroller(/*rows=*/10); // 400px of content in 100px

    REQUIRE(root->scroll_content_size().height > kViewportH);
    REQUIRE(root->max_scroll_offset_y() == Catch::Approx(300.0f));
    REQUIRE(root->wants_wheel_scroll());
}

TEST_CASE("a container whose content fits does not scroll", "[view][overflow-scroll]") {
    // The negative control for the feature: the same code path, content that
    // fits. Everything must report "no scrolling here".
    auto root = make_scroller(/*rows=*/2); // 80px of content in 100px

    REQUIRE(root->max_scroll_offset_y() == 0.0f);
    REQUIRE_FALSE(root->wants_wheel_scroll());
    REQUIRE_FALSE(root->scroll_offset_by(0.0f, 50.0f));
    REQUIRE(root->scroll_offset_y() == 0.0f);

    root->on_mouse_event(wheel_by(60.0f));
    REQUIRE(root->scroll_offset_y() == 0.0f);
}

TEST_CASE("overflow:visible and overflow:hidden containers never scroll",
          "[view][overflow-scroll]") {
    // Scrolling is opt-in via `overflow: scroll`; the other two keywords keep
    // their existing meaning and must not start moving content.
    for (auto mode : {View::Overflow::visible, View::Overflow::hidden}) {
        auto root = make_scroller(/*rows=*/10);
        root->set_overflow(mode);
        REQUIRE_FALSE(root->is_scroll_container());
        REQUIRE_FALSE(root->wants_wheel_scroll());
        REQUIRE(root->max_scroll_offset_y() == 0.0f);
        root->on_mouse_event(wheel_by(80.0f));
        REQUIRE(root->scroll_offset_y() == 0.0f);
    }
}

// ── Wheel + clamping at both ends ────────────────────────────────────────

TEST_CASE("wheel scrolls an overflow:scroll container and clamps at both ends",
          "[view][overflow-scroll]") {
    auto root = make_scroller(/*rows=*/10);
    const float max_scroll = root->max_scroll_offset_y();
    REQUIRE(max_scroll > 0.0f); // control: there is something to clamp

    root->on_mouse_event(wheel_by(40.0f));
    REQUIRE(root->scroll_offset_y() == Catch::Approx(40.0f));

    // Past the far end stops at the end, not beyond it.
    root->on_mouse_event(wheel_by(10'000.0f));
    REQUIRE(root->scroll_offset_y() == Catch::Approx(max_scroll));

    // Past the near end stops at 0, never negative.
    root->on_mouse_event(wheel_by(-10'000.0f));
    REQUIRE(root->scroll_offset_y() == 0.0f);
}

// ── Paint is clipped to the box ──────────────────────────────────────────

TEST_CASE("overflow:scroll clips its content to its own box", "[view][overflow-scroll]") {
    auto root = make_scroller(/*rows=*/10);
    root->set_scroll_offset(0.0f, 120.0f);

    RecordingCanvas canvas;
    root->paint_all(canvas);

    bool clipped_to_box = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::clip_rect)
            continue;
        if (cmd.f[2] == Catch::Approx(kViewportW) && cmd.f[3] == Catch::Approx(kViewportH)) {
            clipped_to_box = true;
            break;
        }
    }
    REQUIRE(clipped_to_box);
}

// ── Hit-testing agrees with paint at a non-zero offset ───────────────────

TEST_CASE("every visible row owns its painted centre at a non-zero scroll offset",
          "[view][overflow-scroll]") {
    auto root = make_scroller(/*rows=*/10);
    const float offset = 130.0f; // deliberately not a whole row height
    REQUIRE(root->set_scroll_offset(0.0f, offset));
    REQUIRE(root->scroll_offset_y() == Catch::Approx(offset));

    RecordingCanvas canvas;
    root->paint_all(canvas);
    const auto painted = painted_row_rects(canvas, /*rows=*/10);

    // Control: paint really moved. At this offset row 0 must have been pushed
    // fully above the viewport, and some row below it must now be painting
    // inside the box — otherwise the "ownership" loop below could pass while
    // nothing scrolled at all.
    REQUIRE(painted.count(0) == 1);
    REQUIRE(painted.at(0).y < 0.0f);

    int checked = 0;
    for (const auto& [index, rect] : painted) {
        const float cx = rect.x + rect.width * 0.5f;
        const float cy = rect.y + rect.height * 0.5f;
        // Only rows whose painted centre lands inside the viewport are
        // reachable; a clipped-away row is correctly not hit-testable.
        if (cy < 0.0f || cy >= kViewportH)
            continue;

        View* hit = root->hit_test({cx, cy});
        REQUIRE(hit != nullptr);
        REQUIRE(hit == root->child_at(static_cast<size_t>(index)));
        ++checked;
    }
    // Non-vacuity: a viewport this tall shows parts of at least three rows.
    REQUIRE(checked >= 2);
}

TEST_CASE("content past the viewport becomes reachable by scrolling", "[view][overflow-scroll]") {
    // The defect this feature exists to fix: a tall container's tail rows are
    // simply unreachable. Prove the last row is out of reach at rest and in
    // reach once scrolled — measured through paint, then pressed.
    auto root = make_scroller(/*rows=*/10);
    auto* last = root->child_at(9);

    {
        RecordingCanvas canvas;
        root->paint_all(canvas);
        const auto painted = painted_row_rects(canvas, 10);
        REQUIRE(painted.count(9) == 1);
        const auto& r = painted.at(9);
        // At rest the last row paints entirely below the viewport.
        REQUIRE(r.y >= kViewportH);
        // And nothing in the viewport belongs to it.
        REQUIRE(root->hit_test({kViewportW * 0.5f, kViewportH * 0.5f}) != last);
    }

    root->set_scroll_offset(0.0f, root->max_scroll_offset_y());

    RecordingCanvas canvas;
    root->paint_all(canvas);
    const auto painted = painted_row_rects(canvas, 10);
    REQUIRE(painted.count(9) == 1);
    const auto& r = painted.at(9);
    const float cx = r.x + r.width * 0.5f;
    const float cy = r.y + r.height * 0.5f;
    REQUIRE(cy >= 0.0f);
    REQUIRE(cy < kViewportH);
    REQUIRE(root->hit_test({cx, cy}) == last);
}

TEST_CASE("scroll offset survives a programmatic set and reports movement",
          "[view][overflow-scroll]") {
    auto root = make_scroller(/*rows=*/10);
    REQUIRE(root->set_scroll_offset(0.0f, 50.0f));
    // A no-op set reports false so callers can skip a repaint.
    REQUIRE_FALSE(root->set_scroll_offset(0.0f, 50.0f));
    // Out-of-range values clamp rather than being rejected.
    REQUIRE(root->set_scroll_offset(0.0f, -1000.0f));
    REQUIRE(root->scroll_offset_y() == 0.0f);
}
