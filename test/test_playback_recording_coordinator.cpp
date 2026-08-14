#include "timebase_test_helpers.hpp"

#include <pulp/playback/recording_coordinator.hpp>
#include <pulp/project_package/project_package.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::audio;
using namespace pulp::playback;
using namespace pulp::project_package;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

CompiledTempoMap tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return require_compiled_tempo_map(points, RationalRate{48'000, 1});
}

LatencySnapshot latency_snapshot() {
    AudioIoTiming timing;
    timing.input_latency_frames = 3;
    timing.output_latency_frames = 4;
    timing.input_safety_offset_frames = 1;
    timing.output_safety_offset_frames = 1;
    timing.io_buffer_frames = 2;
    timing.sample_rate_hz = 48'000.0;
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::user_calibration;
    timing.confidence = AudioTimingConfidence::calibrated;
    timing.route_instance_token = 19;
    timing.calibration_generation = 7;
    const auto snapshot = make_latency_snapshot(timing, 5, 48'000.0);
    REQUIRE(snapshot);
    return *snapshot;
}

RecordingCoordinatorConfig coordinator_config() {
    RecordingCoordinatorConfig config;
    config.sample_rate = {48'000, 1};
    config.maximum_block_size = 8;
    config.maximum_take_frames = 64;
    config.take_slots_per_track = 2;
    config.midi_events_per_take = 8;
    config.latency = latency_snapshot();
    config.current_audio_timing = config.latency.audio_io;
    config.tracks.push_back({
        .track_id = {10},
        .take_lane_id = {101},
        .source = {.id = "loopback:device-19", .input_channel = 1, .output_channel = 0,
                   .channel_count = 1},
        .armed = true,
        .monitoring = RecordingMonitorMode::Auto,
    });
    return config;
}

Project recording_project() {
    auto track = Track::create(TrackInput{.id = {10}, .name = "record"});
    REQUIRE(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({.id = {1},
                                    .name = "recording",
                                    .next_item_id = 100,
                                    .root_sequence_id = {3},
                                    .sequences = {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

class RejectingStager final : public RecordingMediaStager {
  public:
    bool stage_media(const ContentHash&, std::span<const std::uint8_t>) noexcept override {
        called = true;
        return false;
    }
    bool called = false;
};

class PackageMediaStager final : public RecordingMediaStager {
  public:
    explicit PackageMediaStager(PackageWriter& writer) : writer_(writer) {}

    bool stage_media(const ContentHash& hash,
                     std::span<const std::uint8_t> bytes) noexcept override {
        return static_cast<bool>(writer_.stage_blob(BlobStore::Media, hash, bytes));
    }

  private:
    PackageWriter& writer_;
};

class TemporaryPackage {
  public:
    TemporaryPackage() {
        static std::atomic<std::uint64_t> serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("pulp-recording-coordinator-" + std::to_string(stamp) + "-" +
                std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    ~TemporaryPackage() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

RecordingTakeCommitRequest commit_request() {
    return {
        .sequence_id = {3},
        .track_id = {10},
        .take_lane_id = {101},
        .asset_id = {100},
        .take_id = {102},
        .asset_name = "calibrated-loopback.wav",
        .create_take_lane = true,
        .take_lane_name = "loopback take",
    };
}

float wave_sample(std::span<const std::uint8_t> bytes, std::size_t frame) {
    REQUIRE(bytes.size() >= 44 + (frame + 1) * sizeof(float));
    const auto offset = 44 + frame * sizeof(float);
    const auto bits = static_cast<std::uint32_t>(bytes[offset]) |
                      (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                      (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                      (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    return std::bit_cast<float>(bits);
}

} // namespace

TEST_CASE("recording coordinator commits a punched loopback take at calibrated placement") {
    auto config = coordinator_config();
    RecordingCoordinator coordinator;
    REQUIRE(coordinator.prepare(config));
    REQUIRE(coordinator.monitoring_path(0) == RecordingMonitorPath::Software);

    const auto map = tempo_map();
    MasterTransport transport;
    MasterTransportConfig transport_config;
    transport_config.max_buffer_size = 8;
    transport_config.initially_playing = true;
    REQUIRE(transport.prepare(map, transport_config) == TransportError::None);

    CaptureCommand start;
    start.type = CaptureCommandType::Start;
    start.sequence = 9;
    start.session.punch_in = {10};
    start.session.has_punch_out = true;
    start.session.punch_out = {26};
    REQUIRE(coordinator.enqueue_command(start));

    audio::Buffer<float> input(2, 8);
    audio::Buffer<float> monitor(1, 8);
    midi::MidiBuffer midi;
    for (std::int64_t block_start = 0; block_start < 32; block_start += 8) {
        for (std::size_t frame = 0; frame < 8; ++frame) {
            input.channel(0)[frame] = -1.0f;
            input.channel(1)[frame] = static_cast<float>(1000 + block_start + frame);
        }
        monitor.clear();
        TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(8, snapshot) == TransportError::None);
        auto monitor_view = monitor.view();
        REQUIRE(coordinator.process(static_cast<const audio::Buffer<float>&>(input).view(),
                                    monitor_view, midi, snapshot) == CaptureProcessResult::Ok);
        REQUIRE(monitor.channel(0)[0] == input.channel(1)[0]);
    }

    CaptureEvent event;
    CaptureEvent completed;
    bool found = false;
    while (coordinator.pop_event(event)) {
        if (event.type == CaptureEventType::TakeCompleted) {
            completed = event;
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE(completed.placement_start == SamplePosition{10});
    REQUIRE(completed.frame_count == 16);
    REQUIRE(completed.channel_count == 1);

    const auto initial = recording_project();
    auto session_result = DocumentSession::create(initial);
    REQUIRE(session_result);
    auto session = std::move(session_result).value();
    auto writer_result = session->register_writer();
    REQUIRE(writer_result);
    auto writer = std::move(writer_result).value();

    auto stale_timing = config.current_audio_timing;
    ++stale_timing.route_instance_token;
    RejectingStager stale_stager;
    auto stale_commit = coordinator.commit_take(completed, commit_request(), stale_timing,
                                                stale_stager, *session, writer);
    REQUIRE_FALSE(stale_commit);
    REQUIRE(stale_commit.error() == RecordingCoordinatorError::StaleLatency);
    REQUIRE_FALSE(stale_stager.called);

    auto forged = completed;
    forged.frame_count += 1;
    RejectingStager forged_stager;
    auto forged_commit = coordinator.commit_take(forged, commit_request(),
                                                 config.current_audio_timing, forged_stager,
                                                 *session, writer);
    REQUIRE_FALSE(forged_commit);
    REQUIRE(forged_commit.error() == RecordingCoordinatorError::InvalidTake);
    REQUIRE_FALSE(forged_stager.called);

    RejectingStager rejected;
    auto rejected_commit =
        coordinator.commit_take(completed, commit_request(), config.current_audio_timing, rejected,
                                *session, writer);
    REQUIRE_FALSE(rejected_commit);
    REQUIRE(rejected_commit.error() == RecordingCoordinatorError::MediaStageFailed);
    REQUIRE(rejected.called);
    REQUIRE(session->revision() == DocumentRevision{});
    REQUIRE(session->snapshot()->find_asset({100}) == nullptr);

    TemporaryPackage package;
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    auto package_writer = PackageWriter::create(package.path, std::move(registry).value());
    REQUIRE(package_writer);
    PackageMediaStager stager(*package_writer);
    auto committed =
        coordinator.commit_take(completed, commit_request(), config.current_audio_timing, stager,
                                *session, writer);
    REQUIRE(committed);
    REQUIRE(committed->source.id == "loopback:device-19");
    REQUIRE(committed->source.input_channel == 1);
    REQUIRE(committed->source.channel_count == 1);
    REQUIRE(committed->monitoring == RecordingMonitorPath::Software);
    REQUIRE(committed->take.placement_start() == SamplePosition{4});
    REQUIRE(committed->take.media().frame_count == 16);

    const BlobReference reference{BlobStore::Media, committed->asset.content_hash};
    auto media = read_blob(package.path, reference, 4096);
    REQUIRE(media);
    REQUIRE(wave_sample(*media, 0) == 1010.0f);
    REQUIRE(wave_sample(*media, 15) == 1025.0f);

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE(replayed->find_asset({100}) != nullptr);
    const auto* lane = replayed->find_sequence({3})->find_track({10})->find_take_lane({101});
    REQUIRE(lane != nullptr);
    REQUIRE(lane->find_take({102}) != nullptr);
    REQUIRE(lane->find_take({102})->placement_start() == SamplePosition{4});

    REQUIRE(session->undo(writer));
    REQUIRE(session->snapshot()->find_asset({100}) == nullptr);
    REQUIRE(session->snapshot()->find_sequence({3})->find_track({10})->find_take_lane({101}) ==
            nullptr);
    auto retained_media = read_blob(package.path, reference, 4096);
    REQUIRE(retained_media);
    REQUIRE(*retained_media == *media);

    RejectingStager duplicate_stager;
    auto duplicate = coordinator.commit_take(completed, commit_request(),
                                             config.current_audio_timing, duplicate_stager,
                                             *session, writer);
    REQUIRE_FALSE(duplicate);
    REQUIRE(duplicate.error() == RecordingCoordinatorError::InvalidTake);
    REQUIRE_FALSE(duplicate_stager.called);
}

TEST_CASE("recording coordinator trims latency compensation that crosses timeline zero") {
    auto config = coordinator_config();
    config.tracks[0].monitoring = RecordingMonitorMode::Off;
    RecordingCoordinator coordinator;
    REQUIRE(coordinator.prepare(config));

    const auto map = tempo_map();
    MasterTransport transport;
    MasterTransportConfig transport_config;
    transport_config.max_buffer_size = 8;
    transport_config.initially_playing = true;
    REQUIRE(transport.prepare(map, transport_config) == TransportError::None);

    CaptureCommand start;
    start.type = CaptureCommandType::Start;
    start.session.punch_in = {2};
    start.session.has_punch_out = true;
    start.session.punch_out = {10};
    REQUIRE(coordinator.enqueue_command(start));

    audio::Buffer<float> input(2, 8);
    midi::MidiBuffer midi;
    for (std::int64_t block_start = 0; block_start < 16; block_start += 8) {
        for (std::size_t frame = 0; frame < 8; ++frame)
            input.channel(1)[frame] = static_cast<float>(2000 + block_start + frame);
        TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(8, snapshot) == TransportError::None);
        if (block_start == 0) {
            audio::Buffer<float> short_monitor(1, 1);
            auto short_view = short_monitor.view();
            REQUIRE(coordinator.process(
                        static_cast<const audio::Buffer<float>&>(input).view(), short_view, midi,
                        snapshot) == CaptureProcessResult::InvalidBuffers);
        }
        audio::BufferView<float> no_monitor;
        REQUIRE(coordinator.process(static_cast<const audio::Buffer<float>&>(input).view(),
                                    no_monitor, midi, snapshot) == CaptureProcessResult::Ok);
    }

    CaptureEvent event;
    CaptureEvent completed;
    while (coordinator.pop_event(event)) {
        if (event.type == CaptureEventType::TakeCompleted)
            completed = event;
    }
    REQUIRE(completed.type == CaptureEventType::TakeCompleted);
    REQUIRE(completed.placement_start == SamplePosition{2});
    REQUIRE(completed.frame_count == 8);

    const auto initial = recording_project();
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    TemporaryPackage package;
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    auto package_writer = PackageWriter::create(package.path, std::move(registry).value());
    REQUIRE(package_writer);
    PackageMediaStager stager(*package_writer);
    auto committed = coordinator.commit_take(completed, commit_request(),
                                             config.current_audio_timing, stager, *session, writer);
    REQUIRE(committed);
    REQUIRE(committed->take.placement_start() == SamplePosition{0});
    REQUIRE(committed->take.media().frame_count == 4);
    auto media = read_blob(package.path,
                           {BlobStore::Media, committed->asset.content_hash}, 4096);
    REQUIRE(media);
    REQUIRE(wave_sample(*media, 0) == 2006.0f);
    REQUIRE(wave_sample(*media, 3) == 2009.0f);
}

TEST_CASE("recording coordinator selects Off Direct Software and Auto monitoring") {
    auto config = coordinator_config();
    config.tracks.clear();
    const auto add = [&](std::uint64_t id, RecordingMonitorMode mode, bool direct) {
        config.tracks.push_back({
            .track_id = {id},
            .take_lane_id = {100 + id},
            .source = {.id = "source-" + std::to_string(id),
                       .input_channel = 0,
                       .output_channel = 0,
                       .channel_count = 1},
            .monitoring = mode,
            .direct_monitoring_available = direct,
        });
    };
    add(1, RecordingMonitorMode::Off, false);
    add(2, RecordingMonitorMode::Direct, true);
    add(3, RecordingMonitorMode::Software, false);
    add(4, RecordingMonitorMode::Auto, true);
    add(5, RecordingMonitorMode::Auto, false);

    RecordingCoordinator coordinator;
    REQUIRE(coordinator.prepare(config));
    REQUIRE(coordinator.monitoring_path(0) == RecordingMonitorPath::Off);
    REQUIRE(coordinator.monitoring_path(1) == RecordingMonitorPath::Direct);
    REQUIRE(coordinator.monitoring_path(2) == RecordingMonitorPath::Software);
    REQUIRE(coordinator.monitoring_path(3) == RecordingMonitorPath::Direct);
    REQUIRE(coordinator.monitoring_path(4) == RecordingMonitorPath::Software);
}
