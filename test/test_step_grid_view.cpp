#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/step_grid_view.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>

using namespace pulp::view;
using namespace pulp::state;

namespace {

// Minimal headless host so a hosted StepGridView's bounded request_repaint(Rect)
// lands in an observable dirty region. The accumulation lives in the WindowHost
// base class (see test_partial_invalidation.cpp for the core-API coverage).
class TestWindowHost : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

StepCell on_cell(std::uint8_t vel) {
    StepCell c;
    c.flags = StepCell::kEnabledBit;
    c.velocity = vel;
    return c;
}

// Craft a single-cell StepRangeChanged echo (what the audio thread would publish
// after applying a SetCell edit).
AppliedEdit cell_echo(EngineSequence seq, std::uint8_t pattern, std::uint8_t lane,
                      std::uint8_t step, const StepCell& cell) {
    AppliedEdit e;
    e.engine_sequence = seq;
    e.kind = AppliedEditKind::StepRangeChanged;
    e.dirty = {DirtyKind::Cell, pattern, lane, step, 1};
    StepRangeApplied sr;
    sr.pattern = pattern; sr.lane = lane; sr.first_step = step; sr.step_count = 1;
    sr.cells[0] = cell;
    e.payload.step_range = sr;
    return e;
}

// Publish a full authoritative snapshot + raise the resync bar (bulk/resync path).
void publish_snapshot(SequencerStateChannel& ch, const Snapshot& s) {
    ch.audio_publish_snapshot(s);
    ch.audio_mark_resync_required(s.epoch);
}

PlayheadState playhead(std::uint8_t pattern, std::uint8_t step, bool playing) {
    PlayheadState p;
    p.active_pattern = pattern;
    p.active_step = step;
    p.playing = playing ? 1 : 0;
    return p;
}

// A View is non-copyable (it has identity), so configure one in place.
void setup(StepGridView& v, SequencerStateChannel& ch) {
    v.set_bounds(Rect{0, 0, 320, 96});  // 32 steps x 12 lanes
    v.set_channel(&ch);
}

} // namespace

TEST_CASE("StepGridView initial snapshot rebuilds visible cache", "[view][sequencer]") {
    SequencerStateChannel ch;
    Snapshot s;
    s.epoch = 1;
    s.engine_sequence = 5;
    s.patterns[0].lanes[2][4] = on_cell(120);
    publish_snapshot(ch, s);

    StepGridView v;
    setup(v, ch);
    auto r = v.pump();

    REQUIRE(r.resynced);
    REQUIRE(r.full_rebuild);
    REQUIRE(r.rebuilt_cells == kLaneCount * kStepCount);
    REQUIRE(v.last_engine_sequence() == 5);
    REQUIRE(v.cell_visual(2, 4).enabled);
    REQUIRE(v.cell_visual(0, 0).enabled == false);
}

TEST_CASE("StepGridView cell echo updates render copy and one cached visual",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();  // initial resync (empty)

    ch.audio_try_publish_applied(cell_echo(1, 0, 3, 7, on_cell(90)));
    auto r = v.pump();

    REQUIRE(r.echoes_applied == 1);
    REQUIRE(r.rebuilt_cells == 1);
    REQUIRE(r.requested_repaint);
    REQUIRE(v.cell_visual(3, 7).enabled);
    REQUIRE(v.render_snapshot().patterns[0].lanes[3][7].velocity == 90);
    // Neighbors untouched.
    REQUIRE(v.cell_visual(3, 6).enabled == false);
}

TEST_CASE("StepGridView echo on an off-screen pattern updates render copy but does not repaint",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();
    // displayed pattern is 0; echo targets pattern 5.
    ch.audio_try_publish_applied(cell_echo(1, 5, 1, 1, on_cell(100)));
    auto r = v.pump();

    REQUIRE(r.echoes_applied == 1);
    REQUIRE(r.rebuilt_cells == 0);
    REQUIRE(r.requested_repaint == false);
    REQUIRE(v.render_snapshot().patterns[5].lanes[1][1].enabled());  // render copy still advanced
}

TEST_CASE("StepGridView stale echo after resync is ignored", "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();

    // Advance via echoes to engine_sequence 10.
    for (EngineSequence seq = 1; seq <= 10; ++seq)
        ch.audio_try_publish_applied(cell_echo(seq, 0, 0, 0, on_cell(static_cast<std::uint8_t>(seq))));
    v.pump();
    REQUIRE(v.last_engine_sequence() == 10);

    // Resync snapshot at engine_sequence 20, then a STALE echo (seq 8) arrives.
    Snapshot s;
    s.epoch = 2;
    s.engine_sequence = 20;
    s.patterns[0].lanes[0][0] = on_cell(55);
    publish_snapshot(ch, s);
    ch.audio_try_publish_applied(cell_echo(8, 0, 0, 0, on_cell(200)));  // stale (<= 20)
    v.pump();

    REQUIRE(v.last_engine_sequence() == 20);
    // The stale echo must not have overwritten the resynced value.
    REQUIRE(v.render_snapshot().patterns[0].lanes[0][0].velocity == 55);
}

TEST_CASE("StepGridView playhead step change dirties old and new columns",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();

    ch.audio_publish_playhead(playhead(0, 5, true));
    REQUIRE(v.pump().requested_repaint);  // playhead became visible

    ch.audio_publish_playhead(playhead(0, 6, true));
    REQUIRE(v.pump().requested_repaint);  // step advanced

    ch.audio_publish_playhead(playhead(0, 6, true));
    REQUIRE(v.pump().requested_repaint == false);  // unchanged -> no repaint
}

TEST_CASE("StepGridView playhead on a different pattern is not shown", "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();
    // Engine plays pattern 3 while the editor shows pattern 0.
    ch.audio_publish_playhead(playhead(3, 4, true));
    auto r = v.pump();
    REQUIRE(r.requested_repaint == false);  // overlay stays hidden -> nothing to draw
}

TEST_CASE("StepGridView active-pattern echo advances render copy without rebuilding shown cells",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();

    AppliedEdit e;
    e.engine_sequence = 1;
    e.kind = AppliedEditKind::ActivePatternChanged;
    e.dirty = {DirtyKind::FullSnapshot, 0, 0, 0, 0};
    e.payload.active_pattern = SwitchPatternEdit{3};
    ch.audio_try_publish_applied(e);
    auto r = v.pump();

    REQUIRE(r.echoes_applied == 1);
    REQUIRE(r.rebuilt_cells == 0);            // displayed pattern (0) is independent
    REQUIRE(v.render_snapshot().active_pattern == 3);
}

TEST_CASE("StepGridView combined resync + echo + playhead reaches authoritative state",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);

    Snapshot s;
    s.epoch = 1; s.engine_sequence = 3;
    s.patterns[0].lanes[1][1] = on_cell(80);
    publish_snapshot(ch, s);
    ch.audio_try_publish_applied(cell_echo(4, 0, 2, 2, on_cell(64)));  // fresh (> 3)
    ch.audio_publish_playhead(playhead(0, 0, true));

    auto r = v.pump();
    REQUIRE(r.resynced);
    REQUIRE(r.echoes_applied == 1);
    REQUIRE(v.cell_visual(1, 1).enabled);   // from snapshot
    REQUIRE(v.cell_visual(2, 2).enabled);   // from the fresh echo
    REQUIRE(v.last_engine_sequence() == 4);
}

TEST_CASE("StepGridView mouse edit waits for the applied echo before mutating the render copy",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();

    // Click the cell at lane 0, step 0 (top-left).
    v.on_mouse_down(Point{2, 2});
    v.on_mouse_up(Point{2, 2});

    // The commands are queued but the render copy has NOT changed yet.
    REQUIRE(v.render_snapshot().patterns[0].lanes[0][0].enabled() == false);

    // The engine applies EVERY queued command (Begin + End) and echoes each.
    // A correct gesture must not queue a revert: after all commands apply, the
    // clicked cell is enabled, not back to its pre-click value.
    EngineSequence seq = 0;
    int applied_count = 0;
    while (auto cmd = ch.audio_try_pop_command()) {
        REQUIRE(cmd->kind == StepEditKind::SetCell);
        const auto& e = cmd->payload.set_cell;
        ch.audio_try_publish_applied(cell_echo(++seq, e.pattern, e.lane, e.step, e.cell));
        ++applied_count;
    }
    REQUIRE(applied_count >= 1);
    v.pump();

    REQUIRE(v.render_snapshot().patterns[0].lanes[0][0].enabled());
}

TEST_CASE("StepGridView release outside the grid does not edit a stray cell",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();

    v.on_mouse_down(Point{2, 2});          // press cell (0,0)
    v.on_mouse_up(Point{-50, -50});        // release outside the grid

    // Apply whatever was queued. The End marker must target the real last cell
    // (0,0), never a default 0,0 write on an unrelated release — and here (0,0)
    // *is* the edited cell, so the invariant is: no OTHER cell gets touched.
    EngineSequence seq = 0;
    while (auto cmd = ch.audio_try_pop_command()) {
        const auto& e = cmd->payload.set_cell;
        REQUIRE(e.lane == 0);
        REQUIRE(e.step == 0);              // only the pressed cell, never a stray
        ch.audio_try_publish_applied(cell_echo(++seq, e.pattern, e.lane, e.step, e.cell));
    }
    v.pump();
    REQUIRE(v.render_snapshot().patterns[0].lanes[0][0].enabled());
}

TEST_CASE("StepGridView pattern-length echo remutes visible cells on the shown pattern",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    Snapshot s;
    s.epoch = 1;
    s.patterns[0].lanes[0][20] = on_cell(100);  // an enabled step past a short length
    publish_snapshot(ch, s);

    StepGridView v;
    setup(v, ch);
    v.pump();
    REQUIRE(v.cell_visual(0, 20).muted == false);  // full length -> not muted

    // Shorten pattern 0 to 16 steps; step 20 now falls outside the pattern length.
    AppliedEdit e;
    e.engine_sequence = 1;
    e.kind = AppliedEditKind::PatternLengthChanged;
    e.dirty = {DirtyKind::Pattern, 0, 0, 0, 0};
    e.payload.pattern_length = {0, 16};
    ch.audio_try_publish_applied(e);

    auto r = v.pump();
    REQUIRE(r.echoes_applied == 1);
    REQUIRE(r.rebuilt_cells == kLaneCount * kStepCount);  // length remutes all visible
    REQUIRE(r.requested_repaint);
    REQUIRE(v.render_snapshot().patterns[0].length == 16);
    REQUIRE(v.cell_visual(0, 20).muted);   // now past the length
    REQUIRE(v.cell_visual(0, 15).muted == false);
    REQUIRE(v.last_engine_sequence() == 1);
}

TEST_CASE("StepGridView pattern-length echo on an off-screen pattern rebuilds no visible cell",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    StepGridView v;
    setup(v, ch);
    v.pump();  // displayed_pattern_ == 0

    AppliedEdit e;
    e.engine_sequence = 1;
    e.kind = AppliedEditKind::PatternLengthChanged;
    e.payload.pattern_length = {3, 8};   // pattern 3 is not shown
    ch.audio_try_publish_applied(e);

    auto r = v.pump();
    REQUIRE(r.echoes_applied == 1);        // echo consumed (render copy advanced)
    REQUIRE(r.rebuilt_cells == 0);         // but nothing visible rebuilt
    REQUIRE(r.requested_repaint == false); // no visible change -> no repaint
    REQUIRE(v.render_snapshot().patterns[3].length == 8);
}

TEST_CASE("StepGridView rejected-command echo advances the sequence but touches no cell",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    Snapshot s;
    s.epoch = 1;
    s.patterns[0].lanes[1][1] = on_cell(64);
    publish_snapshot(ch, s);

    StepGridView v;
    setup(v, ch);
    v.pump();

    AppliedEdit e;
    e.engine_sequence = 7;
    e.kind = AppliedEditKind::CommandRejected;
    e.payload.reject_reason = 42;
    ch.audio_try_publish_applied(e);

    auto r = v.pump();
    REQUIRE(r.echoes_applied == 1);        // consumed and sequence advanced (no re-request)
    REQUIRE(r.rebuilt_cells == 0);         // a rejection changes nothing on screen
    REQUIRE(r.requested_repaint == false);
    REQUIRE(v.last_engine_sequence() == 7);
    REQUIRE(v.cell_visual(1, 1).enabled);  // prior state intact
}

TEST_CASE("StepGridView paint draws enabled and disabled cells distinctly",
          "[view][sequencer]") {
    SequencerStateChannel ch;
    Snapshot s;
    s.epoch = 1;
    s.patterns[0].lanes[0][0] = on_cell(127);  // enabled, full velocity
    publish_snapshot(ch, s);

    StepGridView v;
    setup(v, ch);
    v.pump();

    pulp::canvas::RecordingCanvas rec;
    v.paint(rec);

    // One rounded rect per visible cell (background is a plain fill_rect).
    REQUIRE(rec.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) ==
            static_cast<size_t>(kLaneCount * kStepCount));

    // Walk the stream tracking the active fill color; assert the enabled cell's
    // rect (top-left, at cell_rect(0,0)) used a bluish accent while a disabled
    // cell used a dim grey.
    const Rect on_rect = v.cell_rect(0, 0);
    const Rect off_rect = v.cell_rect(0, 1);
    pulp::canvas::Color active{};
    bool saw_on = false, saw_off = false;
    for (const auto& cmd : rec.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_fill_color)
            active = cmd.color;
        else if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect) {
            if (cmd.f[0] == on_rect.x && cmd.f[1] == on_rect.y) {
                saw_on = true;
                REQUIRE(active.b > 0.8f);      // accent blue
                REQUIRE(active.a > 0.9f);      // full velocity -> high alpha
            } else if (cmd.f[0] == off_rect.x && cmd.f[1] == off_rect.y) {
                saw_off = true;
                REQUIRE(active.b < 0.5f);      // dim grey
            }
        }
    }
    REQUIRE(saw_on);
    REQUIRE(saw_off);
}

// ── Bounded invalidation (hosted) ─────────────────────────────────────────────
// A live grid drives a repaint every tick from the sequencer echo/playhead
// stream. With bounded invalidation it dirties only the changed cells and the
// leaving/entering playhead columns, so the static grid chrome is not
// re-composited each tick. These pin the mapped root rects a host would clip to.

namespace {
// Nest the grid at an offset inside a root so the tests also exercise the
// local->root offset mapping (grid origin folds into the dirty rect).
StepGridView* host_grid(std::unique_ptr<View>& root, TestWindowHost& host,
                        SequencerStateChannel& ch, Rect grid_bounds) {
    root = std::make_unique<View>();
    root->set_bounds({0, 0, 640, 480});
    auto grid_o = std::make_unique<StepGridView>();
    grid_o->set_bounds(grid_bounds);
    StepGridView* v = grid_o.get();
    root->add_child(std::move(grid_o));
    root->set_window_host(&host);
    v->set_channel(&ch);
    return v;
}
}  // namespace

TEST_CASE("StepGridView cell echo invalidates only that cell's mapped rect",
          "[view][sequencer][partial-render]") {
    SequencerStateChannel ch;
    TestWindowHost host;
    std::unique_ptr<View> root;
    const Rect gb{40, 30, 320, 96};
    StepGridView* v = host_grid(root, host, ch, gb);
    v->pump();                       // initial resync -> full (expected)
    host.clear_pending_dirty();

    ch.audio_try_publish_applied(cell_echo(1, 0, 3, 7, on_cell(90)));
    v->pump();

    REQUIRE_FALSE(host.pending_repaint_is_full());
    const Rect cr = v->cell_rect(3, 7);
    const Rect d = host.pending_dirty_bounds();
    REQUIRE(d.x == cr.x + gb.x);
    REQUIRE(d.y == cr.y + gb.y);
    REQUIRE(d.width == cr.width);
    REQUIRE(d.height == cr.height);
    // Sanity: a single cell is a small fraction of the whole grid.
    REQUIRE(d.width * d.height < gb.width * gb.height * 0.1f);
}

TEST_CASE("StepGridView playhead advance invalidates only the two columns",
          "[view][sequencer][partial-render]") {
    SequencerStateChannel ch;
    TestWindowHost host;
    std::unique_ptr<View> root;
    const Rect gb{40, 30, 320, 96};
    StepGridView* v = host_grid(root, host, ch, gb);
    v->pump();

    ch.audio_publish_playhead(playhead(0, 5, true));
    v->pump();                       // becomes visible at step 5
    host.clear_pending_dirty();

    ch.audio_publish_playhead(playhead(0, 6, true));
    v->pump();                       // 5 -> 6: leaving column 5 + entering column 6

    REQUIRE_FALSE(host.pending_repaint_is_full());
    const Rect c5 = v->playhead_column_rect(5);
    const Rect c6 = v->playhead_column_rect(6);
    const float ux = std::min(c5.x, c6.x);
    const float uy = std::min(c5.y, c6.y);
    const float ur = std::max(c5.x + c5.width, c6.x + c6.width);
    const float ub = std::max(c5.y + c5.height, c6.y + c6.height);
    const Rect d = host.pending_dirty_bounds();
    REQUIRE(d.x == ux + gb.x);
    REQUIRE(d.y == uy + gb.y);
    REQUIRE(d.width == ur - ux);
    REQUIRE(d.height == ub - uy);
    // Two adjacent columns are still a small slice of the full grid width.
    REQUIRE(d.width < gb.width * 0.2f);
}

TEST_CASE("StepGridView pattern-length change escalates to a full repaint",
          "[view][sequencer][partial-render]") {
    SequencerStateChannel ch;
    TestWindowHost host;
    std::unique_ptr<View> root;
    StepGridView* v = host_grid(root, host, ch, Rect{40, 30, 320, 96});
    v->pump();
    host.clear_pending_dirty();

    AppliedEdit e;
    e.engine_sequence = 1;
    e.kind = AppliedEditKind::PatternLengthChanged;
    e.dirty = {DirtyKind::FullSnapshot, 0, 0, 0, 0};
    e.payload.pattern_length = SetPatternLengthEdit{0, 8};  // shown pattern shrinks
    ch.audio_try_publish_applied(e);
    v->pump();

    REQUIRE(host.pending_repaint_is_full());  // every cell's muted flag may flip
}

TEST_CASE("StepGridView resync escalates to a full repaint",
          "[view][sequencer][partial-render]") {
    SequencerStateChannel ch;
    TestWindowHost host;
    std::unique_ptr<View> root;
    StepGridView* v = host_grid(root, host, ch, Rect{40, 30, 320, 96});
    v->pump();
    host.clear_pending_dirty();

    Snapshot s;
    s.epoch = 2;
    s.engine_sequence = 9;
    s.patterns[0].lanes[1][1] = on_cell(80);
    publish_snapshot(ch, s);
    v->pump();

    REQUIRE(host.pending_repaint_is_full());
}

// ── Freeze coverage: parametric configs + custom cell policy + self-heal ─────
namespace {
using YssConfig2 = SequencerConfig<5, 16, 8, StepCell>;
using YssChannel2 = SequencerStateChannelT<YssConfig2>;

struct VineCell2 {
    std::uint16_t growth = 0;
    std::uint8_t on = 0;
};
struct VinePolicy2 {
    static StepGridViewBase::CellVisual visual(const VineCell2& c, bool muted) {
        return {c.on != 0, std::clamp(c.growth / 65535.0f, 0.0f, 1.0f), muted};
    }
    static VineCell2 make_set_cell(const VineCell2& old, bool enabled) {
        VineCell2 c = old;
        c.on = enabled ? 1 : 0;
        if (enabled && c.growth == 0) c.growth = 1000;
        return c;
    }
};
using VineConfig2 = SequencerConfig<6, 8, 2, VineCell2>;
using VineChannel2 = SequencerStateChannelT<VineConfig2>;
} // namespace

TEST_CASE("StepGridViewT drives a second non-square config from a snapshot",
          "[view][sequencer][freeze]") {
    YssChannel2 ch;
    StepGridViewT<YssConfig2> v;
    v.set_bounds(Rect{0, 0, 160, 80});
    v.set_channel(&ch);

    YssChannel2::Snapshot s;
    s.epoch = 1;
    s.engine_sequence = 3;
    s.patterns[0].lanes[4][15].flags = StepCell::kEnabledBit;  // lane 4 (<5), step 15 (<16)
    s.patterns[0].lanes[4][15].velocity = 90;
    ch.audio_publish_snapshot(s);
    ch.audio_mark_resync_required(1);

    auto r = v.pump();
    REQUIRE(r.resynced);
    REQUIRE(v.cell_visual(4, 15).enabled);
    REQUIRE(v.cell_visual(0, 0).enabled == false);
    REQUIRE(v.last_engine_sequence() == 3);
}

TEST_CASE("StepGridViewT with a custom cell policy projects visuals and edits",
          "[view][sequencer][freeze]") {
    VineChannel2 ch;
    StepGridViewT<VineConfig2, VinePolicy2> v;
    v.set_bounds(Rect{0, 0, 80, 60});  // 8 steps x 6 lanes
    v.set_channel(&ch);

    VineChannel2::Snapshot s;
    s.epoch = 1;
    s.engine_sequence = 1;
    VineCell2 cell;
    cell.on = 1;
    cell.growth = 32768;
    s.patterns[0].lanes[2][3] = cell;
    ch.audio_publish_snapshot(s);
    ch.audio_mark_resync_required(1);
    v.pump();

    REQUIRE(v.cell_visual(2, 3).enabled);
    REQUIRE(v.cell_visual(2, 3).intensity > 0.4f);

    // Edit path: a click on cell (0,0) submits a policy-made cell through the channel.
    v.on_mouse_down(Point{5, 5});
    v.on_mouse_up(Point{5, 5});
    auto cmd = ch.audio_try_pop_command();
    REQUIRE(cmd.has_value());
    REQUIRE(cmd->kind == StepEditKind::SetCell);
    REQUIRE(cmd->payload.set_cell.cell.on == 1);  // policy enabled a fresh cell
}

TEST_CASE("StepGridView self-heals when the resync bar precedes the matching snapshot",
          "[view][sequencer][freeze]") {
    SequencerStateChannel ch;
    StepGridView v;
    v.set_bounds(Rect{0, 0, 320, 96});
    v.set_channel(&ch);

    // Reversed ordering: the bar is raised to 10 but only an epoch-5 snapshot exists.
    Snapshot s;
    s.epoch = 5;
    s.engine_sequence = 2;
    s.patterns[0].lanes[1][1] = on_cell(70);
    ch.audio_publish_snapshot(s);
    ch.audio_mark_resync_required(10);

    auto r1 = v.pump();
    REQUIRE(r1.resynced);
    REQUIRE(v.cell_visual(1, 1).enabled);  // render copy correct, never corrupted

    // Still below the bar: keeps resyncing (glitchy but safe), no corruption.
    auto r2 = v.pump();
    REQUIRE(r2.resynced);
    REQUIRE(v.cell_visual(1, 1).enabled);

    // The matching snapshot arrives; the view converges and stops resyncing.
    Snapshot s2 = s;
    s2.epoch = 10;
    ch.audio_publish_snapshot(s2);
    ch.audio_mark_resync_required(10);
    v.pump();
    auto r4 = v.pump();
    REQUIRE(r4.resynced == false);  // converged
    REQUIRE(v.cell_visual(1, 1).enabled);
}
