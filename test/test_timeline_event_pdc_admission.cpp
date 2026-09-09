// Control-thread admission of event-device latency. Latency is discovered once,
// off the audio thread, and an unanswerable or out-of-range report is refused
// rather than folded into a confident zero that a later alignment proof would
// happily certify.
#include "../core/host/src/timeline_graph_binding_internal.hpp"
#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/playback/event_compensation.hpp>
#include <pulp/playback/note_renderer.hpp>

#include <optional>

namespace {

using pulp::host::detail::timeline_graph_binding::admit_event_compensation;
using pulp::host::detail::timeline_graph_binding::event_domain_latency_samples;
using pulp::host::detail::timeline_graph_binding::kEventDeviceLatencyCeilingSamples;
using pulp::host::detail::timeline_graph_binding::resolve_event_device_latency;

/// A basic-instrument slot whose latency report is under the test's control.
class LatencyReportingSlot final : public PluginSlot {
  public:
    LatencyReportingSlot(std::unique_ptr<PluginSlot> inner, int latency, LatencyQuery query)
        : inner_(std::move(inner)), latency_(latency), query_(query) {}

    /// Pulp as a host currently offers a hosted plugin no latency-changed
    /// callback, so a device that varies its latency mid-stream is not
    /// observable through any real backend. This synthetic source stands in for
    /// one so the hold-and-relatch behaviour is proven rather than assumed.
    void report_latency(int latency) noexcept {
        latency_ = latency;
    }

    const PluginInfo& info() const override {
        return inner_->info();
    }
    bool is_loaded() const override {
        return inner_->is_loaded();
    }
    bool prepare(double sample_rate, int maximum_block_size) override {
        return inner_->prepare(sample_rate, maximum_block_size);
    }
    void release() override {
        inner_->release();
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& events, int frames) override {
        inner_->process(output, input, midi_in, midi_out, events, frames);
    }
    std::vector<HostParamInfo> parameters() const override {
        return inner_->parameters();
    }
    float get_parameter(std::uint32_t id) const override {
        return inner_->get_parameter(id);
    }
    void set_parameter(std::uint32_t id, float value) override {
        inner_->set_parameter(id, value);
    }
    void set_bypass(bool bypassed) override {
        inner_->set_bypass(bypassed);
    }
    bool is_bypassed() const override {
        return inner_->is_bypassed();
    }
    std::vector<std::uint8_t> save_state() const override {
        return inner_->save_state();
    }
    bool restore_state(const std::vector<std::uint8_t>& state) override {
        return inner_->restore_state(state);
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return latency_;
    }
    int tail_samples() const override {
        return inner_->tail_samples();
    }
    LatencyQuery latency_query() const override {
        return query_;
    }

  private:
    std::unique_ptr<PluginSlot> inner_;
    int latency_ = 0;
    LatencyQuery query_ = LatencyQuery::Available;
};

struct LatencyProfile {
    int latency = 0;
    PluginSlot::LatencyQuery query = PluginSlot::LatencyQuery::Available;
};

LatencyProfile* selected_latency_profile = nullptr;

std::unique_ptr<PluginSlot> latency_reporting_factory(const PluginInfo& info) {
    auto inner = load_builtin_plugin(info);
    if (!inner || selected_latency_profile == nullptr)
        return inner;
    return std::make_unique<LatencyReportingSlot>(std::move(inner), selected_latency_profile->latency,
                                                  selected_latency_profile->query);
}

class ScopedLatencyProfile {
  public:
    explicit ScopedLatencyProfile(LatencyProfile& profile) : previous_(selected_latency_profile) {
        selected_latency_profile = &profile;
    }
    ~ScopedLatencyProfile() {
        selected_latency_profile = previous_;
    }

  private:
    LatencyProfile* previous_ = nullptr;
};

std::shared_ptr<const Project> instrument_project(const CompiledTempoMap& map) {
    auto content = take(MidiContent::create({note(map, 101, 5, 48)}));
    auto clip = take(Clip::create({100}, {0}, map.samples_to_ticks({128}) - TickPosition{0},
                                  std::move(content)));
    DeviceConfiguration configuration{
        .position = DeviceChainPosition::PreFader,
        .slot_kind = DeviceSlotKind::EventToAudio,
        .device_kind = DeviceKind::BuiltIn,
        .binding_key = std::string(kBasicInstrumentBindingKey),
    };
    auto track = take(Track::create(TrackInput{
        .id = {10},
        .name = "latency instrument",
        .clips = {std::move(clip)},
        .device_chain = {DevicePlacement{{20}, std::move(configuration), std::nullopt}},
    }));
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                          std::vector<Track>{std::move(track)}));
    return std::make_shared<const Project>(take(Project::create(
        ProjectInput{{1}, "latency instrument", 1'000, {2}, {}, {std::move(sequence)}})));
}

TimelineGraphAdmission admit_with(LatencyProfile profile, SignalGraph& graph,
                                  playback::EventCompensationShift& shift) {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(instrument_project(*map), map, take(DecodedAudioAssetPool::create({})), 1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    ScopedLatencyProfile selected(profile);
    binding.set_timeline_device_factory_for_test(&latency_reporting_factory);
    const std::array routes{TimelineTrackGraphRoute{
        .track_id = {10},
        .audio_destination = output_node,
    }};
    const auto admission = binding.prepare(*pinned, routes, config(2), 48'000.0, 64);
    shift = binding.event_compensation_shift_for({10});
    return admission;
}

} // namespace

TEST_CASE("an unanswerable device latency is refused rather than read as zero", "[event-pdc]") {
    SignalGraph graph;
    playback::EventCompensationShift shift;
    const auto admission =
        admit_with({0, PluginSlot::LatencyQuery::Unsupported}, graph, shift);
    REQUIRE_FALSE(admission);
    REQUIRE(admission.code == TimelineGraphAdmissionCode::EventDeviceLatencyUnavailable);
    REQUIRE(admission.item == ItemId{20});
}

TEST_CASE("a device that errored on the latency query is refused", "[event-pdc]") {
    SignalGraph graph;
    playback::EventCompensationShift shift;
    const auto admission =
        admit_with({0, PluginSlot::LatencyQuery::QueryFailed}, graph, shift);
    REQUIRE_FALSE(admission);
    REQUIRE(admission.code == TimelineGraphAdmissionCode::EventDeviceLatencyUnavailable);
}

TEST_CASE("a device latency past the graph's ceiling is refused with both numbers",
          "[event-pdc]") {
    SignalGraph graph;
    playback::EventCompensationShift shift;
    const auto admission = admit_with(
        {kEventDeviceLatencyCeilingSamples + 1, PluginSlot::LatencyQuery::Available}, graph, shift);
    REQUIRE_FALSE(admission);
    REQUIRE(admission.code == TimelineGraphAdmissionCode::EventDeviceLatencyOutOfRange);
    REQUIRE(admission.actual ==
            static_cast<std::uint64_t>(kEventDeviceLatencyCeilingSamples) + 1u);
    REQUIRE(admission.limit == static_cast<std::uint64_t>(kEventDeviceLatencyCeilingSamples));
}

TEST_CASE("a negative device latency is refused", "[event-pdc]") {
    SignalGraph graph;
    playback::EventCompensationShift shift;
    const auto admission = admit_with({-1, PluginSlot::LatencyQuery::Available}, graph, shift);
    REQUIRE_FALSE(admission);
    REQUIRE(admission.code == TimelineGraphAdmissionCode::EventDeviceLatencyOutOfRange);
}

TEST_CASE("an answerable in-range device latency is admitted", "[event-pdc]") {
    // The positive control for every refusal above: the same chain, the same
    // factory seam, a latency the backend can answer, and the binding admits it.
    SignalGraph graph;
    playback::EventCompensationShift shift;
    const auto admission = admit_with(
        {kEventDeviceLatencyCeilingSamples, PluginSlot::LatencyQuery::Available}, graph, shift);
    REQUIRE(admission);
    // An event-to-audio device contributes nothing to the event window: the
    // graph's own delay compensation already aligns its audio output against
    // every sibling branch, so adding it here would compensate twice.
    REQUIRE(shift.samples == 0);
}

TEST_CASE("only an event-to-event slot moves the scheduling window", "[event-pdc]") {
    DevicePlacement placement{{20},
                              DeviceConfiguration{
                                  .position = DeviceChainPosition::PreFader,
                                  .slot_kind = DeviceSlotKind::EventToAudio,
                                  .device_kind = DeviceKind::BuiltIn,
                                  .binding_key = std::string(kBasicInstrumentBindingKey),
                              },
                              std::nullopt};
    REQUIRE(event_domain_latency_samples(placement, 512) == 0);
    placement.configuration.slot_kind = DeviceSlotKind::AudioToAudio;
    REQUIRE(event_domain_latency_samples(placement, 512) == 0);
    placement.configuration.slot_kind = DeviceSlotKind::EventToEvent;
    REQUIRE(event_domain_latency_samples(placement, 512) == 512);
}

TEST_CASE("the scheduling shift uses event-domain latency alone", "[event-pdc]") {
    // The design proposal this slice implements
    // (planning/2026-09-06-event-stream-pdc-contract-proposal.md, section 3)
    // wrote the shift as the event-chain sum PLUS the audio latency from the
    // event-to-audio boundary to the track output. That second term is
    // deliberately NOT implemented: the graph's own delay compensation already
    // delays every sibling branch to match that device's audio output, so
    // adding it here would fire events early by an amount the graph had already
    // accounted for downstream. Double compensation does not error; it produces
    // a plausible-looking wrong offset, which is exactly why it needs a test
    // that can tell the two formulas apart.
    //
    // The two latencies must therefore DIFFER and both be non-zero. With equal
    // values the two formulas would still disagree only by a factor, and with a
    // zero audio latency they would agree outright — either way the test would
    // prove nothing about which rule the code implements.
    constexpr int kEventDomainLatency = 384;
    constexpr int kAudioBoundaryLatency = 512;
    static_assert(kEventDomainLatency != kAudioBoundaryLatency);
    static_assert(kEventDomainLatency != 0 && kAudioBoundaryLatency != 0);

    const DevicePlacement event_device{{20},
                                       DeviceConfiguration{
                                           .position = DeviceChainPosition::PreFader,
                                           .slot_kind = DeviceSlotKind::EventToEvent,
                                           .device_kind = DeviceKind::BuiltIn,
                                           .binding_key = "pulp.test.event-device",
                                       },
                                       std::nullopt};
    const DevicePlacement audio_device{{21},
                                       DeviceConfiguration{
                                           .position = DeviceChainPosition::PreFader,
                                           .slot_kind = DeviceSlotKind::EventToAudio,
                                           .device_kind = DeviceKind::BuiltIn,
                                           .binding_key = std::string(kBasicInstrumentBindingKey),
                                       },
                                       std::nullopt};

    const std::array chain{
        event_domain_latency_samples(event_device, kEventDomainLatency),
        event_domain_latency_samples(audio_device, kAudioBoundaryLatency),
    };
    const auto resolved = accumulate_event_chain_shift(chain, kEventDeviceLatencyCeilingSamples);
    REQUIRE(resolved);
    REQUIRE(resolved.shift.samples == kEventDomainLatency);

    // The two formulas really do disagree on this chain, so the assertion above
    // discriminates rather than happening to hold under both.
    REQUIRE(resolved.shift.samples !=
            static_cast<std::int64_t>(kEventDomainLatency) + kAudioBoundaryLatency);
    REQUIRE(chain[1] == 0);
    // Control: the audio device's own report is not zero, so the exclusion is a
    // decision about its slot kind and not an artefact of a zero input.
    REQUIRE(kAudioBoundaryLatency != 0);
}

TEST_CASE("a compensated chain refuses a track that also offers live input", "[event-pdc]") {
    ProviderSelectorProgram scheduled_only;
    scheduled_only.available_mask = 1u << static_cast<unsigned>(ProviderKind::Arrangement);
    ProviderSelectorProgram with_live = scheduled_only;
    with_live.available_mask |= 1u << static_cast<unsigned>(ProviderKind::ExternalInput);

    const auto compensated = accumulate_event_chain_shift(std::array{384},
                                                          kEventDeviceLatencyCeilingSamples);
    REQUIRE(compensated);
    const auto uncompensated =
        accumulate_event_chain_shift(std::array{0}, kEventDeviceLatencyCeilingSamples);
    REQUIRE(uncompensated);

    // Controls on both axes before the refusal, so the predicate is proven to
    // discriminate rather than to reject everything it sees.
    REQUIRE(admit_event_compensation(uncompensated, with_live, {10}));
    REQUIRE(admit_event_compensation(compensated, scheduled_only, {10}));
    const auto refused = admit_event_compensation(compensated, with_live, {10});
    REQUIRE_FALSE(refused);
    REQUIRE(refused.code == TimelineGraphAdmissionCode::EventChainLiveInputUnsupported);
    REQUIRE(refused.item == ItemId{10});

    const auto out_of_range = accumulate_event_chain_shift(
        std::array{kEventDeviceLatencyCeilingSamples + 1}, kEventDeviceLatencyCeilingSamples);
    REQUIRE_FALSE(out_of_range);
    const auto rejected = admit_event_compensation(out_of_range, scheduled_only, {10});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.code == TimelineGraphAdmissionCode::EventDeviceLatencyOutOfRange);
}

TEST_CASE("device latency discovery reads the query before the number", "[event-pdc]") {
    auto builtin = load_builtin_plugin([] {
        PluginInfo info;
        info.name = "Pulp Basic Instrument";
        info.unique_id = std::string(kBasicInstrumentBindingKey);
        info.format = PluginFormat::BuiltIn;
        info.is_instrument = true;
        info.num_inputs = 0;
        info.num_outputs = 2;
        info.supports_midi_in = true;
        return info;
    }());
    REQUIRE(builtin);

    int latency = -7;
    LatencyReportingSlot answered(std::move(builtin), 96, PluginSlot::LatencyQuery::Available);
    REQUIRE(resolve_event_device_latency(answered, {20}, 3, latency));
    REQUIRE(latency == 96);
}

TEST_CASE("a synthetic device that changes latency relatches only on stop", "[event-pdc]") {
    auto builtin = load_builtin_plugin([] {
        PluginInfo info;
        info.name = "Pulp Basic Instrument";
        info.unique_id = std::string(kBasicInstrumentBindingKey);
        info.format = PluginFormat::BuiltIn;
        info.is_instrument = true;
        info.num_inputs = 0;
        info.num_outputs = 2;
        info.supports_midi_in = true;
        return info;
    }());
    REQUIRE(builtin);
    LatencyReportingSlot device(std::move(builtin), 128, PluginSlot::LatencyQuery::Available);

    // Discovery is the production path: query first, number second, accumulated
    // through the shared rule against the shared ceiling.
    const auto discover = [&] {
        int latency = 0;
        const auto admission = resolve_event_device_latency(device, {20}, 3, latency);
        REQUIRE(admission);
        const std::array chain{latency};
        const auto resolved =
            accumulate_event_chain_shift(chain, kEventDeviceLatencyCeilingSamples);
        REQUIRE(resolved);
        return resolved.shift;
    };
    REQUIRE(discover().samples == 128);

    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map), map, take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    REQUIRE(program);
    PlaybackProgramBlock block(program.get());
    ArrangementNoteRenderer renderer({10});
    REQUIRE(renderer.prepare(256));

    MasterTransport transport;
    MasterTransportConfig transport_config;
    transport_config.max_buffer_size = 256;
    transport_config.initially_playing = true;
    REQUIRE(transport.prepare(*map, transport_config) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(64, snapshot) == TransportError::None);
    auto result = renderer.process(block, snapshot, discover());
    REQUIRE(result.code == playback::NoteRenderCode::Ok);
    REQUIRE(result.applied_shift.samples == 128);
    REQUIRE_FALSE(result.shift_relatch_pending);

    device.report_latency(704);
    REQUIRE(discover().samples == 704);
    for (int index = 0; index < 3; ++index) {
        REQUIRE(transport.begin_block(64, snapshot) == TransportError::None);
        result = renderer.process(block, snapshot, discover());
        REQUIRE(result.code == playback::NoteRenderCode::Ok);
        REQUIRE(result.applied_shift.samples == 128);
        REQUIRE(result.shift_relatch_pending);
    }

    REQUIRE(transport.set_playing(false) == TransportError::None);
    REQUIRE(transport.begin_block(64, snapshot) == TransportError::None);
    REQUIRE_FALSE(snapshot.is_playing);
    result = renderer.process(block, snapshot, discover());
    REQUIRE(result.applied_shift.samples == 704);
    REQUIRE_FALSE(result.shift_relatch_pending);

    // A device whose new report is unanswerable is refused at discovery rather
    // than relatching to a placeholder zero.
    LatencyReportingSlot unanswerable(load_builtin_plugin([] {
        PluginInfo info;
        info.name = "Pulp Basic Instrument";
        info.unique_id = std::string(kBasicInstrumentBindingKey);
        info.format = PluginFormat::BuiltIn;
        info.is_instrument = true;
        info.num_inputs = 0;
        info.num_outputs = 2;
        info.supports_midi_in = true;
        return info;
    }()), 704, PluginSlot::LatencyQuery::QueryFailed);
    int ignored = 0;
    const auto refused = resolve_event_device_latency(unanswerable, {20}, 3, ignored);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.code == TimelineGraphAdmissionCode::EventDeviceLatencyUnavailable);
}
