#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/playback/transport.hpp>

#include <cstddef>
#include <memory>

namespace pulp::examples::timeline_phase1 {

/// A bounded linear-arrangement example with parallel audio tracks, a note
/// track, device automation, tempo and meter changes, and active graph PDC.
class TimelineMultitrackArrangementProcessor final : public format::Processor {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;
    static constexpr int pdc_latency_samples = 8;

    TimelineMultitrackArrangementProcessor();
    ~TimelineMultitrackArrangementProcessor() override;
    TimelineMultitrackArrangementProcessor(const TimelineMultitrackArrangementProcessor&) = delete;
    TimelineMultitrackArrangementProcessor&
    operator=(const TimelineMultitrackArrangementProcessor&) = delete;

    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext& context) override;
    int latency_samples() const override;
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override;

    bool apply_arrangement_meter_change() noexcept;
    bool engine_prepared() const noexcept;
    int graph_latency_samples() const noexcept;
    std::size_t automation_event_count() const noexcept;
    const playback::TransportSnapshot& last_transport() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::unique_ptr<format::Processor> create_timeline_multitrack_arrangement();

} // namespace pulp::examples::timeline_phase1
