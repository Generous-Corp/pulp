#pragma once

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/playback/realtime_stretch_renderer.hpp>
#include <pulp/playback/stable_renderer_shell.hpp>
#include <pulp/runtime/slot.hpp>
#include <pulp/sequence/host_transport_projector.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulp::sequence {

namespace detail {
class MidiLatencyQueue;
}

struct SequenceProcessorConfig {
    std::string name = "Pulp Sequence";
    std::string manufacturer = "Pulp";
    std::string bundle_id = "com.pulp.sequence";
    std::string version = "1.0.0";
    std::uint32_t output_channels = 2;
    std::size_t maximum_note_events_per_track_per_block = 256;
    /// Admission ceiling for each prepared MIDI/UMP latency ring. The processor
    /// prepares up to this bound and fails closed if sustained traffic fills it.
    std::size_t maximum_delayed_events = 262'144;
};

enum class SequenceProcessorStatus : std::uint8_t {
    Unprepared,
    Ready,
    MissingProgram,
    InvalidConfiguration,
    SampleRateMismatch,
    TopologyChanged,
    TransportRejected,
    RenderFailed,
    RealtimeStretchRejected,
    ExecutorFailed,
};

struct SequenceProcessObservation {
    timebase::TickPosition timeline_tick_start{};
    std::uint32_t emitted_midi_events = 0;
    bool discontinuity = false;
    playback::AudioRenderStatus audio_status = playback::AudioRenderStatus::Silent;
    bool valid = false;
};

/// Format-layer adapter for embedding an immutable PlaybackProgram in a
/// VST3/AU/CLAP processor. It owns no plugin host and executes the same
/// GraphRuntimeExecutor::process_routed path as desktop timeline playback.
class SequenceProcessor final : public format::Processor {
  public:
    SequenceProcessor(const playback::PlaybackProgramStore& store,
                      SequenceProcessorConfig config = {});
    ~SequenceProcessor() override;

    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore& store) override;
    void prepare(const format::PrepareContext& context) override;
    void release() override;
    /// Control-thread operation. Prepares an exact program/runtime pair for
    /// the latest same-topology publication, then atomically exposes it to the
    /// callback. Failure preserves the currently active pair.
    bool adopt_latest_program();
    void force_realtime_stretch_failure_for_test() noexcept;
    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input, midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out, const format::ProcessContext& context) override;
    int latency_samples() const override {
        return prepared_ ? static_cast<int>(prepared_latency_samples_) : 0;
    }
    bool has_editor() const override {
        return false;
    }

    SequenceProcessorStatus status() const noexcept {
        return status_;
    }
    bool ready() const noexcept {
        return prepared_;
    }
    SequenceProcessObservation last_observation() const noexcept {
        return last_observation_;
    }

  private:
    struct TrackRuntime;
    struct PreparedPublication;

    static bool process_track(format::ProcessBlock& block,
                              const format::GraphRuntimeNodeProcessContext& context,
                              void* user_data) noexcept;
    bool prepare_graph(const PreparedPublication& publication, std::uint32_t maximum_block_size);
    bool topology_matches(const playback::PlaybackProgram& program) const noexcept;

    const playback::PlaybackProgramStore& store_;
    SequenceProcessorConfig config_;
    HostTransportProjector transport_;
    format::GraphRuntimeExecutor executor_;
    format::GraphRuntimeSnapshot snapshot_;
    format::GraphRuntimeBufferPool pool_;
    format::GraphRuntimeMidiScratch midi_scratch_;
    std::vector<std::unique_ptr<TrackRuntime>> tracks_;
    std::vector<timeline::ItemId> track_ids_;
    runtime::Slot<PreparedPublication> publication_;
    const timebase::CompiledTempoMap* prepared_tempo_map_ = nullptr;
    const playback::PlaybackProgram* active_program_ = nullptr;
    const playback::TransportSnapshot* active_transport_ = nullptr;
    playback::RealtimeStretchProgramRuntime* active_realtime_stretch_ = nullptr;
    std::uint32_t midi_output_node_index_ = 0;
    std::uint32_t maximum_block_size_ = 0;
    std::uint32_t prepared_latency_samples_ = 0;
    std::unique_ptr<detail::MidiLatencyQueue> midi_delay_;
    const playback::PlaybackProgram* midi_delay_publication_ = nullptr;
    std::uint64_t midi_delay_playback_epoch_ = 0;
    bool prepared_ = false;
    SequenceProcessObservation last_observation_;
    std::atomic<playback::AudioRenderStatus> block_audio_status_{
        playback::AudioRenderStatus::Silent};
    SequenceProcessorStatus status_ = SequenceProcessorStatus::Unprepared;
};

} // namespace pulp::sequence
