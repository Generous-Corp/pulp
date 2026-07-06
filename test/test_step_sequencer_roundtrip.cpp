#include <catch2/catch_test_macros.hpp>

#include "../examples/StepSequencer/step_sequencer_processor.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/view/step_grid_view.hpp>

#include <cstdint>
#include <vector>

using namespace pulp;

namespace {

// Drive one process block through the host with an explicit transport context.
// Collects note-on events emitted this block into `notes` and returns whether
// the grid's pump() requested a repaint (playhead / cell change).
struct BlockResult {
    int note_ons = 0;
};

} // namespace

TEST_CASE("StepSequencer UI<->transport<->audio end-to-end round-trip",
          "[examples][sequencer][roundtrip]") {
    format::HeadlessHost host(examples::create_step_sequencer);
    host.prepare(48000, 512, /*in*/ 2, /*out*/ 2);
    auto* proc = host.processor_as<examples::StepSequencerProcessor>();
    REQUIRE(proc != nullptr);

    // Bind a real StepGridView to the processor's owned channel.
    view::StepGridView grid;
    grid.set_bounds(view::Rect{0, 0, 320, 96});  // 32 steps x 12 lanes
    grid.set_channel(&proc->channel());
    grid.pump();  // consume the initial snapshot published by prepare()

    // Scratch buffers reused for every block (silence in, silence out).
    constexpr int kBlock = 512;
    std::vector<float> in_l(kBlock, 0.0f), in_r(kBlock, 0.0f);
    std::vector<float> out_l(kBlock, 0.0f), out_r(kBlock, 0.0f);
    const float* in_ptrs[] = {in_l.data(), in_r.data()};
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<const float> input(in_ptrs, 2, kBlock);
    audio::BufferView<float> output(out_ptrs, 2, kBlock);

    double beats = 0.0;
    const double tempo = 120.0;
    auto run_block = [&](bool playing, midi::MidiBuffer& midi_out) {
        midi::MidiBuffer midi_in;
        midi_out.clear();
        format::ProcessContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.num_samples = kBlock;
        ctx.is_playing = playing;
        ctx.tempo_bpm = tempo;
        ctx.position_beats = beats;
        ctx.position_samples = static_cast<int64_t>(beats * 60.0 / tempo * 48000.0);
        host.process(output, input, midi_in, midi_out, ctx);
        beats += (kBlock / 48000.0) * (tempo / 60.0);
    };

    // The cell the "user" clicks: lane 0, step 0. Its MIDI note is kBaseNote.
    const std::uint8_t kLane = 0, kStep = 0;

    // ── UI edit: a mouse click enables one cell (Begin + End gesture). The view
    //    submits StepEditCommands but does NOT mutate its render copy yet. ──────
    // Cell geometry: 320/32 = 10 px per step, 96/12 = 8 px per lane; a point at
    // (5, 4) lands in step 0, lane 0.
    grid.on_mouse_down(view::Point{5, 4});
    grid.on_mouse_up(view::Point{5, 4});
    REQUIRE(grid.render_snapshot().patterns[0].lanes[kLane][kStep].enabled() == false);

    // ── LEG (i): UI -> audio -> echo -> UI content round-trip. ────────────────
    // A stopped block drains the queued commands and publishes the echoes; then
    // the grid replays them into its render copy + visual cache.
    {
        midi::MidiBuffer midi_out;
        run_block(/*playing=*/false, midi_out);
        REQUIRE(midi_out.size() == 0);  // stopped transport emits no notes
    }
    grid.pump();
    REQUIRE(grid.cell_visual(kLane, kStep).enabled);          // enabled cell round-tripped
    REQUIRE(grid.cell_visual(kLane, kStep + 1).enabled == false);  // neighbor still off
    REQUIRE(grid.cell_visual(kLane + 1, kStep).enabled == false);  // other lane still off

    // The velocity the engine applied + echoed (the view's default for a freshly
    // enabled cell). Used below to prove the emitted MIDI matches.
    const std::uint8_t applied_velocity =
        grid.render_snapshot().patterns[0].lanes[kLane][kStep].velocity;
    REQUIRE(applied_velocity > 0);

    // ── LEG (ii) + (iii): play the transport across the enabled step and the
    //    disabled steps that follow it, collecting MIDI + watching the playhead. ─
    // samples_per_step at 120 BPM / 48 kHz = 6000. In ~25600 samples (50 blocks)
    // the sequencer crosses steps 0..4: only step 0 is enabled, so exactly the
    // note for lane 0 should appear, and none for the disabled steps.
    int total_note_ons = 0;
    int matching_note_ons = 0;
    int foreign_note_ons = 0;
    bool overlay_repainted = false;
    const std::uint8_t expected_note =
        static_cast<std::uint8_t>(examples::StepSequencerProcessor::kBaseNote + kLane);

    for (int block = 0; block < 50; ++block) {
        midi::MidiBuffer midi_out;
        run_block(/*playing=*/true, midi_out);
        for (const auto& ev : midi_out) {
            if (!ev.is_note_on()) continue;
            ++total_note_ons;
            if (ev.note() == expected_note && ev.velocity() == applied_velocity)
                ++matching_note_ons;
            else
                ++foreign_note_ons;
        }
        if (grid.pump().requested_repaint) overlay_repainted = true;
    }

    // Leg (ii): the enabled step fired at least once with the right note/velocity,
    // and no disabled step (or empty lane) ever produced a note.
    REQUIRE(matching_note_ons >= 1);
    REQUIRE(foreign_note_ons == 0);
    REQUIRE(total_note_ons == matching_note_ons);

    // Leg (iii): the audio thread advanced the playhead across steps and the UI
    // observed it (overlay repaint on the displayed, playing pattern).
    const state::PlayheadState ph = proc->channel().ui_read_playhead();
    REQUIRE(ph.playing == 1);
    REQUIRE(ph.active_pattern == grid.displayed_pattern());
    REQUIRE(ph.active_step > 0);         // stepped past step 0
    REQUIRE(overlay_repainted);          // the view repainted for the moving overlay
}
