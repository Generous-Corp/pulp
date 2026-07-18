#pragma once

#include "timeline_example_engine.hpp"
#include "timeline_step_pattern_content.hpp"

#include <pulp/state/sequencer_state_channel.hpp>

#include <cstdint>
#include <atomic>
#include <memory>

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
    /// Control-thread pump: applies queued channel commands, rebuilds the typed
    /// persistent component, and publishes a lowered program to the stable graph.
    bool apply_pending_edits_and_recompile();
    const timeline::Project* persistent_project() const noexcept {
        return persistent_project_.get();
    }
    const timeline::SchemaRegistry& pattern_registry() const noexcept { return registry_; }
    /// Replace the live grid from its registered persistent component. This is a
    /// bulk replacement: successful loads publish a fresh snapshot/resync epoch.
    bool load_persistent_project(const timeline::Project& project);
    playback::TransportError set_playing(bool playing) noexcept;
    playback::TransportError seek_samples(std::int64_t sample) noexcept;
    playback::TransportError set_loop_samples(bool enabled, std::int64_t start,
                                              std::int64_t end) noexcept;
    const playback::TransportSnapshot& last_transport() const noexcept;

  private:
    state::SequencerStateChannel channel_;
    state::Snapshot pattern_;
    state::Epoch epoch_ = 0;
    state::EngineSequence engine_sequence_ = 0;
    timeline::SchemaRegistry registry_;
    bool registry_ready_ = false;
    std::shared_ptr<const timeline::Project> persistent_project_;
    double sample_rate_ = 0.0;
    std::uint32_t maximum_block_size_ = 0;
    std::atomic<std::uint8_t> active_pattern_{0};
    std::atomic<std::uint8_t> active_step_count_{state::kStepCount};
    TimelineExampleEngine engine_;

    bool compile_pattern(const state::Snapshot& snapshot, bool replace_engine);
};

std::unique_ptr<format::Processor> create_timeline_step_sequencer();

} // namespace pulp::examples::timeline_phase1
