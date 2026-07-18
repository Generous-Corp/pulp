#pragma once

#include "timeline_example_engine.hpp"

#include <pulp/state/sequencer_state_channel.hpp>

namespace pulp::examples::timeline_phase1 {

/// One-bar pattern example. SequencerStateChannel remains the frozen typed
/// grid/UI transport; the persistent pattern is lowered to timeline NoteContent
/// and ArrangementNoteRenderer feeds a stable SignalGraph MidiInput connected
/// to the example's audible synth destination.
class TimelineStepSequencerProcessor final : public format::Processor {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    TimelineStepSequencerProcessor();

    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext& context) override;
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override;

    state::SequencerStateChannel& channel() noexcept { return channel_; }
    const state::Snapshot& pattern_snapshot() const noexcept { return pattern_; }
    bool engine_prepared() const noexcept { return engine_.prepared(); }
    bool has_active_notes() const noexcept { return engine_.synth_has_active_notes(); }
    playback::TransportError set_playing(bool playing) noexcept;
    playback::TransportError seek_samples(std::int64_t sample) noexcept;
    playback::TransportError set_loop_samples(bool enabled, std::int64_t start,
                                              std::int64_t end) noexcept;
    const playback::TransportSnapshot& last_transport() const noexcept;

  private:
    state::SequencerStateChannel channel_;
    state::Snapshot pattern_;
    state::Epoch epoch_ = 0;
    TimelineExampleEngine engine_;
};

std::unique_ptr<format::Processor> create_timeline_step_sequencer();

} // namespace pulp::examples::timeline_phase1
