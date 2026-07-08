#pragma once

/// StepSequencerProcessor is the audio-thread half of a synth-class step
/// sequencer: it OWNS a SequencerStateChannel and the authoritative Snapshot,
/// drains UI edit commands off the channel via the shared step_edit_reducer
/// (drain_and_apply), echoes each applied edit back to the UI, and — while the
/// transport plays — emits sample-accurate MIDI note-ons for every enabled cell
/// of the active pattern. It is the reference consumer that proves the full
/// StepGridView (UI) <-> SequencerStateChannel (transport) <-> Processor (audio)
/// round-trip end to end.
///
/// The edit logic (apply + overflow recovery) lives in step_edit_reducer.hpp so a
/// producer — including a NON-Pulp engine — never hand-rolls the protocol.

#include <pulp/format/processor.hpp>
#include <pulp/state/sequencer_state_channel.hpp>
#include <pulp/state/step_edit_reducer.hpp>

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

        // 1. Drain the ordered UI edit stream, apply each, echo the result. The
        // shared reducer owns the apply logic + the echo-overflow recovery.
        state::drain_and_apply(channel_, snapshot_, epoch_, engine_seq_);

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
