#pragma once

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/playback/stable_renderer_shell.hpp>
#include <pulp/sequence/host_transport_projector.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulp::sequence {

struct SequenceProcessorConfig {
    std::string name = "Pulp Sequence";
    std::string manufacturer = "Pulp";
    std::string bundle_id = "com.pulp.sequence";
    std::string version = "1.0.0";
    std::uint32_t output_channels = 2;
    std::size_t maximum_note_events_per_track_per_block = 256;
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
    ExecutorFailed,
};

struct SequenceProcessObservation {
    timebase::TickPosition timeline_tick_start{};
    std::uint32_t emitted_midi_events = 0;
    bool discontinuity = false;
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
    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input, midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out, const format::ProcessContext& context) override;
    bool has_editor() const override {
        return false;
    }

    SequenceProcessorStatus status() const noexcept {
        return status_;
    }
    bool ready() const noexcept {
        return status_ == SequenceProcessorStatus::Ready;
    }
    SequenceProcessObservation last_observation() const noexcept {
        return last_observation_;
    }

  private:
    struct TrackRuntime;

    static bool process_track(format::ProcessBlock& block,
                              const format::GraphRuntimeNodeProcessContext& context,
                              void* user_data) noexcept;
    bool prepare_graph(const playback::PlaybackProgram& program, std::uint32_t maximum_block_size);
    bool topology_matches(const playback::PlaybackProgram& program) const noexcept;

    const playback::PlaybackProgramStore& store_;
    SequenceProcessorConfig config_;
    playback::PlaybackProgramBlockLatch latch_;
    HostTransportProjector transport_;
    format::GraphRuntimeExecutor executor_;
    format::GraphRuntimeSnapshot snapshot_;
    format::GraphRuntimeBufferPool pool_;
    format::GraphRuntimeMidiScratch midi_scratch_;
    std::vector<std::unique_ptr<TrackRuntime>> tracks_;
    std::vector<timeline::ItemId> track_ids_;
    const timebase::CompiledTempoMap* prepared_tempo_map_ = nullptr;
    const playback::PlaybackProgram* active_program_ = nullptr;
    const playback::TransportSnapshot* active_transport_ = nullptr;
    std::uint32_t midi_output_node_index_ = 0;
    std::uint32_t maximum_block_size_ = 0;
    SequenceProcessObservation last_observation_;
    SequenceProcessorStatus status_ = SequenceProcessorStatus::Unprepared;
};

} // namespace pulp::sequence
