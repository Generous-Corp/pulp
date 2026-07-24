#include "harness/scoped_rt_process_probe.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/audio/device.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/playback/capture_engine.hpp>
#include <pulp/playback/recording_commit.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

class StandaloneRecordingProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Standalone Recording",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.standalone-recording",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Input", 1}},
            .output_buses = {{"Monitor", 1}},
        };
    }

    void define_parameters(state::StateStore&) override {}

    void prepare(const format::PrepareContext& context) override {
        const std::array points{TempoPoint{{0}, 120.0}};
        auto map =
            CompiledTempoMap::compile(points, {static_cast<std::uint64_t>(context.sample_rate), 1});
        REQUIRE(map);
        tempo_map_ = std::make_unique<const CompiledTempoMap>(std::move(map).value());
        CaptureEngineConfig config;
        config.sample_rate = tempo_map_->sample_rate();
        config.maximum_block_size = static_cast<std::uint32_t>(context.max_buffer_size);
        config.maximum_take_frames = 128;
        config.take_slots_per_track = 2;
        config.midi_events_per_take = 8;
        config.tracks.push_back({
            .track_id = {10},
            .take_lane_id = {101},
            .input_channel = 0,
            .output_channel = 0,
            .channel_count = 1,
            .armed = true,
            .monitor = true,
            .capture_midi = true,
        });
        REQUIRE(capture_.prepare(std::move(config)));
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext& context) override {
        output.clear();
        if (context.is_recording != was_recording_) {
            CaptureCommand command;
            command.type =
                context.is_recording ? CaptureCommandType::Start : CaptureCommandType::Stop;
            command.sequence = 1;
            command.session.count_in_start = {context.position_samples};
            command.session.punch_in = {context.position_samples};
            capture_.enqueue_command(command);
            was_recording_ = context.is_recording;
        }

        TransportSnapshot transport;
        transport.tempo_map = tempo_map_.get();
        transport.sample_rate = tempo_map_->sample_rate();
        transport.frame_count = static_cast<std::uint32_t>(context.num_samples);
        transport.is_playing = context.is_playing;
        transport.range_count = 1;
        transport.ranges[0].frame_count = transport.frame_count;
        transport.ranges[0].timeline_sample_start = {context.position_samples};
        transport.ranges[0].timeline_tick_start =
            tempo_map_->samples_to_ticks({context.position_samples});
        transport.ranges[0].timeline_tick_end =
            tempo_map_->samples_to_ticks({context.position_samples + context.num_samples});
        capture_.process(input, output, midi_in, transport);
    }

    bool pop_event(CaptureEvent& event) noexcept {
        return capture_.pop_event(event);
    }

    bool copy_audio(CaptureTakeHandle take, audio::BufferView<float> destination) const noexcept {
        return capture_.copy_audio(take, destination);
    }

  private:
    std::unique_ptr<const CompiledTempoMap> tempo_map_;
    CaptureEngine capture_;
    bool was_recording_ = false;
};

StandaloneRecordingProcessor* processor = nullptr;

std::unique_ptr<format::Processor> create_processor() {
    auto result = std::make_unique<StandaloneRecordingProcessor>();
    processor = result.get();
    return result;
}

Project empty_recording_project() {
    auto track = Track::create(TrackInput{.id = {10}, .name = "record"});
    REQUIRE(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1}, "recording", 100, {3}, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

} // namespace

namespace pulp::format {
struct StandaloneRenderTestAccess {
    static void ensure_processor(StandaloneApp& app) {
        if (!app.processor_) {
            app.processor_ = app.factory_();
            app.processor_->set_state_store(&app.store_);
            app.processor_->define_parameters(app.store_);
        }
    }
    static void prepare(StandaloneApp& app) {
        app.prepare_render_state();
    }
    static void render(StandaloneApp& app, const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output, const audio::CallbackContext& context) {
        app.render_audio_block(input, output, context);
    }
};
} // namespace pulp::format

TEST_CASE("standalone host records input into a replayable take comp") {
    constexpr std::size_t frames = 16;
    format::StandaloneConfig config;
    config.sample_rate = 48'000.0;
    config.buffer_size = frames;
    config.input_channels = 1;
    config.output_channels = 1;
    config.persist_settings = false;
    config.transport_playing = true;
    config.transport_recording = false;

    format::StandaloneApp app(create_processor);
    app.set_config(config);
    format::StandaloneRenderTestAccess::ensure_processor(app);
    format::StandaloneRenderTestAccess::prepare(app);
    REQUIRE(processor != nullptr);

    std::array<float, frames> input_storage{};
    std::array<float, frames> output_storage{};
    const float* input_channels[]{input_storage.data()};
    float* output_channels[]{output_storage.data()};
    audio::BufferView<const float> input(input_channels, 1, frames);
    audio::BufferView<float> output(output_channels, 1, frames);
    audio::CallbackContext callback;
    callback.sample_rate = 48'000.0;
    callback.buffer_size = frames;

    format::StandaloneRenderTestAccess::render(app, input, output, callback);
    format::StandaloneRenderTestAccess::render(app, input, output, callback);

    config.transport_recording = true;
    app.set_config(config);
    std::fill(input_storage.begin(), input_storage.end(), 1.0f);
    format::StandaloneRenderTestAccess::render(app, input, output, callback);
    std::fill(input_storage.begin(), input_storage.end(), 2.0f);
    std::size_t allocations = 0;
    {
        test::ScopedRtProcessProbe probe;
        format::StandaloneRenderTestAccess::render(app, input, output, callback);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);

    config.transport_recording = false;
    app.set_config(config);
    format::StandaloneRenderTestAccess::render(app, input, output, callback);

    CaptureEvent event;
    CaptureEvent completed;
    bool found_completed = false;
    while (processor->pop_event(event)) {
        if (event.type == CaptureEventType::TakeCompleted) {
            completed = event;
            found_completed = true;
        }
    }
    REQUIRE(found_completed);
    REQUIRE(completed.placement_start == SamplePosition{32});
    REQUIRE(completed.frame_count == 32);

    audio::Buffer<float> captured(1, completed.frame_count);
    REQUIRE(processor->copy_audio(completed.take, captured.view()));
    REQUIRE(std::all_of(captured.channel(0).begin(), captured.channel(0).begin() + frames,
                        [](float sample) { return sample == 1.0f; }));
    REQUIRE(std::all_of(captured.channel(0).begin() + frames, captured.channel(0).end(),
                        [](float sample) { return sample == 2.0f; }));

    RecordingTakeCommitRequest request;
    request.sequence_id = {3};
    request.track_id = {10};
    request.take_lane_id = {101};
    request.asset_id = {100};
    request.take_id = {102};
    request.placement_start = completed.placement_start;
    request.sample_rate = {48'000, 1};
    request.asset_name = "standalone-capture.wav";
    request.create_take_lane = true;
    request.take_lane_name = "standalone recording";
    auto sealed = seal_recording_take(static_cast<const audio::Buffer<float>&>(captured).view(),
                                      std::move(request));
    REQUIRE(sealed);
    sealed->commands.emplace_back(SetTakeComp{
        {3},
        {10},
        {101},
        {},
        {{.take_id = {102},
          .range = {completed.placement_start, completed.frame_count, {48'000, 1}}}},
    });
    sealed->commands.emplace_back(SetActiveTakeLane{{3}, {10}, {}, {101}});

    const auto initial = empty_recording_project();
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    REQUIRE(
        session->submit(writer, timeline_test::session_transaction(writer, {}, sealed->commands)));
    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    const auto* lane = replayed->find_sequence({3})->find_track({10})->find_take_lane({101});
    REQUIRE(lane != nullptr);
    REQUIRE(lane->comp_segments().size() == 1);
    REQUIRE(lane->comp_segments()[0].take_id == ItemId{102});
    REQUIRE(replayed->find_sequence({3})->find_track({10})->active_take_lane_id() == ItemId{101});
}
