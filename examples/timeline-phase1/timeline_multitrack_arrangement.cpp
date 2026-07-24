#include "timeline_multitrack_arrangement.hpp"

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/state/param_cursor.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/automation_curve.hpp>
#include <pulp/timeline/automation_lane.hpp>
#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::examples::timeline_phase1 {
namespace {

constexpr timeline::ItemId kSequenceId{3};
constexpr timeline::ItemId kDirectTrackId{10};
constexpr timeline::ItemId kDelayedTrackId{11};
constexpr timeline::ItemId kInstrumentTrackId{12};
constexpr timeline::ItemId kDelayPlacementId{29};
constexpr timeline::ItemId kInstrumentPlacementId{30};
constexpr state::ParamID kInstrumentLevelParam = 1;
constexpr playback::MeterSignature kChangedMeter{3, 4};
constexpr timebase::TickPosition kMeterChangeTick{4 * timebase::kTicksPerQuarter};
constexpr timebase::TickPosition kTempoChangeTick{5120};

template <class T, class E> std::optional<T> value_or_none(runtime::Result<T, E> result) {
    if (!result)
        return std::nullopt;
    return std::move(result).value();
}

class InlineCompileExecutor final : public playback::CompileExecutor {
  public:
    bool submit(std::unique_ptr<playback::CompileTask> task,
                std::chrono::steady_clock::time_point) override {
        if (!task)
            return false;
        constexpr std::size_t kWorkPerSlice = 4096;
        while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                kWorkPerSlice}) == playback::CompileTaskStatus::Pending) {
        }
        return true;
    }
};

class FixedDelaySlot final : public host::PluginSlot {
  public:
    FixedDelaySlot() {
        info_.name = "Timeline fixed-delay device";
        info_.unique_id = "pulp.example.timeline-fixed-delay";
        info_.format = host::PluginFormat::CLAP;
        info_.is_effect = true;
        info_.num_inputs = 2;
        info_.num_outputs = 2;
    }

    const host::PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        delay_.fill({});
        cursor_ = 0;
        return true;
    }
    void release() override {}
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 const midi::MidiBuffer&, midi::MidiBuffer&, const host::ParameterEventQueue&,
                 int frames) override {
        for (int frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
                output.channel_ptr(channel)[frame] = delay_[channel][cursor_];
                delay_[channel][cursor_] = input.channel_ptr(channel)[frame];
            }
            cursor_ = (cursor_ + 1) % TimelineMultitrackArrangementProcessor::pdc_latency_samples;
        }
    }
    std::vector<host::HostParamInfo> parameters() const override {
        return {};
    }
    float get_parameter(std::uint32_t) const override {
        return 0.0f;
    }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override {
        return false;
    }
    std::vector<std::uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        return true;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return TimelineMultitrackArrangementProcessor::pdc_latency_samples;
    }
    int tail_samples() const override {
        return 0;
    }

  private:
    host::PluginInfo info_;
    std::array<std::array<float, TimelineMultitrackArrangementProcessor::pdc_latency_samples>, 2>
        delay_{};
    std::size_t cursor_ = 0;
};

class PulseInstrumentSlot final : public host::PluginSlot {
  public:
    PulseInstrumentSlot() {
        info_.name = "Timeline pulse instrument";
        info_.unique_id = "pulp.example.timeline-pulse-instrument";
        info_.format = host::PluginFormat::CLAP;
        info_.category = "Instrument";
        info_.num_inputs = 0;
        info_.num_outputs = 2;
        level_.id = kInstrumentLevelParam;
        level_.name = "Level";
        level_.default_value = 0.25f;
        store_.add_parameter(
            {.id = kInstrumentLevelParam, .name = "Level", .range = {0.0f, 1.0f, 0.25f, 0.0f}});
    }

    const host::PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        return true;
    }
    void release() override {}
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const host::ParameterEventQueue& parameter_events, int frames) override {
        output.clear();
        automation_event_count_.fetch_add(parameter_events.size(), std::memory_order_relaxed);
        state::ParamCursor parameters(store_, &parameter_events);
        auto event = midi_in.begin();
        const auto end = midi_in.end();
        for (int frame = 0; frame < frames; ++frame) {
            parameters.advance_to(frame);
            bool trigger = false;
            while (event != end && event->sample_offset <= frame) {
                trigger = trigger || (event->is_note_on() && event->velocity() != 0);
                ++event;
            }
            if (!trigger)
                continue;
            const float level = parameters.value(kInstrumentLevelParam);
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
                output.channel_ptr(channel)[frame] = level;
        }
    }
    std::vector<host::HostParamInfo> parameters() const override {
        return {level_};
    }
    float get_parameter(std::uint32_t id) const override {
        return id == kInstrumentLevelParam ? store_.get_value(id) : 0.0f;
    }
    void set_parameter(std::uint32_t id, float value) override {
        if (id == kInstrumentLevelParam)
            store_.set_value(id, value);
    }
    void set_bypass(bool) override {}
    bool is_bypassed() const override {
        return false;
    }
    std::vector<std::uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        return true;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return 0;
    }
    int tail_samples() const override {
        return 0;
    }

    std::size_t automation_event_count() const noexcept {
        return automation_event_count_.load(std::memory_order_relaxed);
    }

  private:
    host::PluginInfo info_;
    host::HostParamInfo level_;
    state::StateStore store_;
    std::atomic<std::size_t> automation_event_count_{0};
};

std::optional<timebase::TempoMap> make_tempo_map() {
    const std::array points{
        timebase::TempoPoint{{0}, 120.0},
        timebase::TempoPoint{kTempoChangeTick, 132.0},
    };
    return value_or_none(timebase::TempoMap::create(points));
}

std::optional<timebase::MeterMap> make_meter_map() {
    const std::array points{
        timebase::MeterPoint{{0}, {4, 4}},
        timebase::MeterPoint{kMeterChangeTick, kChangedMeter},
    };
    return value_or_none(timebase::MeterMap::create(points));
}

std::shared_ptr<const audio::AudioFileData> make_impulse_audio(std::uint32_t frames,
                                                               std::uint32_t sample_rate) {
    auto audio = std::make_shared<audio::AudioFileData>();
    audio->sample_rate = sample_rate;
    audio->channels.assign(2, std::vector<float>(frames, 0.0f));
    for (auto& channel : audio->channels)
        channel.front() = 1.0f;
    return audio;
}

std::shared_ptr<const timeline::Project>
make_arrangement_project(const timebase::CompiledTempoMap& compiled_map, std::uint32_t frames,
                         timebase::RationalRate rate, timebase::TempoMap tempo_map,
                         timebase::MeterMap meter_map) {
    const auto make_audio_clip = [frames, rate](timeline::ItemId id) {
        return value_or_none(timeline::Clip::create_absolute(id, {0}, frames, rate,
                                                             timeline::MediaRef{{2}, {0}, frames}));
    };
    auto direct_clip = make_audio_clip({100});
    auto delayed_clip = make_audio_clip({101});
    if (!direct_clip || !delayed_clip)
        return {};
    auto direct = value_or_none(
        timeline::Track::create(kDirectTrackId, "Direct audio", {std::move(*direct_clip)}));
    timeline::TrackInput delayed_input;
    delayed_input.id = kDelayedTrackId;
    delayed_input.name = "Delayed audio";
    delayed_input.clips = {std::move(*delayed_clip)};
    delayed_input.device_chain = {{kDelayPlacementId}};
    auto delayed = value_or_none(timeline::Track::create(std::move(delayed_input)));
    if (!direct || !delayed)
        return {};

    const auto note_start = compiled_map.samples_to_ticks({32});
    const auto note_end = compiled_map.samples_to_ticks({40});
    auto notes = value_or_none(
        timeline::NoteContent::create({{{103}, note_start, note_end - note_start, 0xffff, 60, 0}}));
    auto note_clip =
        notes ? value_or_none(timeline::Clip::create(
                    {102}, {0}, compiled_map.samples_to_ticks({frames}) - timebase::TickPosition{0},
                    std::move(*notes)))
              : std::nullopt;
    auto curve = value_or_none(timeline::AutomationCurve::create({
        {{32}, {0}, 0.25f},
        {{33}, compiled_map.samples_to_ticks({64}), 0.5f},
    }));
    auto lane =
        curve ? value_or_none(timeline::AutomationLane::create(
                    {31},
                    timeline::DeviceParameterTarget{kInstrumentPlacementId, kInstrumentLevelParam},
                    std::move(*curve)))
              : std::nullopt;
    if (!note_clip || !lane)
        return {};
    timeline::TrackInput instrument_input;
    instrument_input.id = kInstrumentTrackId;
    instrument_input.name = "Instrument";
    instrument_input.clips = {std::move(*note_clip)};
    instrument_input.device_chain = {{kInstrumentPlacementId}};
    instrument_input.automation_lanes = {std::move(*lane)};
    auto instrument = value_or_none(timeline::Track::create(std::move(instrument_input)));
    if (!instrument)
        return {};

    auto sequence = value_or_none(timeline::Sequence::create(
        kSequenceId, "Multitrack arrangement", std::nullopt,
        timeline::AbsoluteTimelineDuration{frames, rate},
        std::vector<timeline::Track>{std::move(*direct), std::move(*delayed),
                                     std::move(*instrument)}));
    if (!sequence)
        return {};

    const auto content_hash = timeline::ContentHash::from_hex(std::string(64, 'a'));
    if (!content_hash)
        return {};
    timeline::ProjectInput input;
    input.id = {1};
    input.name = "Timeline multitrack arrangement";
    input.next_item_id = 104;
    input.root_sequence_id = kSequenceId;
    input.assets = {{.id = {2},
                     .name = "Stereo impulse",
                     .frame_count = frames,
                     .sample_rate = rate,
                     .content_hash = *content_hash}};
    input.sequences = {std::move(*sequence)};
    input.tempo_map = std::move(tempo_map);
    input.meter_map = std::move(meter_map);
    auto project = value_or_none(timeline::Project::create(std::move(input)));
    return project ? std::make_shared<const timeline::Project>(std::move(*project)) : nullptr;
}

} // namespace

struct TimelineMultitrackArrangementProcessor::Impl {
    playback::PlaybackProgramStore store;
    InlineCompileExecutor executor;
    playback::PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};
    host::SignalGraph graph;
    host::TimelineGraphPlaybackBinding binding{graph, store};
    playback::MasterTransport transport;
    playback::TransportSnapshot last_transport;
    PulseInstrumentSlot* instrument = nullptr;
    std::uint32_t maximum_block_size = 0;
    bool ready = false;

    bool prepare(double sample_rate, std::uint32_t max_block_size) {
        const auto integer_rate = static_cast<std::uint64_t>(std::llround(sample_rate));
        if (integer_rate == 0 || max_block_size == 0)
            return false;
        const timebase::RationalRate rate{integer_rate, 1};
        auto tempo_map = make_tempo_map();
        if (!tempo_map || integer_rate > std::numeric_limits<std::uint32_t>::max())
            return false;
        auto compiled = timebase::CompiledTempoMap::compile(*tempo_map, rate);
        if (!compiled)
            return false;
        auto meter_map = make_meter_map();
        if (!meter_map)
            return false;
        const auto tempo_change_sample = compiled.value().ticks_to_samples(kTempoChangeTick).value;
        if (tempo_change_sample < 0 ||
            static_cast<std::uint64_t>(tempo_change_sample) >
                std::numeric_limits<std::uint32_t>::max() - max_block_size)
            return false;
        const auto arrangement_frames =
            static_cast<std::uint32_t>(tempo_change_sample) + max_block_size;
        auto map = std::make_shared<const timebase::CompiledTempoMap>(std::move(compiled).value());
        auto project = make_arrangement_project(*map, arrangement_frames, rate,
                                                std::move(*tempo_map), std::move(*meter_map));
        auto pool = playback::DecodedAudioAssetPool::create({{
            {2},
            make_impulse_audio(arrangement_frames, static_cast<std::uint32_t>(integer_rate)),
        }});
        if (!project || !pool)
            return false;

        playback::ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = kSequenceId;
        request.tempo_map = map;
        request.document_revision = 1;
        request.audio_assets = std::move(pool).value();
        request.audio_limits.max_tracks = 3;
        request.audio_limits.max_channels = 2;
        request.audio_limits.max_block_frames = max_block_size;
        request.dirty.all = true;
        if (!compiler.submit(std::move(request)) || compiler.status().has_error)
            return false;
        auto program = store.read();
        if (!program || program->tracks().size() != 3)
            return false;

        graph.set_parallel_routing_enabled(false);
        const auto output = graph.add_output_node(2, "Arrangement output");
        const auto delayed =
            graph.add_plugin_node(std::make_unique<FixedDelaySlot>(), 2, 2, "Fixed-delay device");
        auto pulse = std::make_unique<PulseInstrumentSlot>();
        instrument = pulse.get();
        const auto synth = graph.add_plugin_node(std::move(pulse), 0, 2, "Pulse instrument");
        if (output == 0 || delayed == 0 || synth == 0)
            return false;
        for (host::PortIndex channel = 0; channel < 2; ++channel) {
            if (!graph.connect(delayed, channel, output, channel) ||
                !graph.connect(synth, channel, output, channel))
                return false;
        }
        if (!graph.prepare(sample_rate, static_cast<int>(max_block_size)) ||
            graph.latency_samples() != pdc_latency_samples)
            return false;

        const std::array instrument_devices{
            host::TimelineDeviceGraphRoute{kInstrumentPlacementId, synth}};
        const std::array delayed_devices{
            host::TimelineDeviceGraphRoute{kDelayPlacementId, delayed}};
        const std::array routes{
            host::TimelineTrackGraphRoute{kDirectTrackId, output, 0, 0},
            host::TimelineTrackGraphRoute{kDelayedTrackId, delayed, 0, 0, delayed_devices},
            host::TimelineTrackGraphRoute{kInstrumentTrackId, output, 0, synth, instrument_devices},
        };
        host::TimelineGraphBindingConfig config;
        config.audio_channels = 2;
        config.maximum_note_events_per_track_per_block = 64;
        config.audio_limits.max_tracks = 3;
        config.audio_limits.max_channels = 2;
        config.audio_limits.max_block_frames = max_block_size;
        if (!binding.prepare(*program, routes, config, sample_rate,
                             static_cast<int>(max_block_size)))
            return false;

        playback::MasterTransportConfig transport_config;
        transport_config.max_buffer_size = max_block_size;
        transport_config.initially_playing = true;
        if (transport.prepare(program->tempo_map(), transport_config) !=
            playback::TransportError::None)
            return false;
        last_transport.tempo_map = &program->tempo_map();
        last_transport.sample_rate = program->tempo_map().sample_rate();
        maximum_block_size = max_block_size;
        ready = true;
        return true;
    }

    bool apply_arrangement_meter_change() noexcept {
        if (!ready || last_transport.range_count == 0 ||
            last_transport.ranges[last_transport.range_count - 1].timeline_tick_end <
                kMeterChangeTick)
            return false;
        return transport.set_meter(kChangedMeter) == playback::TransportError::None;
    }
};

TimelineMultitrackArrangementProcessor::TimelineMultitrackArrangementProcessor() = default;
TimelineMultitrackArrangementProcessor::~TimelineMultitrackArrangementProcessor() = default;

format::PluginDescriptor TimelineMultitrackArrangementProcessor::descriptor() const {
    return {.name = "Timeline Multitrack Arrangement",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.timeline-multitrack-arrangement",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = 0};
}

void TimelineMultitrackArrangementProcessor::prepare(const format::PrepareContext& context) {
    auto replacement = std::make_unique<Impl>();
    if (context.sample_rate > 0.0 && context.max_buffer_size > 0 &&
        replacement->prepare(context.sample_rate,
                             static_cast<std::uint32_t>(context.max_buffer_size)))
        impl_ = std::move(replacement);
}

int TimelineMultitrackArrangementProcessor::latency_samples() const {
    return graph_latency_samples();
}

bool TimelineMultitrackArrangementProcessor::apply_arrangement_meter_change() noexcept {
    return impl_ && impl_->apply_arrangement_meter_change();
}

void TimelineMultitrackArrangementProcessor::process(audio::BufferView<float>& output,
                                                     const audio::BufferView<const float>& input,
                                                     midi::MidiBuffer&, midi::MidiBuffer&,
                                                     const format::ProcessContext&) {
    runtime::ScopedNoAlloc no_alloc;
    if (!impl_ || !impl_->ready || output.num_samples() > impl_->maximum_block_size ||
        impl_->transport.begin_block(static_cast<std::uint32_t>(output.num_samples()),
                                     impl_->last_transport) != playback::TransportError::None) {
        output.clear();
        return;
    }
    if (!impl_->binding.process(output, input, impl_->last_transport))
        output.clear();
}

bool TimelineMultitrackArrangementProcessor::engine_prepared() const noexcept {
    return impl_ && impl_->ready;
}

int TimelineMultitrackArrangementProcessor::graph_latency_samples() const noexcept {
    return impl_ ? impl_->graph.latency_samples() : 0;
}

std::size_t TimelineMultitrackArrangementProcessor::automation_event_count() const noexcept {
    return impl_ && impl_->instrument ? impl_->instrument->automation_event_count() : 0;
}

const playback::TransportSnapshot&
TimelineMultitrackArrangementProcessor::last_transport() const noexcept {
    static const playback::TransportSnapshot empty;
    return impl_ ? impl_->last_transport : empty;
}

std::unique_ptr<format::Processor> create_timeline_multitrack_arrangement() {
    return std::make_unique<TimelineMultitrackArrangementProcessor>();
}

} // namespace pulp::examples::timeline_phase1
