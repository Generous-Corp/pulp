#pragma once

/// EXPERIMENTAL — API not yet frozen.
///
/// StepSequencerProcessor is the audio-thread half of a synth-class step
/// sequencer: it OWNS a SequencerStateChannel and the authoritative Snapshot,
/// drains UI edit commands off the channel, echoes each applied edit back to the
/// UI, and — while the transport plays — emits sample-accurate MIDI note-ons for
/// every enabled cell of the active pattern. It is the reference consumer that
/// proves the full StepGridView (UI) <-> SequencerStateChannel (transport) <->
/// Processor (audio) round-trip end to end.
///
/// The apply() switch is the authoritative audio-side edit logic: it mutates the
/// owned Snapshot and returns the exact AppliedEdit echo the UI replays, so the
/// UI never diffs the grid or re-runs an engine algorithm.

#include <pulp/format/processor.hpp>
#include <pulp/state/sequencer_state_channel.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace pulp::examples {

class StepSequencerProcessor : public format::Processor {
public:
    /// MIDI note for lane 0; each higher lane maps one semitone up.
    static constexpr std::uint8_t kBaseNote = 36;
    static constexpr std::uint8_t kChannel = 0;

    StepSequencerProcessor() { snapshot_.schema_version = 1; }

    /// The channel this processor owns. The UI binds a StepGridView to it.
    /// Outlives any view bound to it (the view holds only a pointer).
    state::SequencerStateChannel& channel() { return channel_; }
    const state::SequencerStateChannel& channel() const { return channel_; }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "StepSequencer",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.stepsequencer",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            // MIDI instrument: no audio input bus, stereo (silent) audio out so
            // hosts have a valid bus layout; the useful output is the MIDI stream.
            .input_buses = {},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = false,
            .produces_midi = true,
            .tail_samples = 0,
        };
    }

    void define_parameters(state::StateStore&) override {
        // No automatable parameters: the sequencer's state lives on the channel,
        // which is exactly the non-param bulk state the channel exists to carry.
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate > 0 ? ctx.sample_rate : 48000.0;
        current_step_ = 0;
        playhead_step_ = 0;
        samples_since_step_ = 0.0;
        was_playing_ = false;

        // Publish the initial authoritative snapshot so a freshly bound view has
        // a coherent resync point on its first pump().
        snapshot_.epoch = ++epoch_;
        snapshot_.engine_sequence = engine_seq_;
        channel_.audio_publish_snapshot(snapshot_);
        channel_.audio_mark_resync_required(snapshot_.epoch);
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>& /*input*/,
        midi::MidiBuffer& /*midi_in*/,
        midi::MidiBuffer& midi_out,
        const format::ProcessContext& ctx) override {
        // A MIDI generator produces no audio: emit silence.
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            for (float& s : output.channel(ch)) s = 0.0f;
        }

        // 1. Drain the ordered UI edit stream, apply each, echo the result.
        drain_commands();

        // 2. Sequence: emit note-ons for the active pattern's enabled cells.
        const int num_samples = static_cast<int>(output.num_samples());
        if (ctx.is_playing) {
            emit_playing_block(midi_out, ctx, num_samples);
        } else {
            was_playing_ = false;
        }

        // 3. Publish the playhead once per block.
        state::PlayheadState ph{};
        ph.playing = ctx.is_playing ? 1 : 0;
        ph.active_pattern = snapshot_.active_pattern;
        ph.active_step = playhead_step_;
        ph.ppq_position = ctx.position_beats;
        ph.sample_time = static_cast<std::uint64_t>(ctx.position_samples);
        channel_.audio_publish_playhead(ph);
    }

private:
    void drain_commands() {
        while (auto cmd = channel_.audio_try_pop_command()) {
            auto echo = apply(*cmd);
            if (!echo) continue;
            if (!channel_.audio_try_publish_applied(*echo)) {
                // Echo FIFO overflowed: the UI would miss incremental edits, so
                // republish the authoritative snapshot and raise the resync bar.
                snapshot_.epoch = ++epoch_;
                snapshot_.engine_sequence = engine_seq_;
                channel_.audio_publish_snapshot(snapshot_);
                channel_.audio_mark_resync_required(snapshot_.epoch);
            }
        }
    }

    // Authoritative audio-side edit logic. Mutates the owned snapshot and returns
    // the exact echo the UI replays. Returns nullopt only for a no-op command.
    std::optional<state::AppliedEdit> apply(const state::StepEditCommand& cmd) {
        using namespace state;
        AppliedEdit echo{};
        echo.engine_sequence = ++engine_seq_;
        echo.snapshot_epoch = snapshot_.epoch;
        echo.client_sequence = cmd.client_sequence;
        echo.transaction_id = cmd.transaction_id;

        switch (cmd.kind) {
        case StepEditKind::SetCell: {
            const auto& e = cmd.payload.set_cell;
            if (e.pattern >= kPatternCount || e.lane >= kLaneCount ||
                e.step >= kStepCount) {
                echo.kind = AppliedEditKind::CommandRejected;
                echo.payload.reject_reason = 1;
                return echo;
            }
            snapshot_.patterns[e.pattern].lanes[e.lane][e.step] = e.cell;
            echo.kind = AppliedEditKind::StepRangeChanged;
            echo.dirty = {DirtyKind::Cell, e.pattern, e.lane, e.step, 1};
            StepRangeApplied sr{};
            sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = e.step;
            sr.step_count = 1; sr.cells[0] = e.cell;
            echo.payload.step_range = sr;
            return echo;
        }
        case StepEditKind::Clear: {
            const auto& e = cmd.payload.clear;
            if (e.pattern >= kPatternCount || e.lane >= kLaneCount ||
                e.step >= kStepCount) {
                echo.kind = AppliedEditKind::CommandRejected;
                echo.payload.reject_reason = 1;
                return echo;
            }
            snapshot_.patterns[e.pattern].lanes[e.lane][e.step] = StepCell{};
            echo.kind = AppliedEditKind::StepRangeChanged;
            echo.dirty = {DirtyKind::Cell, e.pattern, e.lane, e.step, 1};
            StepRangeApplied sr{};
            sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = e.step;
            sr.step_count = 1; sr.cells[0] = StepCell{};
            echo.payload.step_range = sr;
            return echo;
        }
        case StepEditKind::RandomizeLane: {
            const auto& e = cmd.payload.randomize_lane;
            if (e.pattern >= kPatternCount || e.lane >= kLaneCount) {
                echo.kind = AppliedEditKind::CommandRejected;
                echo.payload.reject_reason = 1;
                return echo;
            }
            // Deterministic PRNG so the echo carries the exact cells the UI
            // replays — it never re-runs this algorithm.
            std::uint32_t s = e.seed ? e.seed : 1u;
            auto next = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
            StepRangeApplied sr{};
            sr.pattern = e.pattern; sr.lane = e.lane; sr.first_step = 0;
            sr.step_count = kStepCount;
            const std::uint8_t span =
                static_cast<std::uint8_t>(1u + e.max_velocity - e.min_velocity);
            for (std::uint8_t st = 0; st < kStepCount; ++st) {
                StepCell c{};
                const bool on = (next() % 128u) < e.density;
                c.flags = on ? StepCell::kEnabledBit : 0;
                c.velocity = static_cast<std::uint8_t>(
                    e.min_velocity + (span ? next() % span : 0u));
                snapshot_.patterns[e.pattern].lanes[e.lane][st] = c;
                sr.cells[st] = c;
            }
            echo.kind = AppliedEditKind::StepRangeChanged;
            echo.dirty = {DirtyKind::Lane, e.pattern, e.lane, 0, kStepCount};
            echo.payload.step_range = sr;
            return echo;
        }
        case StepEditKind::SetPatternLength: {
            const auto& e = cmd.payload.set_pattern_length;
            if (e.pattern >= kPatternCount) {
                echo.kind = AppliedEditKind::CommandRejected;
                echo.payload.reject_reason = 1;
                return echo;
            }
            snapshot_.patterns[e.pattern].length =
                e.length > kStepCount ? kStepCount : e.length;
            echo.kind = AppliedEditKind::PatternLengthChanged;
            echo.dirty = {DirtyKind::Pattern, e.pattern, 0, 0, 0};
            echo.payload.pattern_length = {e.pattern,
                                           snapshot_.patterns[e.pattern].length};
            return echo;
        }
        case StepEditKind::SwitchPattern: {
            const auto& e = cmd.payload.switch_pattern;
            if (e.pattern >= kPatternCount) {
                echo.kind = AppliedEditKind::CommandRejected;
                echo.payload.reject_reason = 1;
                return echo;
            }
            snapshot_.active_pattern = e.pattern;
            echo.kind = AppliedEditKind::ActivePatternChanged;
            echo.dirty = {DirtyKind::FullSnapshot, 0, 0, 0, 0};
            echo.payload.active_pattern = e;
            return echo;
        }
        }
        return std::nullopt;
    }

    void emit_playing_block(midi::MidiBuffer& midi_out,
                            const format::ProcessContext& ctx, int num_samples) {
        const double tempo = ctx.tempo_bpm > 0 ? ctx.tempo_bpm : 120.0;
        // Samples per 16th note.
        const double samples_per_step = (sample_rate_ * 60.0) / (tempo * 4.0);

        const state::PatternState& pat = snapshot_.patterns[snapshot_.active_pattern];
        const int length = pat.length > 0 ? pat.length : state::kStepCount;

        // On (re)start, fire step 0 at the block's first sample.
        if (!was_playing_) {
            samples_since_step_ = samples_per_step;
            current_step_ = 0;
            was_playing_ = true;
        }

        for (int i = 0; i < num_samples; ++i) {
            if (samples_since_step_ >= samples_per_step) {
                emit_step(midi_out, pat, current_step_, i);
                playhead_step_ = static_cast<std::uint8_t>(current_step_);
                current_step_ = (current_step_ + 1) % length;
                samples_since_step_ = 0.0;
            }
            samples_since_step_ += 1.0;
        }
    }

    void emit_step(midi::MidiBuffer& midi_out, const state::PatternState& pat,
                   int step, int sample_offset) {
        for (std::uint8_t lane = 0; lane < state::kLaneCount; ++lane) {
            const state::StepCell& cell = pat.lanes[lane][step];
            if (!cell.enabled()) continue;
            auto note = midi::MidiEvent::note_on(
                kChannel, static_cast<std::uint8_t>(kBaseNote + lane), cell.velocity);
            note.sample_offset = sample_offset;
            midi_out.add(note);
        }
    }

    state::SequencerStateChannel channel_{};
    state::Snapshot snapshot_{};
    state::Epoch epoch_ = 0;
    state::EngineSequence engine_seq_ = 0;

    double sample_rate_ = 48000.0;
    int current_step_ = 0;
    std::uint8_t playhead_step_ = 0;
    double samples_since_step_ = 0.0;
    bool was_playing_ = false;
};

inline std::unique_ptr<format::Processor> create_step_sequencer() {
    return std::make_unique<StepSequencerProcessor>();
}

} // namespace pulp::examples
