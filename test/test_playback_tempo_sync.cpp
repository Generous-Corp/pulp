#include <pulp/playback/tempo_sync.hpp>
#include <pulp/playback/transport.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

CompiledTempoMap constant_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return require_compiled_tempo_map(points, RationalRate{48'000, 1});
}

class FakeTempoSyncSource final : public TempoSyncSource {
  public:
    void set_enabled(bool enabled) {
        enabled_ = enabled;
    }

    TempoSyncError capture_audio_block(const TempoSyncBlockRequest& request,
                                       TempoSyncBlockState& state) noexcept override {
        last_request = request;
        ++capture_count;
        if (!enabled_)
            return TempoSyncError::Disabled;
        if (!valid_tempo_sync_request(request))
            return TempoSyncError::InvalidRequest;
        if (invalid_state) {
            state.tempo_bpm = std::numeric_limits<double>::quiet_NaN();
            return TempoSyncError::None;
        }

        if (request.command.request_tempo)
            tempo = request.command.tempo_bpm;
        if (request.command.request_playing)
            playing = request.command.playing;
        if (request.command.request_beat)
            next_beat = request.command.beat;

        state.tempo_bpm = tempo;
        state.beat_start = next_beat;
        state.beat_end = next_beat + static_cast<double>(request.frame_count) * tempo /
                                         (60.0 * request.sample_rate);
        state.is_playing_at_host_time_micros = playing_transition_time_micros;
        state.is_playing = playing;
        next_beat = state.beat_end;
        return TempoSyncError::None;
    }

    TempoSyncBlockRequest last_request{};
    std::uint32_t capture_count = 0;
    double tempo = 120.0;
    double next_beat = 1.25;
    std::int64_t playing_transition_time_micros = 0;
    bool playing = true;
    bool invalid_state = false;

  private:
    bool enabled_ = true;
};

MasterTransportConfig config(FakeTempoSyncSource& source, std::uint32_t maximum = 4'800) {
    MasterTransportConfig result;
    result.max_buffer_size = maximum;
    result.tempo_sync_source = &source;
    result.tempo_sync_quantum_beats = 4.0;
    return result;
}

} // namespace

static_assert(TempoSyncSource::capture_audio_block_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);

TEST_CASE("tempo sync request and state validation fail closed", "[playback][tempo-sync]") {
    TempoSyncBlockRequest request;
    request.output_host_time_micros = 10;
    request.frame_count = 256;
    request.sample_rate = 48'000.0;
    REQUIRE(valid_tempo_sync_request(request));

    request.quantum_beats = 0.0;
    REQUIRE_FALSE(valid_tempo_sync_request(request));
    request.quantum_beats = 4.0;
    request.command.request_tempo = true;
    request.command.tempo_bpm = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(valid_tempo_sync_request(request));
    request.command.request_tempo = false;
    request.output_host_time_micros = std::numeric_limits<std::int64_t>::max();
    REQUIRE_FALSE(valid_tempo_sync_request(request));
    std::int64_t invalid_block_end = 0;
    REQUIRE_FALSE(tempo_sync_block_end_host_time_micros(request, invalid_block_end));

    TempoSyncBlockState state;
    state.tempo_bpm = 128.0;
    state.beat_start = 3.0;
    state.beat_end = 3.25;
    REQUIRE(valid_tempo_sync_state(state));
    state.beat_end = 2.0;
    REQUIRE_FALSE(valid_tempo_sync_state(state));
    state.beat_end = state.beat_start;
    state.is_playing = true;
    REQUIRE_FALSE(valid_tempo_sync_state(state));
    state.is_playing = false;
    REQUIRE(valid_tempo_sync_state(state));
    state.beat_end = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(valid_tempo_sync_state(state));
}

TEST_CASE("tempo sync playing projection honors half-open block boundaries",
          "[playback][tempo-sync]") {
    TempoSyncBlockRequest request;
    request.output_host_time_micros = 10'000;
    request.frame_count = 48;
    request.sample_rate = 48'000.0;
    REQUIRE(valid_tempo_sync_request(request));
    std::int64_t block_end = 0;
    REQUIRE(tempo_sync_block_end_host_time_micros(request, block_end));
    REQUIRE(block_end == 11'000);

    TempoSyncBlockState state;
    state.is_playing = true;

    state.is_playing_at_host_time_micros = 9'999;
    auto projection = project_tempo_sync_playing(request, state, false);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::BeforeOrAtBlockStart);
    REQUIRE(projection.playing_for_block);
    REQUIRE_FALSE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 10'000;
    projection = project_tempo_sync_playing(request, state, false);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::BeforeOrAtBlockStart);
    REQUIRE(projection.playing_for_block);
    REQUIRE_FALSE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 10'001;
    projection = project_tempo_sync_playing(request, state, false);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::InsideBlock);
    REQUIRE_FALSE(projection.playing_for_block);
    REQUIRE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 10'999;
    projection = project_tempo_sync_playing(request, state, false);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::InsideBlock);
    REQUIRE_FALSE(projection.playing_for_block);
    REQUIRE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 11'000;
    projection = project_tempo_sync_playing(request, state, false);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::AtOrAfterBlockEnd);
    REQUIRE_FALSE(projection.playing_for_block);
    REQUIRE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 11'001;
    projection = project_tempo_sync_playing(request, state, false);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::AtOrAfterBlockEnd);
    REQUIRE_FALSE(projection.playing_for_block);
    REQUIRE(projection.transition_deferred);

    state.is_playing = false;
    state.is_playing_at_host_time_micros = 9'999;
    projection = project_tempo_sync_playing(request, state, true);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::BeforeOrAtBlockStart);
    REQUIRE_FALSE(projection.playing_for_block);
    REQUIRE_FALSE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 10'000;
    projection = project_tempo_sync_playing(request, state, true);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::BeforeOrAtBlockStart);
    REQUIRE_FALSE(projection.playing_for_block);
    REQUIRE_FALSE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 10'500;
    projection = project_tempo_sync_playing(request, state, true);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::InsideBlock);
    REQUIRE(projection.playing_for_block);
    REQUIRE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 11'000;
    projection = project_tempo_sync_playing(request, state, true);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::AtOrAfterBlockEnd);
    REQUIRE(projection.playing_for_block);
    REQUIRE(projection.transition_deferred);

    state.is_playing_at_host_time_micros = 11'001;
    projection = project_tempo_sync_playing(request, state, true);
    REQUIRE(projection.boundary == TempoSyncPlayingBoundary::AtOrAfterBlockEnd);
    REQUIRE(projection.playing_for_block);
    REQUIRE(projection.transition_deferred);
}

TEST_CASE("master transport defers timestamped start and stop transitions to block boundaries",
          "[playback][tempo-sync]") {
    const auto map = constant_map();
    FakeTempoSyncSource source;
    source.playing = false;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config(source)) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(48, 1'000, snapshot) == TransportError::None);
    REQUIRE_FALSE(snapshot.is_playing);

    source.playing = true;
    source.playing_transition_time_micros = 2'500;
    REQUIRE(transport.begin_block(48, 2'000, snapshot) == TransportError::None);
    REQUIRE_FALSE(snapshot.is_playing);
    REQUIRE_FALSE(snapshot.transport_changed);
    REQUIRE_FALSE(snapshot.transport_started);
    REQUIRE(snapshot.ranges[0].host_tick_end == snapshot.ranges[0].host_tick_start);

    REQUIRE(transport.begin_block(48, 3'000, snapshot) == TransportError::None);
    REQUIRE(snapshot.is_playing);
    REQUIRE(snapshot.transport_changed);
    REQUIRE(snapshot.transport_started);
    REQUIRE(snapshot.ranges[0].host_tick_end > snapshot.ranges[0].host_tick_start);

    source.playing = false;
    source.playing_transition_time_micros = 4'500;
    REQUIRE(transport.begin_block(48, 4'000, snapshot) == TransportError::None);
    REQUIRE(snapshot.is_playing);
    REQUIRE_FALSE(snapshot.transport_changed);
    REQUIRE(snapshot.ranges[0].host_tick_end > snapshot.ranges[0].host_tick_start);

    REQUIRE(transport.begin_block(48, 5'000, snapshot) == TransportError::None);
    REQUIRE_FALSE(snapshot.is_playing);
    REQUIRE(snapshot.transport_changed);
    REQUIRE_FALSE(snapshot.transport_started);
    REQUIRE(snapshot.ranges[0].host_tick_end == snapshot.ranges[0].host_tick_start);
}

TEST_CASE("master transport rejects invalid tempo sync configuration", "[playback][tempo-sync]") {
    const auto map = constant_map();
    FakeTempoSyncSource source;
    auto invalid = config(source);
    invalid.tempo_sync_quantum_beats = 0.0;

    MasterTransport transport;
    REQUIRE(transport.prepare(map, invalid) == TransportError::InvalidTempoSyncConfig);

    REQUIRE(transport.prepare(map, config(source, 256)) == TransportError::None);
    REQUIRE(transport.begin_scrub(256, {}) == TransportError::InvalidTempoSyncConfig);

    MasterTransportConfig internal_config;
    internal_config.max_buffer_size = 256;
    REQUIRE(transport.prepare(map, internal_config) == TransportError::None);
    REQUIRE(transport.set_tempo_sync_tempo(120.0) == TransportError::InvalidTempoSyncConfig);
}

TEST_CASE("master transport consumes a backend-independent tempo sync mapping",
          "[playback][tempo-sync]") {
    const auto map = constant_map();
    FakeTempoSyncSource source;
    source.tempo = 128.0;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config(source)) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(256, snapshot) == TransportError::TempoSyncHostTimeRequired);
    REQUIRE(transport.begin_block(256, 42'000, snapshot) == TransportError::None);

    REQUIRE(source.capture_count == 1);
    REQUIRE(source.last_request.output_host_time_micros == 42'000);
    REQUIRE(source.last_request.quantum_beats == 4.0);
    REQUIRE_FALSE(source.last_request.command.request_playing);
    REQUIRE_FALSE(source.last_request.command.request_beat);
    REQUIRE_FALSE(source.last_request.command.request_tempo);
    REQUIRE(snapshot.is_playing);
    REQUIRE(snapshot.tempo_bpm == 128.0);
    REQUIRE(snapshot.range_count == 1);
    REQUIRE(snapshot.ranges[0].host_beat_mapping);
    REQUIRE(snapshot.ranges[0].has_precise_host_ticks);
    REQUIRE(snapshot.ranges[0].host_tick_start ==
            Catch::Approx(1.25 * static_cast<double>(kTicksPerQuarter)));
    REQUIRE(snapshot.ranges[0].host_tick_end > snapshot.ranges[0].host_tick_start);
    REQUIRE(snapshot.ranges[0].timeline_tick_start ==
            TickPosition{static_cast<std::int64_t>(1.25 * kTicksPerQuarter)});
}

TEST_CASE("master transport sends only explicit tempo sync commands", "[playback][tempo-sync]") {
    const auto map = constant_map();
    FakeTempoSyncSource source;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config(source)) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(128, 1'000, snapshot) == TransportError::None);
    REQUIRE(transport.set_playing(false) == TransportError::None);
    REQUIRE(transport.seek({2 * kTicksPerQuarter}) == TransportError::None);
    REQUIRE(transport.set_tempo_sync_tempo(0.0) == TransportError::InvalidTempo);
    REQUIRE(transport.set_tempo_sync_tempo(135.0) == TransportError::None);
    REQUIRE(transport.begin_block(128, 2'000, snapshot) == TransportError::None);

    REQUIRE(source.last_request.command.request_playing);
    REQUIRE_FALSE(source.last_request.command.playing);
    REQUIRE(source.last_request.command.request_beat);
    REQUIRE(source.last_request.command.beat == 2.0);
    REQUIRE(source.last_request.command.request_tempo);
    REQUIRE(source.last_request.command.tempo_bpm == 135.0);
    REQUIRE_FALSE(snapshot.is_playing);
    REQUIRE(snapshot.ranges[0].host_tick_end == snapshot.ranges[0].host_tick_start);

    REQUIRE(transport.begin_block(128, 3'000, snapshot) == TransportError::None);
    REQUIRE_FALSE(source.last_request.command.request_playing);
    REQUIRE_FALSE(source.last_request.command.request_beat);
    REQUIRE_FALSE(source.last_request.command.request_tempo);
}

TEST_CASE("tempo sync failure never falls back to the document clock", "[playback][tempo-sync]") {
    const auto map = constant_map();
    FakeTempoSyncSource source;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config(source)) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.set_playing(false) == TransportError::None);
    source.set_enabled(false);
    REQUIRE(transport.begin_block(128, 1'000, snapshot) == TransportError::TempoSyncUnavailable);
    source.set_enabled(true);
    REQUIRE(transport.begin_block(128, 2'000, snapshot) == TransportError::None);
    REQUIRE(source.last_request.command.request_playing);
    REQUIRE_FALSE(source.last_request.command.playing);

    source.invalid_state = true;
    REQUIRE(transport.begin_block(128, 3'000, snapshot) == TransportError::InvalidTempoSyncState);
}

TEST_CASE("tempo sync rejects beats outside Pulp's signed tick domain", "[playback][tempo-sync]") {
    const auto map = constant_map();
    FakeTempoSyncSource source;
    source.playing = false;
    source.next_beat = std::ldexp(1.0, 63) / static_cast<double>(kTicksPerQuarter);
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config(source, 256)) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(128, 1'000, snapshot) == TransportError::InvalidTempoSyncState);
}

TEST_CASE("tempo sync projection preserves the two-range loop contract", "[playback][tempo-sync]") {
    const auto map = constant_map();
    FakeTempoSyncSource source;
    // A relative epsilon would treat this whole block as "equal" to the loop
    // boundary because of the large absolute beat number. The projection must
    // use a precision-aware absolute tolerance and still split the wrap.
    source.next_beat = 1'000'000'003.9;
    auto setup = config(source);
    setup.loop = {true, {0}, {4 * kTicksPerQuarter}};
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(4'800, 10'000, snapshot) == TransportError::None);
    REQUIRE(snapshot.range_count == 2);
    REQUIRE(snapshot.ranges[0].timeline_tick_end == TickPosition{4 * kTicksPerQuarter});
    REQUIRE(snapshot.ranges[1].timeline_tick_start == TickPosition{0});
    REQUIRE(snapshot.ranges[1].discontinuity);
    REQUIRE(snapshot.ranges[0].frame_count + snapshot.ranges[1].frame_count == 4'800);
}

TEST_CASE("tempo sync continuity tolerates timestamp quantization but detects jumps",
          "[playback][tempo-sync]") {
    const auto map = constant_map();
    FakeTempoSyncSource source;
    source.next_beat = 8.0;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config(source)) == TransportError::None);

    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(256, 10'000, snapshot) == TransportError::None);
    REQUIRE_FALSE(snapshot.reset_requested);

    const auto half_sample_beat = 0.5 * source.tempo / (60.0 * 48'000.0);
    source.next_beat += half_sample_beat;
    REQUIRE(transport.begin_block(256, 20'000, snapshot) == TransportError::None);
    REQUIRE_FALSE(snapshot.reset_requested);

    source.next_beat += 0.25;
    REQUIRE(transport.begin_block(256, 30'000, snapshot) == TransportError::None);
    REQUIRE(snapshot.reset_requested);
}
