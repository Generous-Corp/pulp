#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/device.hpp>

#include "../core/audio/platform/win/wasapi_io_timing.hpp"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

#if defined(__APPLE__)
#include "../core/audio/platform/mac/coreaudio_device.hpp"
#include <CoreAudio/CoreAudio.h>
#endif

using namespace pulp::audio;

namespace {

class UnsupportedTimingDevice final : public AudioDevice {
public:
    bool open(const DeviceConfig&) override { return true; }
    void close() override {}
    bool start(AudioCallback) override { return true; }
    void stop() override {}
    bool is_open() const override { return true; }
    bool is_running() const override { return false; }
    DeviceInfo info() const override { return {}; }
    double sample_rate() const override { return 48000.0; }
    int buffer_size() const override { return 128; }
};

} // namespace

TEST_CASE("unsupported audio devices return no timing without a virtual slot",
          "[audio][timing]") {
    UnsupportedTimingDevice device;
    CHECK_FALSE(query_audio_io_timing(device).has_value());
}

TEST_CASE("audio io timing composes with graph latency into one snapshot",
          "[audio][timing]") {
    AudioIoTiming timing{};
    timing.input_latency_frames = 31;
    timing.output_latency_frames = 47;
    timing.input_safety_offset_frames = 5;
    timing.output_safety_offset_frames = 7;
    timing.io_buffer_frames = 128;
    timing.sample_rate_hz = 48000.0;
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::device_clock;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = 17;
    timing.calibration_generation = 9;

    constexpr std::uint64_t graph_total_from_pdc = 113;
    const auto snapshot =
        make_latency_snapshot(timing, graph_total_from_pdc, 48000.0);

    REQUIRE(snapshot.has_value());
    CHECK(snapshot->graph_latency_frames == graph_total_from_pdc);
    CHECK(snapshot->sample_rate_hz == 48000.0);
    CHECK(snapshot->input_placement_offset_frames == 164);
    CHECK(snapshot->output_scheduling_offset_frames == 295);
    CHECK(snapshot->monitoring_latency_frames == 459);
    CHECK(snapshot->audio_io.timestamp_source == AudioTimingSource::device_clock);
}

TEST_CASE("calibration generation bump invalidates stale latency snapshot",
          "[audio][timing]") {
    AudioIoTiming timing{};
    timing.input_latency_frames = 0;
    timing.output_latency_frames = 0;
    timing.input_safety_offset_frames = 0;
    timing.output_safety_offset_frames = 0;
    timing.io_buffer_frames = 64;
    timing.sample_rate_hz = 48000.0;
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::device_clock;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = 5;
    timing.calibration_generation = 41;
    const auto snapshot = make_latency_snapshot(timing, 64, 48000.0);

    REQUIRE(snapshot.has_value());
    CHECK(snapshot->is_current_for(timing));
    ++timing.calibration_generation;
    CHECK_FALSE(snapshot->is_current_for(timing));

    timing.calibration_generation = 41;
    ++timing.route_instance_token;
    CHECK_FALSE(snapshot->is_current_for(timing));

    timing.calibration_generation = 0;
    const auto unavailable = make_latency_snapshot(timing, 64, 48000.0);
    CHECK_FALSE(unavailable.has_value());
}

TEST_CASE("latency composition refuses missing fields or a different frame rate",
          "[audio][timing]") {
    AudioIoTiming timing{};
    timing.input_latency_frames = 1;
    timing.output_latency_frames = 2;
    timing.input_safety_offset_frames = 3;
    timing.io_buffer_frames = 8;
    timing.sample_rate_hz = 48000.0;
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::device_clock;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = 1;
    timing.calibration_generation = 1;

    const auto input_only = make_latency_snapshot(timing, 4, 48000.0);
    REQUIRE(input_only.has_value());
    CHECK(input_only->input_placement_offset_frames == 12);
    CHECK_FALSE(input_only->output_scheduling_offset_frames.has_value());
    CHECK_FALSE(input_only->monitoring_latency_frames.has_value());
    timing.output_safety_offset_frames = 5;
    CHECK_FALSE(make_latency_snapshot(timing, 4, 44100.0).has_value());

    timing.input_latency_frames.reset();
    timing.input_safety_offset_frames.reset();
    const auto output_only = make_latency_snapshot(timing, 4, 48000.0);
    REQUIRE(output_only.has_value());
    CHECK(output_only->output_scheduling_offset_frames == 19);
    CHECK_FALSE(output_only->input_placement_offset_frames.has_value());
    CHECK_FALSE(output_only->monitoring_latency_frames.has_value());
}

TEST_CASE("latency composition saturates adversarial graph totals",
          "[audio][timing]") {
    AudioIoTiming timing{};
    timing.input_latency_frames = 1;
    timing.output_latency_frames = 2;
    timing.input_safety_offset_frames = 3;
    timing.output_safety_offset_frames = 5;
    timing.io_buffer_frames = 8;
    timing.sample_rate_hz = 48000.0;
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::device_clock;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = 1;
    timing.calibration_generation = 1;

    const auto snapshot = make_latency_snapshot(
        timing, std::numeric_limits<std::uint64_t>::max(), 48000.0);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->output_scheduling_offset_frames ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(snapshot->monitoring_latency_frames ==
          std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("audio io timing rejects invalid identity rate and exhausted generation",
          "[audio][timing]") {
    AudioIoTiming timing{};
    timing.output_latency_frames = 2;
    timing.output_safety_offset_frames = 5;
    timing.io_buffer_frames = 8;
    timing.sample_rate_hz = std::numeric_limits<double>::infinity();
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::device_clock;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = 1;
    timing.calibration_generation = 1;
    CHECK_FALSE(make_latency_snapshot(timing, 4, timing.sample_rate_hz));

    timing.sample_rate_hz = 48000.0;
    timing.route_instance_token = 0;
    CHECK_FALSE(make_latency_snapshot(timing, 4, 48000.0));
    timing.route_instance_token = 1;
    CHECK_FALSE(next_calibration_generation(
        std::numeric_limits<std::uint64_t>::max()));
}

TEST_CASE("latency composition rejects unspecified timing authority") {
    AudioIoTiming timing{};
    timing.output_latency_frames = 2;
    timing.output_safety_offset_frames = 5;
    timing.io_buffer_frames = 8;
    timing.sample_rate_hz = 48000.0;
    timing.route_instance_token = 1;
    timing.calibration_generation = 1;

    CHECK_FALSE(make_latency_snapshot(timing, 4, 48000.0));
    timing.confidence = AudioTimingConfidence::reported;
    CHECK_FALSE(make_latency_snapshot(timing, 4, 48000.0));
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    CHECK_FALSE(make_latency_snapshot(timing, 4, 48000.0));
    timing.timestamp_source = AudioTimingSource::device_clock;
    CHECK(make_latency_snapshot(timing, 4, 48000.0).has_value());
}

TEST_CASE("WASAPI timing reconstructs the reported render maximum exactly",
          "[audio][timing][wasapi]") {
    win::WasapiTimingValues values{};
    values.stream_latency_100ns = 100'000;
    values.endpoint_buffer_frames = 128;
    values.sample_rate_hz = 48'000;
    values.direction = win::WasapiTimingDirection::render;

    const auto timing = win::make_wasapi_audio_io_timing(values, 17, 1);
    REQUIRE(timing.has_value());
    CHECK(timing->output_latency_frames == 352);
    CHECK(timing->output_safety_offset_frames == 0);
    CHECK_FALSE(timing->input_latency_frames.has_value());
    CHECK(timing->timestamp_source == AudioTimingSource::os_estimate);

    const auto snapshot = make_latency_snapshot(*timing, 13, 48'000.0);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->output_scheduling_offset_frames == 493);
}

TEST_CASE("WASAPI timing publishes only the represented capture direction",
          "[audio][timing][wasapi]") {
    win::WasapiTimingValues values{};
    values.stream_latency_100ns = 100'000;
    values.endpoint_buffer_frames = 128;
    values.sample_rate_hz = 48'000;
    values.direction = win::WasapiTimingDirection::capture;

    const auto timing = win::make_wasapi_audio_io_timing(values, 19, 1);
    REQUIRE(timing.has_value());
    CHECK(timing->input_latency_frames == 352);
    CHECK(timing->input_safety_offset_frames == 0);
    CHECK_FALSE(timing->output_latency_frames.has_value());

    const auto snapshot = make_latency_snapshot(*timing, 13, 48'000.0);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->input_placement_offset_frames == 480);
    CHECK_FALSE(snapshot->output_scheduling_offset_frames.has_value());
}

TEST_CASE("WASAPI timing preserves zero residual and rounds maximums upward",
          "[audio][timing][wasapi]") {
    win::WasapiTimingValues values{};
    values.stream_latency_100ns = 100'000;
    values.endpoint_buffer_frames = 480;
    values.sample_rate_hz = 48'000;

    const auto exact = win::make_wasapi_audio_io_timing(values, 23, 1);
    REQUIRE(exact.has_value());
    REQUIRE(exact->output_latency_frames.has_value());
    CHECK(*exact->output_latency_frames == 0);

    CHECK(win::wasapi_duration_to_frames(1, 44'100) == 1);
    CHECK(win::wasapi_duration_to_frames(100'000, 48'000) == 480);
}

TEST_CASE("WASAPI timing fails closed on incomplete or inconsistent reports",
          "[audio][timing][wasapi]") {
    win::WasapiTimingValues values{};
    values.endpoint_buffer_frames = 128;
    values.sample_rate_hz = 48'000;
    CHECK_FALSE(win::make_wasapi_audio_io_timing(values, 29, 1));

    values.stream_latency_100ns = 10'000;
    CHECK_FALSE(win::make_wasapi_audio_io_timing(values, 29, 1));

    values.stream_latency_100ns = std::numeric_limits<std::uint64_t>::max();
    CHECK_FALSE(win::make_wasapi_audio_io_timing(values, 29, 1));

    values.stream_latency_100ns = 100'000;
    values.sample_rate_hz = 0;
    CHECK_FALSE(win::make_wasapi_audio_io_timing(values, 29, 1));
    values.sample_rate_hz = 48'000;
    CHECK_FALSE(win::make_wasapi_audio_io_timing(values, 0, 1));
    CHECK_FALSE(win::make_wasapi_audio_io_timing(values, 29, 0));
}

TEST_CASE("WASAPI timing publication fails closed after route invalidation",
          "[audio][timing][wasapi]") {
    AudioIoTiming timing{};
    timing.output_latency_frames = 352;
    timing.route_instance_token = 31;
    timing.calibration_generation = 1;

    win::WasapiTimingPublication publication;
    publication.publish(timing);
    REQUIRE(publication.read().has_value());
    CHECK(publication.read()->route_instance_token == 31);

    publication.invalidate();
    CHECK_FALSE(publication.read().has_value());
}

TEST_CASE("WASAPI route invalidation notifies once per opened route",
          "[audio][timing][wasapi]") {
    win::WasapiRouteInvalidationGate gate;
    CHECK_FALSE(gate.take_notification());
    gate.mark_pending();
    CHECK(gate.take_notification());
    CHECK_FALSE(gate.take_notification());
    gate.mark_pending();
    CHECK_FALSE(gate.take_notification());

    gate.reset();
    gate.mark_pending();
    CHECK(gate.take_notification());
    CHECK_FALSE(gate.take_notification());
}

#if defined(_WIN32)
TEST_CASE("WASAPI classifies every terminal route result",
          "[audio][timing][wasapi]") {
    CHECK(win::wasapi_result_invalidates_route(
        AUDCLNT_E_DEVICE_INVALIDATED));
    CHECK(win::wasapi_result_invalidates_route(
        AUDCLNT_E_RESOURCES_INVALIDATED));
    CHECK(win::wasapi_result_invalidates_route(
        AUDCLNT_E_SERVICE_NOT_RUNNING));
    CHECK_FALSE(win::wasapi_result_invalidates_route(E_FAIL));
}

TEST_CASE("WASAPI reports initialized default-output timing", "[audio][timing][wasapi]") {
    auto system = create_audio_system();
    REQUIRE(system);
    const auto output = system->default_output_device();
    if (output.id.empty())
        SKIP("WASAPI has no default output device");

    auto device = system->create_device(output.id);
    REQUIRE(device);
    DeviceConfig config{};
    config.device_id = output.id;
    config.output_channels = 2;
    REQUIRE(device->open(config));
    const auto timing = query_audio_io_timing(*device);
    device->close();

    if (!timing)
        SKIP("WASAPI endpoint did not publish representable stream timing");
    CHECK(timing->output_latency_frames.has_value());
    CHECK(timing->output_safety_offset_frames == 0);
    CHECK(timing->io_buffer_frames.has_value());
    CHECK(timing->route_instance_token != 0);
}
#endif

#if defined(__APPLE__)
TEST_CASE("CoreAudio timing assembly preserves exact property presence",
          "[audio][timing][coreaudio]") {
    pulp::audio::mac::CoreAudioTimingPropertyValues values{};
    values.output_latency_frames = 0;
    values.output_safety_offset_frames = 13;
    values.io_buffer_frames = 256;
    values.sample_rate_hz = 96000.0;

    const auto timing = pulp::audio::mac::make_coreaudio_audio_io_timing(
        values, 23, 29);
    REQUIRE(timing.has_value());
    CHECK_FALSE(timing->input_latency_frames.has_value());
    CHECK_FALSE(timing->input_safety_offset_frames.has_value());
    CHECK(timing->output_latency_frames == 0);
    CHECK(timing->output_safety_offset_frames == 13);
    CHECK(timing->io_buffer_frames == 256);
    CHECK(timing->sample_rate_hz == 96000.0);
    CHECK(timing->timestamp_domain ==
          AudioTimestampDomain::device_sample_frames);
    CHECK(timing->timestamp_source == AudioTimingSource::device_clock);
    CHECK(timing->confidence == AudioTimingConfidence::reported);
    CHECK(timing->route_instance_token == 23);
    CHECK(timing->calibration_generation == 29);

    std::array<bool, 6> output_only_listeners{
        false, true, false, true, true, true};
    CHECK(pulp::audio::mac::coreaudio_timing_listener_coverage_complete(
        output_only_listeners, *timing));
    output_only_listeners[3] = false;
    CHECK_FALSE(
        pulp::audio::mac::coreaudio_timing_listener_coverage_complete(
            output_only_listeners, *timing));

    values.io_buffer_frames.reset();
    CHECK_FALSE(pulp::audio::mac::make_coreaudio_audio_io_timing(
        values, 23, 29));
    values.io_buffer_frames = 256;
    values.output_latency_frames.reset();
    values.output_safety_offset_frames.reset();
    CHECK_FALSE(pulp::audio::mac::make_coreaudio_audio_io_timing(
        values, 23, 29));
}

TEST_CASE("CoreAudio timing invalidation advances or fails closed at exhaustion",
          "[audio][timing][coreaudio]") {
    std::atomic<std::uint64_t> generation{0};
    CHECK(pulp::audio::mac::advance_coreaudio_timing_generation(generation));
    CHECK(generation.load() == 1);
    CHECK(pulp::audio::mac::advance_coreaudio_timing_generation(generation));
    CHECK(generation.load() == 2);

    generation.store(std::numeric_limits<std::uint64_t>::max());
    CHECK_FALSE(
        pulp::audio::mac::advance_coreaudio_timing_generation(generation));
    CHECK(generation.load() == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("CoreAudio initial listener coverage publishes the first report",
          "[audio][timing][coreaudio]") {
    std::atomic<std::uint64_t> generation{1};
    std::atomic<bool> exhausted{false};
    std::atomic<bool> dirty{true};
    const std::array<bool, 6> output_only_listeners{
        false, true, false, true, true, true};

    const auto published =
        pulp::audio::mac::refresh_coreaudio_timing_with_listener_coverage(
            generation, exhausted, dirty, output_only_listeners,
            [](std::uint64_t observed) {
                pulp::audio::mac::CoreAudioTimingPropertyValues values{};
                values.output_latency_frames = 5;
                values.output_safety_offset_frames = 7;
                values.io_buffer_frames = 128;
                values.sample_rate_hz = 48000.0;
                return pulp::audio::mac::make_coreaudio_audio_io_timing(
                    values, 11, observed);
            });

    REQUIRE(published.has_value());
    CHECK(published->output_latency_frames == 5);
    CHECK(published->output_safety_offset_frames == 7);
    CHECK(published->io_buffer_frames == 128);
    CHECK(published->calibration_generation == 1);
    CHECK_FALSE(dirty.load());
}

TEST_CASE("CoreAudio timing refresh is bounded under property churn and retries",
          "[audio][timing][coreaudio]") {
    std::atomic<std::uint64_t> generation{1};
    std::atomic<bool> exhausted{false};
    std::atomic<bool> dirty{true};
    unsigned attempts = 0;

    const auto churned = pulp::audio::mac::refresh_coreaudio_timing_bounded(
        generation, exhausted, dirty, [&](std::uint64_t observed) {
            ++attempts;
            CHECK(observed == attempts);
            REQUIRE(pulp::audio::mac::advance_coreaudio_timing_generation(
                generation));
            dirty.store(true);
            AudioIoTiming timing{};
            timing.calibration_generation = observed;
            return std::optional<AudioIoTiming>{timing};
        });

    CHECK_FALSE(churned.has_value());
    CHECK(attempts == 3);
    CHECK(dirty.load());

    const auto stable = pulp::audio::mac::refresh_coreaudio_timing_bounded(
        generation, exhausted, dirty, [&](std::uint64_t observed) {
            ++attempts;
            AudioIoTiming timing{};
            timing.calibration_generation = observed;
            return std::optional<AudioIoTiming>{timing};
        });
    REQUIRE(stable.has_value());
    CHECK(stable->calibration_generation == generation.load());
    CHECK(attempts == 4);
    CHECK_FALSE(dirty.load());
}

TEST_CASE("CoreAudio reports populated monotonic audio io timing",
          "[audio][timing][coreaudio]") {
    const auto device_id =
        pulp::audio::mac::CoreAudioSystem::get_default_device(false);
    if (device_id == kAudioObjectUnknown) {
        SKIP("CoreAudio has no default output device");
    }

    const auto first = pulp::audio::mac::CoreAudioDevice::query_audio_io_timing(
        device_id, 19, 71);
    const auto next = pulp::audio::mac::CoreAudioDevice::query_audio_io_timing(
        device_id, 19, 72);

    REQUIRE(first.has_value());
    REQUIRE(next.has_value());
    CHECK(first->timestamp_domain == AudioTimestampDomain::device_sample_frames);
    CHECK(first->timestamp_source == AudioTimingSource::device_clock);
    CHECK(first->sample_rate_hz > 0.0);
    CHECK(first->confidence == AudioTimingConfidence::reported);
    REQUIRE(first->io_buffer_frames.has_value());
    CHECK(*first->io_buffer_frames > 0);
    CHECK(first->route_instance_token == 19);
    CHECK((first->input_latency_frames.has_value() ||
           first->output_latency_frames.has_value() ||
           first->input_safety_offset_frames.has_value() ||
           first->output_safety_offset_frames.has_value()));
    CHECK(next->calibration_generation > first->calibration_generation);
}

TEST_CASE("CoreAudio listener unregister preserves failed registrations",
          "[audio][timing][coreaudio]") {
    std::array<bool, 4> installed{true, true, false, true};
    const bool complete =
        pulp::audio::mac::remove_coreaudio_timing_listener_registrations(
            installed, [](std::size_t index) { return index != 1; });

    CHECK_FALSE(complete);
    CHECK_FALSE(installed[0]);
    CHECK(installed[1]);
    CHECK_FALSE(installed[2]);
    CHECK_FALSE(installed[3]);
    CHECK_FALSE(
        pulp::audio::mac::coreaudio_timing_listener_context_reusable(complete));
    CHECK(pulp::audio::mac::coreaudio_timing_listener_context_reusable(true));
}

TEST_CASE("CoreAudio listener context detaches owner and drains callbacks",
          "[audio][timing][coreaudio]") {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool release = false;
        unsigned calls = 0;
    } state;

    pulp::audio::mac::CoreAudioTimingListenerContext context(
        &state, [](void* opaque) noexcept {
            auto& state = *static_cast<State*>(opaque);
            std::unique_lock<std::mutex> lock(state.mutex);
            ++state.calls;
            state.entered = true;
            state.changed.notify_all();
            state.changed.wait(lock, [&state] { return state.release; });
        });

    std::thread callback([&context] { context.notify(); });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.changed.wait(lock, [&state] { return state.entered; });
    }

    std::atomic<bool> detached{false};
    std::thread drainer([&] {
        context.detach_and_wait();
        detached.store(true, std::memory_order_release);
    });
    while (context.is_attached()) std::this_thread::yield();
    CHECK_FALSE(detached.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.release = true;
    }
    state.changed.notify_all();
    callback.join();
    drainer.join();
    CHECK(detached.load(std::memory_order_acquire));

    context.notify();
    CHECK(state.calls == 1);

    context.attach(&state);
    context.notify();
    CHECK(state.calls == 2);
}
#endif
