#include <pulp/playback/transport.hpp>
#include <pulp/timebase/beat_division.hpp>
#include <pulp/timebase/coordinate_random.hpp>
#include <pulp/timebase/grid_projection.hpp>
#include <pulp/timebase/groove_kernel.hpp>
#include <pulp/timebase/inline_groove_projector.hpp>
#include <pulp/timeline/model.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pulp::timebase;

namespace {

CompiledMeterMap meter_map(std::span<const MeterPoint> points) {
    auto result = CompiledMeterMap::compile(points);
    REQUIRE(result);
    return std::move(result).value();
}

class GridTempoSyncSource final : public pulp::playback::TempoSyncSource {
  public:
    pulp::playback::TempoSyncHostTime time(std::int64_t micros) const noexcept {
        return make_host_time(micros);
    }

    pulp::playback::TempoSyncError
    capture_audio_block(const pulp::playback::TempoSyncBlockRequest& request,
                        pulp::playback::TempoSyncBlockState& state) noexcept override {
        if (request.command.request_playing)
            playing = request.command.playing;
        if (request.command.request_beat)
            next_beat = request.command.beat;
        if (request.command.request_tempo)
            tempo = request.command.tempo_bpm;
        state.tempo_bpm = tempo;
        state.beat_start = next_beat;
        state.beat_end = next_beat + static_cast<double>(request.frame_count) * tempo /
                                         (60.0 * request.sample_rate);
        state.is_playing_at_host_time_micros = request.output_host_time_micros;
        state.is_playing = playing;
        next_beat = state.beat_end;
        return pulp::playback::TempoSyncError::None;
    }

    double tempo = 180.0;
    double next_beat = 0.9000001;
    bool playing = true;
};

std::uint64_t reference_multiply_high(std::uint64_t lhs, std::uint64_t rhs) {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    for (unsigned bit = 0; bit < 64; ++bit) {
        if ((rhs & (std::uint64_t{1} << bit)) == 0)
            continue;
        const auto add_low = lhs << bit;
        const auto add_high = bit == 0 ? 0 : lhs >> (64U - bit);
        const auto previous_low = low;
        low += add_low;
        high += add_high + static_cast<std::uint64_t>(low < previous_low);
    }
    return high;
}

#if defined(__SIZEOF_INT128__)
std::uint64_t reference_multiply_high_wide(std::uint64_t lhs, std::uint64_t rhs) {
    __extension__ using Wide = unsigned __int128;
    return static_cast<std::uint64_t>((static_cast<Wide>(lhs) * static_cast<Wide>(rhs)) >> 64U);
}
#endif

struct HostProjectionContext {
    HostGridAnchor anchor{};
    std::int64_t block_frame_start = 0;
    double loop_length_ticks = 0.0;
};

GridProjectionRange projection_range(const pulp::playback::TransportRange& range,
                                     const HostProjectionContext* host = nullptr) {
    GridProjectionRange result{
        range.sample_offset,       range.frame_count,       range.timeline_sample_start,
        range.timeline_tick_start, range.timeline_tick_end, range.monotonic_start,
        range.monotonic_end,       range.loop_pass_index,   range.host_beat_mapping,
        range.host_tick_start,     range.host_tick_end,     range.has_precise_host_ticks};
    if (range.host_beat_mapping && host != nullptr) {
        result.host_anchor = host->anchor;
        result.absolute_frame_start =
            host->block_frame_start + static_cast<std::int64_t>(range.sample_offset);
        result.document_to_source_tick_offset =
            static_cast<double>(range.loop_pass_index) * host->loop_length_ticks;
        result.has_host_anchor = true;
    }
    return result;
}

std::vector<GridProjectionPoint> project_snapshot(const CompiledTempoMap& tempo,
                                                  const CompiledMeterMap& meter,
                                                  const pulp::playback::TransportSnapshot& snapshot,
                                                  BeatDivision division = BeatDivision::Eighth,
                                                  GridAnchor anchor = GridAnchor::Timeline,
                                                  const HostProjectionContext* host = nullptr) {
    std::array<GridProjectionRange, 2> ranges{};
    for (std::size_t index = 0; index < snapshot.range_count; ++index)
        ranges[index] = projection_range(snapshot.ranges[index], host);
    std::array<GridProjectionPoint, 512> storage{};
    const auto result = project_grid(
        tempo, meter, {division, anchor, snapshot.is_playing},
        std::span<const GridProjectionRange>(ranges.data(), snapshot.range_count), storage);
    REQUIRE(result);
    return {storage.begin(), storage.begin() + static_cast<std::ptrdiff_t>(result.count)};
}

std::vector<GridProjectionPoint> range_oracle(const CompiledTempoMap& tempo,
                                              const CompiledMeterMap& meter,
                                              const pulp::playback::TransportSnapshot& snapshot,
                                              BeatDivision division) {
    const auto quantum = division_ticks(division).value().value;
    std::vector<GridProjectionPoint> result;
    for (std::size_t range_index = 0; range_index < snapshot.range_count; ++range_index) {
        const auto& range = snapshot.ranges[range_index];
        auto candidate = range.timeline_tick_start.value / quantum;
        if (range.timeline_tick_start.value % quantum > 0)
            ++candidate;
        candidate *= quantum;
        for (; candidate <= range.timeline_tick_end.value; candidate += quantum) {
            const auto sample = tempo.ticks_to_samples({candidate});
            const auto sample_end = range.timeline_sample_start.value + range.frame_count;
            if (sample.value < range.timeline_sample_start.value || sample.value >= sample_end)
                continue;
            const auto local_tick = candidate - range.timeline_tick_start.value;
            result.push_back({static_cast<std::uint32_t>(range.sample_offset + sample.value -
                                                         range.timeline_sample_start.value),
                              {candidate},
                              {{range.monotonic_start.position.value + local_tick}},
                              meter.tick_to_bar({candidate}),
                              range.loop_pass_index});
        }
    }
    return result;
}

struct TransportProjectionCapture {
    std::vector<GridProjectionPoint> points;
    std::vector<pulp::playback::TransportRange> ranges;
};

TransportProjectionCapture run_transport(const CompiledTempoMap& tempo,
                                         const CompiledMeterMap& meter,
                                         std::span<const std::uint32_t> blocks,
                                         std::uint64_t total_frames,
                                         TickPosition initial_position = {}) {
    pulp::playback::MasterTransport transport;
    const pulp::playback::LoopRegion loop{true, {kTicksPerQuarter}, {7 * kTicksPerQuarter}};
    REQUIRE(transport.prepare(tempo, {512, {4, 4}, loop, initial_position, true}) ==
            pulp::playback::TransportError::None);

    TransportProjectionCapture capture;
    std::uint64_t rendered = 0;
    std::size_t block_index = 0;
    while (rendered < total_frames) {
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            blocks[block_index++ % blocks.size()], total_frames - rendered));
        pulp::playback::TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(frames, snapshot) == pulp::playback::TransportError::None);
        const auto projected = project_snapshot(tempo, meter, snapshot);
        REQUIRE(projected == range_oracle(tempo, meter, snapshot, BeatDivision::Eighth));
        for (auto point : projected) {
            point.frame_offset += static_cast<std::uint32_t>(rendered);
            capture.points.push_back(point);
        }
        for (std::size_t index = 0; index < snapshot.range_count; ++index)
            capture.ranges.push_back(snapshot.ranges[index]);
        rendered += frames;
    }
    return capture;
}

std::vector<GridProjectionPoint> run_host_transport(const CompiledTempoMap& tempo,
                                                    const CompiledMeterMap& meter,
                                                    std::span<const std::uint32_t> blocks) {
    GridTempoSyncSource source;
    source.next_beat = 4.93748125;
    source.tempo = 180.0;
    pulp::playback::MasterTransportConfig config;
    config.max_buffer_size = 4'800;
    config.loop = {true, {0}, {kTicksPerQuarter}};
    config.initially_playing = true;
    config.tempo_sync_source = &source;
    pulp::playback::MasterTransport transport;
    REQUIRE(transport.prepare(tempo, config) == pulp::playback::TransportError::None);

    std::optional<HostGridAnchor> anchor;
    std::vector<GridProjectionPoint> result;
    std::uint32_t rendered = 0;
    std::size_t block_index = 0;
    while (rendered < 4'800) {
        const auto frames = std::min(blocks[block_index++ % blocks.size()], 4'800U - rendered);
        pulp::playback::TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(frames, source.time(rendered), snapshot) ==
                pulp::playback::TransportError::None);
        REQUIRE(snapshot.range_count > 0);
        if (!anchor) {
            anchor = HostGridAnchor{
                snapshot.ranges[0].host_tick_start, rendered + snapshot.ranges[0].sample_offset,
                source.tempo * static_cast<double>(kTicksPerQuarter) / (60.0 * 48'000.0)};
        }
        const HostProjectionContext context{*anchor, rendered,
                                            static_cast<double>(kTicksPerQuarter)};
        auto points = project_snapshot(tempo, meter, snapshot, BeatDivision::SixtyFourthTriplet,
                                       GridAnchor::Timeline, &context);
        for (auto& point : points) {
            point.frame_offset += rendered;
            result.push_back(point);
        }
        rendered += frames;
    }
    return result;
}

} // namespace

TEST_CASE("beat divisions are an append-only exact rational vocabulary", "[timebase][division]") {
    static_assert(static_cast<std::uint8_t>(BeatDivision::Whole) == 0);
    static_assert(static_cast<std::uint8_t>(BeatDivision::Quarter) == 6);
    static_assert(static_cast<std::uint8_t>(BeatDivision::SixtyFourthTriplet) == 20);

    constexpr std::array<BeatFraction, 21> expected{{
        {4, 1}, {6, 1}, {8, 3}, {2, 1}, {3, 1}, {4, 3},  {1, 1},  {3, 2},  {2, 3},  {1, 2},  {3, 4},
        {1, 3}, {1, 4}, {3, 8}, {1, 6}, {1, 8}, {3, 16}, {1, 12}, {1, 16}, {3, 32}, {1, 24},
    }};
    for (std::uint8_t index = 0; index < expected.size(); ++index) {
        const auto division = static_cast<BeatDivision>(index);
        REQUIRE(beat_fraction(division).value() == expected[index]);
        const auto ticks = division_ticks(division).value();
        REQUIRE(ticks.value * expected[index].denominator ==
                kTicksPerQuarter * expected[index].numerator);
    }
    REQUIRE(beat_fraction(static_cast<BeatDivision>(255)).error() ==
            BeatDivisionError::InvalidDivision);
}

TEST_CASE("coordinate randomness is stable and callback-independent", "[timebase][random]") {
    constexpr std::uint64_t seed = 0x123456789abcdef0ULL;
    const std::array ticks{
        std::numeric_limits<std::int64_t>::min(),
        std::int64_t{-1},
        std::int64_t{0},
        std::int64_t{1},
        std::numeric_limits<std::int64_t>::max(),
    };
    std::array<std::uint64_t, ticks.size()> whole{};
    for (std::size_t index = 0; index < ticks.size(); ++index)
        whole[index] = coordinate_random(seed, {{ticks[index]}, 7, 11, 13});
    constexpr std::array<std::uint64_t, ticks.size()> golden{
        0xd7c65501cdb45d38ULL, 0x7976f35d766e2f6eULL, 0xeb54175e98b40959ULL,
        0xe19e8a9a830b2cd2ULL, 0xa7f712082fc5ec9fULL,
    };
    REQUIRE(whole == golden);

    std::array<std::uint64_t, ticks.size()> partitioned{};
    for (const auto [begin, end] :
         std::array<std::pair<std::size_t, std::size_t>, 3>{{{0, 1}, {1, 4}, {4, 5}}}) {
        for (auto index = begin; index < end; ++index)
            partitioned[index] = coordinate_random(seed, {{ticks[index]}, 7, 11, 13});
    }
    REQUIRE(partitioned == whole);

    auto state = std::uint64_t{0x6a09e667f3bcc909ULL};
    for (int index = 0; index < 1'000; ++index) {
        state = detail::mix_coordinate(state);
        const auto lhs = state;
        state = detail::mix_coordinate(state);
        const auto rhs = state;
        REQUIRE(detail::multiply_high(lhs, rhs) == reference_multiply_high(lhs, rhs));
#if defined(__SIZEOF_INT128__)
        REQUIRE(detail::multiply_high(lhs, rhs) == reference_multiply_high_wide(lhs, rhs));
#endif
        const RandomCoordinate coordinate{{static_cast<std::int64_t>(lhs)}, rhs, 7, 9};
        const auto numerator = rhs / 3;
        const auto denominator = rhs == 0 ? std::uint64_t{1} : rhs;
        REQUIRE(coordinate_chance(seed, coordinate, numerator, denominator).value() ==
                (reference_multiply_high(coordinate_random(seed, coordinate), denominator) <
                 numerator));
    }
    REQUIRE(coordinate_chance(seed, {}, 18, 17).error() == ProbabilityError::InvalidRatio);
}

TEST_CASE("order-preserving groove kernel aligns with canonical groove domains",
          "[timebase][groove]") {
    static_assert(kGrooveKernelUnitScale == pulp::timeline::kGrooveUnitScale);
    static_assert(kMaximumGrooveKernelVelocityScale == pulp::timeline::kMaxGrooveVelocityScale);
    static_assert(kMaximumGrooveKernelSteps == pulp::timeline::kMaxGrooveSteps);

    constexpr TickDuration swing_grid{48};
    constexpr TickDuration table_grid{24};
    const std::array steps{
        GrooveKernelStep{{0}, 4'000},
        GrooveKernelStep{{1}, 2'000},
        GrooveKernelStep{{0}, 1'000},
        GrooveKernelStep{{0}, 0},
    };
    for (const auto strength : {0, 1, 500, 999, 1'000}) {
        const auto created = OrderPreservingGrooveKernel::create(
            {swing_grid, kTripletSwing, table_grid, steps, strength, strength});
        REQUIRE(created);
        const auto kernel = created.value();
        TickPosition previous{-192};
        for (std::int64_t tick = -192; tick <= 384; ++tick) {
            const auto applied = kernel.apply_timing({tick});
            REQUIRE(applied);
            REQUIRE(applied.value() >= previous);
            previous = applied.value();
        }
        if (strength == 0) {
            for (std::int64_t tick = -192; tick <= 384; ++tick)
                REQUIRE(kernel.apply_timing({tick}).value() == TickPosition{tick});
        }
        REQUIRE(kernel.velocity_scale_at({0}) >= kGrooveKernelUnitScale);
        REQUIRE(kernel.velocity_scale_at({3 * table_grid.value}) <= kGrooveKernelUnitScale);
    }

    const std::array reordering{
        GrooveKernelStep{{table_grid.value - 1}, 1'000},
        GrooveKernelStep{{-(table_grid.value - 1)}, 1'000},
    };
    REQUIRE(OrderPreservingGrooveKernel::create(
                {{}, kStraightSwing, table_grid, reordering, 1'000, 1'000})
                .error() == GrooveKernelError::ReordersEvents);

    const std::array one_step{GrooveKernelStep{{0}, 1'000}};
    REQUIRE(OrderPreservingGrooveKernel::create(
                {{1'000'000'000}, kTripletSwing, {999'999'937}, one_step, 1'000, 1'000})
                .error() == GrooveKernelError::ValidationLimitExceeded);

    const std::array overflowing{GrooveKernelStep{{1}, 1'000}};
    const auto edge = OrderPreservingGrooveKernel::create(
        {{}, kStraightSwing, table_grid, overflowing, 1'000, 1'000});
    REQUIRE(edge);
    REQUIRE(edge.value().apply_timing({std::numeric_limits<std::int64_t>::max()}).error() ==
            GrooveKernelError::RangeExceeded);
}

TEST_CASE("grid ceiling handles the signed minimum without invalid intermediates",
          "[timebase][grid]") {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    for (const std::int64_t quantum : std::array<std::int64_t, 6>{
             1, 2, 3, 7, 29'400, std::numeric_limits<std::int64_t>::max()}) {
        std::int64_t actual = 0;
        REQUIRE(detail::ceil_grid(minimum, quantum, actual));
        const auto quotient = minimum / quantum; // truncation is ceiling for a negative value
        REQUIRE(actual == quotient * quantum);
        REQUIRE(actual >= minimum);
        REQUIRE(actual % quantum == 0);
        if (actual != minimum)
            REQUIRE(actual < minimum + quantum);
    }
    std::int64_t unrepresentable = 0;
    REQUIRE_FALSE(detail::ceil_grid(std::numeric_limits<std::int64_t>::max(), 2, unrepresentable));
}

TEST_CASE("grid ranges match MasterTransport before loops and on repeated variable-tempo passes",
          "[timebase][grid]") {
    const std::array tempo_points{
        TempoPoint{{0}, 80.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 160.0, TempoCurve::LinearInTicks},
        TempoPoint{{3 * kTicksPerQuarter}, 100.0, TempoCurve::Constant},
    };
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{
        MeterPoint{{0}, {4, 4}},
        MeterPoint{{4 * kTicksPerQuarter}, {3, 4}},
    };
    const auto meter = meter_map(meter_points);
    const auto total =
        static_cast<std::uint64_t>(tempo.ticks_to_samples({7 * kTicksPerQuarter}).value +
                                   2 * (tempo.ticks_to_samples({7 * kTicksPerQuarter}).value -
                                        tempo.ticks_to_samples({kTicksPerQuarter}).value) +
                                   257);
    const std::array blocks_a{257U};
    const std::array blocks_b{31U, 127U, 3U, 211U, 19U};
    const auto regular = run_transport(tempo, meter, blocks_a, total);
    const auto partitioned = run_transport(tempo, meter, blocks_b, total);
    REQUIRE(regular.points == partitioned.points);

    bool saw_pre_loop = false;
    std::array<bool, 3> saw_pass{};
    std::array<MonotonicBeat, 3> pass_starts{};
    for (const auto& range : regular.ranges) {
        if (range.timeline_tick_start < TickPosition{kTicksPerQuarter})
            saw_pre_loop = true;
        if (range.loop_pass_index < saw_pass.size() &&
            range.timeline_tick_start >= TickPosition{kTicksPerQuarter}) {
            saw_pass[range.loop_pass_index] = true;
            if (range.timeline_tick_start == TickPosition{kTicksPerQuarter}) {
                pass_starts[range.loop_pass_index] = range.monotonic_start;
                REQUIRE(range.timeline_sample_start == tempo.ticks_to_samples({kTicksPerQuarter}));
                REQUIRE(range.tempo_bpm == tempo.tempo_at_tick({kTicksPerQuarter}));
            }
        }
        if (range.loop_pass_index > 0)
            REQUIRE(range.timeline_sample_start >= tempo.ticks_to_samples({kTicksPerQuarter}));
    }
    REQUIRE(saw_pre_loop);
    REQUIRE(saw_pass[0]);
    REQUIRE(saw_pass[1]);
    REQUIRE(saw_pass[2]);
    REQUIRE(pass_starts[1] > pass_starts[0]);
    REQUIRE(pass_starts[2] > pass_starts[1]);
}

TEST_CASE("sparse document ticks remain partition-independent at a half-open sample boundary",
          "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 1.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    REQUIRE(tempo.ticks_to_samples({0}) == SamplePosition{0});
    REQUIRE(tempo.ticks_to_samples({1}) == SamplePosition{4});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    const std::array whole_block{4U};
    const std::array split_once{1U, 3U};
    const std::array split_many{1U, 1U, 1U, 1U};
    const std::array split_irregular{2U, 1U, 1U};

    const auto whole = run_transport(tempo, meter, whole_block, 4);
    REQUIRE(whole.points.size() == 1);
    REQUIRE(whole.points.front().timeline_tick == TickPosition{0});
    REQUIRE(whole.points.front().frame_offset == 0);
    REQUIRE(run_transport(tempo, meter, split_once, 4).points == whole.points);
    REQUIRE(run_transport(tempo, meter, split_many, 4).points == whole.points);
    REQUIRE(run_transport(tempo, meter, split_irregular, 4).points == whole.points);

    const auto negative_grid = -division_ticks(BeatDivision::Eighth).value().value;
    const auto negative_whole = run_transport(tempo, meter, whole_block, 4, {negative_grid});
    REQUIRE(negative_whole.points.size() == 1);
    REQUIRE(negative_whole.points.front().timeline_tick == TickPosition{negative_grid});
    REQUIRE(run_transport(tempo, meter, split_once, 4, {negative_grid}).points ==
            negative_whole.points);
    REQUIRE(run_transport(tempo, meter, split_many, 4, {negative_grid}).points ==
            negative_whole.points);
}

TEST_CASE("grid seek preserves the transport's independent monotonic anchor and stop is empty",
          "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 120.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    pulp::playback::MasterTransport transport;
    REQUIRE(transport.prepare(tempo, {512, {4, 4}, {}, {0}, true}) ==
            pulp::playback::TransportError::None);
    pulp::playback::TransportSnapshot before;
    REQUIRE(transport.begin_block(257, before) == pulp::playback::TransportError::None);
    const auto monotonic_before_seek = before.ranges[0].monotonic_end;
    REQUIRE(transport.seek({2 * kTicksPerQuarter}) == pulp::playback::TransportError::None);
    pulp::playback::TransportSnapshot after;
    REQUIRE(transport.begin_block(257, after) == pulp::playback::TransportError::None);
    REQUIRE(after.ranges[0].timeline_tick_start == TickPosition{2 * kTicksPerQuarter});
    REQUIRE(after.ranges[0].monotonic_start == monotonic_before_seek);
    const auto points = project_snapshot(tempo, meter, after, BeatDivision::Quarter);
    REQUIRE_FALSE(points.empty());
    REQUIRE(points.front().timeline_tick == TickPosition{2 * kTicksPerQuarter});
    REQUIRE(points.front().transport_tick == monotonic_before_seek);

    REQUIRE(transport.set_playing(false) == pulp::playback::TransportError::None);
    pulp::playback::TransportSnapshot stopped;
    REQUIRE(transport.begin_block(257, stopped) == pulp::playback::TransportError::None);
    REQUIRE(project_snapshot(tempo, meter, stopped).empty());
}

TEST_CASE("bar-anchored grid resets exactly across a meter change and range split",
          "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 120.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{
        MeterPoint{{0}, {4, 4}},
        MeterPoint{{4 * kTicksPerQuarter}, {3, 4}},
    };
    const auto meter = meter_map(meter_points);
    const auto sample_at = [&](std::int64_t tick) { return tempo.ticks_to_samples({tick}).value; };
    const auto end_sample = sample_at(8 * kTicksPerQuarter);
    const GridProjectionRange whole{0,
                                    static_cast<std::uint32_t>(end_sample),
                                    {0},
                                    {0},
                                    {8 * kTicksPerQuarter},
                                    {{0}},
                                    {{8 * kTicksPerQuarter}},
                                    0};
    std::array<GridProjectionPoint, 16> whole_output{};
    const GridProjectionRequest request{BeatDivision::HalfDotted, GridAnchor::Bar, true};
    const auto whole_result = project_grid(
        tempo, meter, request, std::span<const GridProjectionRange>(&whole, 1), whole_output);
    REQUIRE(whole_result);
    REQUIRE(whole_result.count == 4);
    const std::array expected_ticks{TickPosition{0}, TickPosition{3 * kTicksPerQuarter},
                                    TickPosition{4 * kTicksPerQuarter},
                                    TickPosition{7 * kTicksPerQuarter}};
    for (std::size_t index = 0; index < expected_ticks.size(); ++index)
        REQUIRE(whole_output[index].timeline_tick == expected_ticks[index]);

    const auto split_sample = sample_at(5 * kTicksPerQuarter);
    const std::array ranges{
        GridProjectionRange{0,
                            static_cast<std::uint32_t>(split_sample),
                            {0},
                            {0},
                            {5 * kTicksPerQuarter},
                            {{0}},
                            {{5 * kTicksPerQuarter}},
                            0},
        GridProjectionRange{static_cast<std::uint32_t>(split_sample),
                            static_cast<std::uint32_t>(end_sample - split_sample),
                            {split_sample},
                            {5 * kTicksPerQuarter},
                            {8 * kTicksPerQuarter},
                            {{5 * kTicksPerQuarter}},
                            {{8 * kTicksPerQuarter}},
                            0},
    };
    std::array<GridProjectionPoint, 16> split_output{};
    const auto split_result = project_grid(tempo, meter, request, ranges, split_output);
    REQUIRE(split_result);
    REQUIRE(split_result.count == whole_result.count);
    REQUIRE(std::equal(whole_output.begin(), whole_output.begin() + whole_result.count,
                       split_output.begin()));
}

TEST_CASE("grid projection matches tempo-synced host mapping across a precise loop split",
          "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 60.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    GridTempoSyncSource source;
    pulp::playback::MasterTransportConfig config;
    config.max_buffer_size = 4'800;
    config.loop = {true, {0}, {kTicksPerQuarter}};
    config.initially_playing = true;
    config.tempo_sync_source = &source;
    pulp::playback::MasterTransport transport;
    REQUIRE(transport.prepare(tempo, config) == pulp::playback::TransportError::None);

    pulp::playback::TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(4'800, source.time(0), snapshot) ==
            pulp::playback::TransportError::None);
    REQUIRE(snapshot.range_count == 2);
    REQUIRE(snapshot.ranges[0].host_beat_mapping);
    REQUIRE(snapshot.ranges[0].has_precise_host_ticks);
    REQUIRE(snapshot.ranges[0].host_tick_start !=
            static_cast<double>(snapshot.ranges[0].timeline_tick_start.value));

    const HostProjectionContext host{
        {0.9000001 * static_cast<double>(kTicksPerQuarter), 0,
         180.0 * static_cast<double>(kTicksPerQuarter) / (60.0 * 48'000.0)},
        0,
        static_cast<double>(kTicksPerQuarter)};
    const auto points = project_snapshot(tempo, meter, snapshot, BeatDivision::SixtyFourth,
                                         GridAnchor::Timeline, &host);
    REQUIRE_FALSE(points.empty());
    bool differs_from_document_clock = false;
    for (const auto& point : points) {
        bool found_range = false;
        for (std::size_t index = 0; index < snapshot.range_count; ++index) {
            const auto& range = snapshot.ranges[index];
            std::uint32_t local = 0;
            if (!pulp::playback::host_mapped_output_offset_for_tick(range, point.timeline_tick,
                                                                    local))
                continue;
            const auto source_tick = static_cast<double>(point.timeline_tick.value) +
                                     static_cast<double>(range.loop_pass_index) * kTicksPerQuarter;
            const auto stable_frame = static_cast<std::int64_t>(
                std::floor((source_tick - host.anchor.source_tick) / host.anchor.ticks_per_frame));
            REQUIRE(point.frame_offset == static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                                              stable_frame, range.sample_offset,
                                              range.sample_offset + range.frame_count - 1)));
            const auto document_offset = tempo.ticks_to_samples(point.timeline_tick).value -
                                         range.timeline_sample_start.value;
            differs_from_document_clock =
                differs_from_document_clock || document_offset != static_cast<std::int64_t>(local);
            found_range = true;
            break;
        }
        REQUIRE(found_range);
    }
    REQUIRE(differs_from_document_clock);
    REQUIRE(points.front().loop_pass_index == 0);
    REQUIRE(points.back().loop_pass_index == 1);
}

TEST_CASE("stable host source/frame anchor survives callback and loop partitions",
          "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 60.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    const std::array whole{4'800U};
    const std::array reviewer_split{1'500U, 3'300U};
    const std::array boundary_split{1'000U, 500U, 3'300U};
    const std::array irregular{257U, 31U, 997U, 83U, 443U};

    const auto expected = run_host_transport(tempo, meter, whole);
    REQUIRE(run_host_transport(tempo, meter, reviewer_split) == expected);
    REQUIRE(run_host_transport(tempo, meter, boundary_split) == expected);
    REQUIRE(run_host_transport(tempo, meter, irregular) == expected);
    const auto reviewer_point =
        std::find_if(expected.begin(), expected.end(), [](const auto& point) {
            return point.timeline_tick == TickPosition{29'400} && point.loop_pass_index == 1;
        });
    REQUIRE(reviewer_point != expected.end());
    REQUIRE(reviewer_point->frame_offset == 1'666);
}

TEST_CASE("host grid projection rejects the exclusive signed frame bound", "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 60.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    GridProjectionRange range{0, 1, {0}, {0}, {1}, {{0}}, {{1}}, 0};
    range.host_beat_mapping = true;
    range.host_tick_start = 0.0;
    range.host_tick_end = 1.0;
    range.has_precise_host_ticks = true;
    range.host_anchor = {0.0, 0, 1.0};
    range.absolute_frame_start = 0;
    range.has_host_anchor = true;
    std::array<GridProjectionPoint, 1> output{{{77, {88}, {{99}}, {{11}, {22}}, 33}}};

    const auto project = [&] {
        return project_grid(tempo, meter, {BeatDivision::Quarter, GridAnchor::Timeline, true},
                            std::span<const GridProjectionRange>(&range, 1), output);
    };
    const auto positive = project();
    REQUIRE(positive);
    REQUIRE(positive.count == 1);
    REQUIRE(output[0].frame_offset == 0);

    output[0].frame_offset = 77;
    range.host_anchor = {-1.0, std::numeric_limits<std::int64_t>::max(), 1.0};
    const auto exclusive_upper = project();
    REQUIRE(exclusive_upper);
    REQUIRE(exclusive_upper.count == 0);
    REQUIRE(exclusive_upper.required == 0);
    REQUIRE(output[0].frame_offset == 77);
}

TEST_CASE("grid projection accepts the last representable output frame", "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 60.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    const GridProjectionRange range{std::numeric_limits<std::uint32_t>::max(),
                                    1,
                                    {0},
                                    {0},
                                    {1},
                                    {{0}},
                                    {{1}},
                                    0};
    std::array<GridProjectionPoint, 1> output{};

    const auto result =
        project_grid(tempo, meter, {BeatDivision::Quarter, GridAnchor::Timeline, true},
                     std::span<const GridProjectionRange>(&range, 1), output);

    REQUIRE(result);
    REQUIRE(result.count == 1);
    CHECK(output[0].frame_offset == std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("host grid projection is binary64-stable at a frame boundary", "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 60.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    constexpr std::int64_t document_tick = -1'656'925'200;
    GridProjectionRange range{
        0, 1'000, {0}, {-1'656'965'198}, {-1'656'869'050}, {{-1'656'965'198}}, {{-1'656'869'050}},
        0};
    range.host_beat_mapping = true;
    range.host_tick_start = -1'656'965'197.5003655;
    range.host_tick_end = -1'656'869'049.6629484;
    range.has_precise_host_ticks = true;
    range.host_anchor = {-2'554'068'054.6532755, 0, 96.1478374170194};
    range.absolute_frame_start = 565'000;
    range.document_to_source_tick_offset = -842'779'329.012294;
    range.has_host_anchor = true;
    std::array<GridProjectionPoint, 2> output{};

    const auto result =
        project_grid(tempo, meter, {BeatDivision::SixtyFourth, GridAnchor::Timeline, true},
                     std::span<const GridProjectionRange>(&range, 1), output);

    REQUIRE(result);
    REQUIRE(result.count == 2);
    REQUIRE(output[0].timeline_tick == TickPosition{document_tick});
    REQUIRE(output[0].frame_offset == 416);
}

TEST_CASE("host grid projection recovers an integer delta representation error moved",
          "[timebase][grid]") {
    using pulp::timebase::detail::grid_frame_error_budget;
    using pulp::timebase::detail::grid_integral_frame_delta;

    // The defect this covers: `frame_delta` is a cancellation between two tick
    // values near 1e9, so an exactly-on-grid event arrives as its integer plus
    // or minus several ulp OF THE INPUTS. A bare floor turns the low side into
    // the preceding frame, firing the event one sample early, and only ever in
    // that direction.
    const auto budget = grid_frame_error_budget(-2'499'704'529.012294, -2'554'068'054.6532755,
                                                96.1478374170194, 565'416.0);
    REQUIRE(budget > 0.0);

    SECTION("a delta nudged below an integer snaps back up, not down a frame") {
        const auto low = std::nextafter(565'416.0, 0.0);
        REQUIRE(low < 565'416.0);
        REQUIRE(std::floor(low) == 565'415.0); // what the bare floor did
        REQUIRE(grid_integral_frame_delta(low, budget) == 565'416.0);
    }

    SECTION("a delta nudged above an integer stays on it") {
        const auto high = std::nextafter(565'416.0, 1e9);
        REQUIRE(high > 565'416.0);
        REQUIRE(grid_integral_frame_delta(high, budget) == 565'416.0);
    }

    SECTION("the real measured value from the boundary range") {
        // Exactly the operands the binary64-stability case above projects.
        const auto source_tick = -1'656'925'200.0 + -842'779'329.012294;
        const auto frame_delta = (source_tick - -2'554'068'054.6532755) / 96.1478374170194;
        // Binary64 lands ABOVE the integer here while exact rational arithmetic
        // on the same inputs lands below it: the same intended 565416, either
        // side of the boundary. Snapping makes the answer independent of which.
        REQUIRE(frame_delta != 565'416.0);
        REQUIRE(std::abs(frame_delta - 565'416.0) < 1e-8);
        REQUIRE(grid_integral_frame_delta(
                    frame_delta, grid_frame_error_budget(source_tick, -2'554'068'054.6532755,
                                                         96.1478374170194, frame_delta)) ==
                565'416.0);
    }
}

TEST_CASE("grid placement policy is Floor by default and Nearest on request", "[timebase][grid]") {
    using pulp::timebase::GridPlacement;
    using pulp::timebase::detail::grid_integral_frame_delta;

    // A budget of 0 isolates the PLACEMENT decision from the epsilon recovery:
    // nothing is close enough to an integer to be snapped, so what comes back
    // is purely the policy.
    constexpr double kNoSnap = 0.0;

    SECTION("the default is Floor, so nothing that exists today moves") {
        // Load-bearing: Pulp's timeline has always floored. If the default ever
        // became Nearest, every projected event in the product would shift.
        REQUIRE(pulp::timebase::GridProjectionRequest{}.placement == GridPlacement::Floor);
        REQUIRE(grid_integral_frame_delta(55.6, kNoSnap) == 55.0);
        REQUIRE(grid_integral_frame_delta(55.4, kNoSnap) == 55.0);
    }

    SECTION("Floor never fires late") {
        for (const double fraction : {0.01, 0.25, 0.5, 0.75, 0.99}) {
            const auto placed =
                grid_integral_frame_delta(55.0 + fraction, kNoSnap, GridPlacement::Floor);
            REQUIRE(placed == 55.0);
        }
    }

    SECTION("Nearest takes the closer sample in either direction") {
        REQUIRE(grid_integral_frame_delta(55.4, kNoSnap, GridPlacement::Nearest) == 55.0);
        REQUIRE(grid_integral_frame_delta(55.6, kNoSnap, GridPlacement::Nearest) == 56.0);
        REQUIRE(grid_integral_frame_delta(-55.6, kNoSnap, GridPlacement::Nearest) == -56.0);
    }

    SECTION("Nearest breaks a tie away from zero, matching llround") {
        // This is why the policy is std::round rather than std::nearbyint.
        // nearbyint honours the current FP rounding mode, which defaults to
        // half-to-EVEN: it answers 56 for both 55.5 and 56.5. A consumer clock
        // using llround answers 56 and 57. The policy exists to match such a
        // consumer, so it must not disagree at the tie.
        REQUIRE(grid_integral_frame_delta(55.5, kNoSnap, GridPlacement::Nearest) == 56.0);
        REQUIRE(grid_integral_frame_delta(56.5, kNoSnap, GridPlacement::Nearest) == 57.0);
        REQUIRE(static_cast<double>(std::llround(56.5)) ==
                grid_integral_frame_delta(56.5, kNoSnap, GridPlacement::Nearest));
    }

    SECTION("the epsilon recovery still applies under both placements") {
        // Representation error must be recovered before the placement decides,
        // or a delta a few ulp below an integer floors a whole sample early --
        // the defect the epsilon exists for, which the policy must not reopen.
        const auto budget = pulp::timebase::detail::grid_frame_error_budget(
            -2'499'704'529.012294, -2'554'068'054.6532755, 96.1478374170194, 565'416.0);
        const auto low = std::nextafter(565'416.0, 0.0);
        REQUIRE(grid_integral_frame_delta(low, budget, GridPlacement::Floor) == 565'416.0);
        REQUIRE(grid_integral_frame_delta(low, budget, GridPlacement::Nearest) == 565'416.0);
    }
}

TEST_CASE("host grid projection never rounds a genuinely early event forward", "[timebase][grid]") {
    using pulp::timebase::detail::grid_frame_error_budget;
    using pulp::timebase::detail::grid_integral_frame_delta;

    SECTION("a fractional delta is floored, however close to the boundary") {
        const auto budget = grid_frame_error_budget(-2'499'704'529.012294, -2'554'068'054.6532755,
                                                    96.1478374170194, 565'416.0);
        // Anything the budget cannot explain as representation error must keep
        // the old behaviour exactly.
        for (const double fraction : {0.5, 0.25, 0.75, 0.999, 0.001, 0.1, 0.9}) {
            const auto delta = 565'416.0 + fraction;
            REQUIRE(grid_integral_frame_delta(delta, budget) == 565'416.0);
        }
        // Just outside the budget on the low side still floors down a frame --
        // the snap is a tolerance, not a re-rounding of the whole domain.
        const auto outside = 565'416.0 - budget * 16.0;
        REQUIRE(outside < 565'416.0);
        REQUIRE(grid_integral_frame_delta(outside, budget) == 565'415.0);
    }

    SECTION("the budget cannot reach half a frame for any usable tick magnitude") {
        // A genuinely early event differs from the boundary by a whole frame.
        // For the snap to reach one, the budget would have to reach 0.5 -- this
        // pins how far that is from anything a host can present. int64 ticks
        // are exact in binary64 only to 2^53, so that is the ceiling worth
        // testing; beyond it the tick value itself is already approximate.
        const auto exact_integer_ceiling = std::ldexp(1.0, 53);
        const auto worst = grid_frame_error_budget(exact_integer_ceiling, -exact_integer_ceiling,
                                                   1.0, exact_integer_ceiling);
        REQUIRE(worst < 0.5);
        // The raw forward-error bound reaches 48 frames there, so the ceiling
        // is what holds this -- not the bound. Assert the ceiling is the thing
        // doing the work, or a later widening of it passes unnoticed.
        REQUIRE(worst == pulp::timebase::detail::kGridMaxSnapFrames);
        REQUIRE(pulp::timebase::detail::kGridMaxSnapFrames < 0.5);

        // And at the magnitudes actually seen on a host clock it is ~1e-8.
        const auto realistic = grid_frame_error_budget(
            -2'499'704'529.012294, -2'554'068'054.6532755, 96.1478374170194, 565'416.0);
        REQUIRE(realistic < 1e-6);
    }
}

TEST_CASE("host grid projection preserves cancellable signed frame deltas", "[timebase][grid]") {
    std::uint32_t offset = 77;
    GridProjectionRange range{};
    range.frame_count = 20;
    range.host_beat_mapping = true;
    range.host_tick_start = 0.0;
    range.host_tick_end = 2.0;
    range.has_precise_host_ticks = true;
    range.absolute_frame_start = -10;
    range.has_host_anchor = true;

    range.host_anchor = {0.0, std::numeric_limits<std::int64_t>::min(), 1.0};
    range.document_to_source_tick_offset = std::ldexp(1.0, 63);
    REQUIRE(pulp::timebase::detail::host_mapped_grid_output_offset(range, {0}, offset));
    REQUIRE(offset == 10);

    range.host_anchor = {0.0, std::numeric_limits<std::int64_t>::max(), 1.0};
    range.document_to_source_tick_offset = -std::ldexp(1.0, 63);
    REQUIRE(pulp::timebase::detail::host_mapped_grid_output_offset(range, {0}, offset));
    REQUIRE(offset == 9);
}

TEST_CASE("grid candidate preflight bounds incoherent remote-sample ranges", "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 120.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const GridProjectionRange adversarial{0, 1, {0}, {0}, {maximum}, {{0}}, {{maximum}}, 0};
    std::array<GridProjectionPoint, 1> output{{{77, {88}, {{99}}, {{11}, {22}}, 33}}};
    for (const auto anchor : {GridAnchor::Timeline, GridAnchor::Bar}) {
        const auto result =
            project_grid(tempo, meter, {BeatDivision::SixtyFourthTriplet, anchor, true},
                         std::span<const GridProjectionRange>(&adversarial, 1), output);
        REQUIRE(result.error == GridProjectionError::ProjectionLimitExceeded);
        REQUIRE(output[0].frame_offset == 77);
    }
}

TEST_CASE("grid projection fails closed without touching caller output", "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 120.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    const auto quarter_samples = tempo.ticks_to_samples({kTicksPerQuarter}).value;
    const GridProjectionRange range{0,
                                    static_cast<std::uint32_t>(quarter_samples + 1),
                                    {0},
                                    {0},
                                    {kTicksPerQuarter},
                                    {{0}},
                                    {{kTicksPerQuarter}},
                                    0};
    constexpr GridProjectionPoint sentinel{77, {88}, {{99}}, {{11}, {22}}, 33};
    std::array<GridProjectionPoint, 1> output{sentinel};

    const auto too_small =
        project_grid(tempo, meter, {BeatDivision::Eighth, GridAnchor::Timeline, true},
                     std::span<const GridProjectionRange>(&range, 1), output);
    REQUIRE(too_small.error == GridProjectionError::OutputTooSmall);
    REQUIRE(too_small.count == 0);
    REQUIRE(too_small.required == 3);
    REQUIRE(output[0] == sentinel);

    const auto invalid_anchor =
        project_grid(tempo, meter, {BeatDivision::Quarter, static_cast<GridAnchor>(255), true},
                     std::span<const GridProjectionRange>(&range, 1), output);
    REQUIRE(invalid_anchor.error == GridProjectionError::InvalidAnchor);
    REQUIRE(invalid_anchor.count == 0);
    REQUIRE(invalid_anchor.required == 0);
    REQUIRE(output[0] == sentinel);
}

TEST_CASE("timebase groove projection remains allocation-free", "[timebase][groove][rt-safety]") {
    const std::array tempo_points{TempoPoint{{0}, 120.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {48'000, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    const auto quarter_samples = tempo.ticks_to_samples({kTicksPerQuarter}).value;
    const GridProjectionRange range{0,
                                    static_cast<std::uint32_t>(quarter_samples + 1),
                                    {0},
                                    {0},
                                    {kTicksPerQuarter},
                                    {{0}},
                                    {{kTicksPerQuarter}},
                                    0};
    std::array<GridProjectionPoint, 8> output{};
    const std::array steps{GrooveKernelStep{{0}, 1'000}, GrooveKernelStep{{1}, 1'100}};

    GridProjectionResult projected{};
    bool created_ok = false;
    bool transformed_ok = false;
    bool chance_ok = false;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        projected = project_grid(tempo, meter, {BeatDivision::Eighth, GridAnchor::Timeline, true},
                                 std::span<const GridProjectionRange>(&range, 1), output);
        const auto created = OrderPreservingGrooveKernel::create(
            {{}, kStraightSwing, {kTicksPerQuarter / 2}, steps, 1'000, 1'000});
        created_ok = static_cast<bool>(created);
        if (created)
            transformed_ok =
                static_cast<bool>(created.value().apply_timing({kTicksPerQuarter / 2}));
        chance_ok =
            static_cast<bool>(coordinate_chance(0x1234, {{kTicksPerQuarter}, 1, 2, 3}, 1, 2));
        allocations = probe.allocation_count();
    }

    REQUIRE(projected);
    REQUIRE(projected.count == 3);
    REQUIRE(created_ok);
    REQUIRE(transformed_ok);
    REQUIRE(chance_ok);
    REQUIRE(allocations == 0);
}

TEST_CASE("valid maximum tempo can collapse several exact grid points onto one sample",
          "[timebase][grid]") {
    const std::array tempo_points{TempoPoint{{0}, 1'000.0}};
    const auto tempo = require_compiled_tempo_map(tempo_points, {1, 1});
    const std::array meter_points{MeterPoint{{0}, {4, 4}}};
    const auto meter = meter_map(meter_points);
    const auto grid = division_ticks(BeatDivision::SixtyFourthTriplet).value().value;
    const GridProjectionRange range{
        0, 1, {0}, {-10 * grid}, {10 * grid}, {{-10 * grid}}, {{10 * grid}}, 0};
    std::array<GridProjectionPoint, 64> output{};
    const auto result =
        project_grid(tempo, meter, {BeatDivision::SixtyFourthTriplet, GridAnchor::Timeline, true},
                     std::span<const GridProjectionRange>(&range, 1), output);
    REQUIRE(result);
    REQUIRE(result.count > 1);
    for (std::size_t index = 0; index < result.count; ++index)
        REQUIRE(output[index].frame_offset == 0);
}

namespace {

// A groove table shaped like a real authored one: sixteen entries on a
// sixteenth grid, offsets of a few percent of an entry, leaning both ways.
// Authoring granularity of one percent of a sixteenth is 1'764 ticks, which is
// why such a table cannot fit the order-preserving kernel's admissible window.
std::array<GrooveKernelStep, 16> authored_sixteenth_groove(std::int64_t entry) {
    constexpr std::array<int, 16> percent{0, 4, -2, 3, 0, 5, -3, 2, 0, 4, -2, 3, 0, 5, -3, 2};
    constexpr std::array<std::int32_t, 16> accent{1'000, 820, 1'140, 760, 1'000, 880, 1'200, 700,
                                                  1'000, 820, 1'140, 760, 1'000, 880, 1'200, 700};
    std::array<GrooveKernelStep, 16> steps{};
    for (std::size_t index = 0; index < steps.size(); ++index)
        steps[index] = GrooveKernelStep{{percent[index] * entry / 100}, accent[index]};
    return steps;
}

pulp::timeline::GrooveTemplate canonical_groove(std::int64_t entry,
                                                std::span<const GrooveKernelStep> steps,
                                                TickDuration swing_grid, SwingRatio swing,
                                                std::int32_t strength) {
    pulp::timeline::GrooveTemplateInput input;
    input.swing_grid = swing_grid;
    input.swing = swing;
    input.step = TickDuration{entry};
    for (const auto& step : steps)
        input.steps.push_back(pulp::timeline::GrooveStep{step.timing_offset, step.velocity_scale});
    input.timing_strength = strength;
    input.velocity_strength = strength;
    auto created = pulp::timeline::GrooveTemplate::create(std::move(input));
    REQUIRE(created);
    return std::move(created).value();
}

} // namespace

TEST_CASE("inline groove projection admits the authored table the strict kernel refuses",
          "[timebase][groove]") {
    static_assert(std::is_trivially_copyable_v<InlineGrooveProjector>);
    static_assert(!std::is_trivially_copyable_v<pulp::timeline::GrooveTemplate>);
    static_assert(sizeof(InlineGrooveProjector) < sizeof(OrderPreservingGrooveKernel) / 16);
    static_assert(kMaximumInlineGrooveSteps == 16);

    constexpr std::int64_t entry = kTicksPerQuarter / 4;
    const auto steps = authored_sixteenth_groove(entry);
    const std::span<const GrooveKernelStep> table{steps};

    // The contrast is the reason this type exists: the same authored table, at
    // the same strength, on the same grid.
    const auto strict = OrderPreservingGrooveKernel::create(
        {{}, kStraightSwing, {entry}, table, kGrooveKernelUnitScale, kGrooveKernelUnitScale});
    REQUIRE_FALSE(strict);
    REQUIRE(strict.error() == GrooveKernelError::ReordersEvents);

    const auto created = InlineGrooveProjector::create(
        {{}, kStraightSwing, {entry}, table, kGrooveKernelUnitScale, kGrooveKernelUnitScale});
    REQUIRE(created);
    const auto projector = created.value();

    // Control: the strict kernel does admit a table that stays inside its
    // window, so the refusal above is about this table and not about the call.
    const std::array<GrooveKernelStep, 16> flat{};
    REQUIRE(OrderPreservingGrooveKernel::create({{},
                                                 kStraightSwing,
                                                 {entry},
                                                 std::span<const GrooveKernelStep>(flat),
                                                 kGrooveKernelUnitScale,
                                                 kGrooveKernelUnitScale}));

    // The projection is the canonical authored model's arithmetic, so it agrees
    // with it position for position and accent for accent.
    for (const auto strength : {0, 1, 250, 999, kGrooveKernelUnitScale}) {
        const auto canonical = canonical_groove(entry, table, {entry * 2}, kTripletSwing,
                                                static_cast<std::int32_t>(strength));
        const auto swung = InlineGrooveProjector::create({{entry * 2},
                                                          kTripletSwing,
                                                          {entry},
                                                          table,
                                                          static_cast<std::int32_t>(strength),
                                                          static_cast<std::int32_t>(strength)});
        REQUIRE(swung);
        for (std::int64_t tick = -4 * entry; tick <= 20 * entry; tick += 617) {
            const auto applied = swung.value().apply_timing({tick});
            REQUIRE(applied);
            REQUIRE(applied.value() == canonical.apply_timing({tick}));
            REQUIRE(swung.value().velocity_scale_at({tick}) == canonical.velocity_scale_at({tick}));
        }
    }

    // Reordering is admitted, which is the contract: the last tick of the entry
    // carrying +5% lands after the first tick of the entry carrying -3%.
    const auto late = projector.apply_timing({6 * entry - 1});
    const auto early = projector.apply_timing({6 * entry});
    REQUIRE(late);
    REQUIRE(early);
    REQUIRE(late.value() > early.value());

    // Unauthored slots project onto themselves and leave velocity alone.
    REQUIRE(projector.apply_timing({0}).value() == TickPosition{0});
    REQUIRE(projector.velocity_scale_at({0}) == kGrooveKernelUnitScale);
    REQUIRE(projector.velocity_scale_at({entry}) == 820);
    REQUIRE_FALSE(projector.states_no_feel());
    REQUIRE(InlineGrooveProjector{}.states_no_feel());
}

TEST_CASE("zero inline groove strength is exact identity including swing", "[timebase][groove]") {
    constexpr std::int64_t entry = kTicksPerQuarter / 4;
    const auto steps = authored_sixteenth_groove(entry);
    const std::span<const GrooveKernelStep> table{steps};
    const auto created =
        InlineGrooveProjector::create({{entry * 2}, kTripletSwing, {entry}, table, 0, 0});
    REQUIRE(created);
    const auto projector = created.value();

    // Control: at full strength the same swing and table displace, so the
    // identity below is the strength and not an inert configuration.
    const auto full = InlineGrooveProjector::create({{entry * 2},
                                                     kTripletSwing,
                                                     {entry},
                                                     table,
                                                     kGrooveKernelUnitScale,
                                                     kGrooveKernelUnitScale});
    REQUIRE(full);
    std::size_t displaced = 0;
    std::size_t scaled = 0;
    for (std::int64_t tick = -2 * entry; tick <= 8 * entry; tick += 37) {
        const auto applied = projector.apply_timing({tick});
        REQUIRE(applied);
        REQUIRE(applied.value() == TickPosition{tick});
        REQUIRE(projector.velocity_scale_at({tick}) == kGrooveKernelUnitScale);
        if (full.value().apply_timing({tick}).value() != TickPosition{tick})
            ++displaced;
        if (full.value().velocity_scale_at({tick}) != kGrooveKernelUnitScale)
            ++scaled;
    }
    REQUIRE(displaced > 0);
    REQUIRE(scaled > 0);

    // Swing alone, with no table at all, is identity at zero strength too.
    const auto swing_only =
        InlineGrooveProjector::create({{entry * 2}, kTripletSwing, {}, {}, 0, 0});
    REQUIRE(swing_only);
    for (std::int64_t tick = -entry; tick <= 4 * entry; tick += 11)
        REQUIRE(swing_only.value().apply_timing({tick}).value() == TickPosition{tick});
}

TEST_CASE("inline groove construction rejects out-of-domain tables", "[timebase][groove]") {
    constexpr std::int64_t entry = kTicksPerQuarter / 4;
    const std::array<GrooveKernelStep, 1> one{};
    const std::span<const GrooveKernelStep> single{one};

    std::array<GrooveKernelStep, kMaximumInlineGrooveSteps + 1> too_many{};
    REQUIRE(InlineGrooveProjector::create({{},
                                           kStraightSwing,
                                           {entry},
                                           std::span<const GrooveKernelStep>(too_many),
                                           kGrooveKernelUnitScale,
                                           kGrooveKernelUnitScale})
                .error() == InlineGrooveError::TooManySteps);

    // The largest admitted table is accepted, so the refusal above is the
    // capacity bound and not the table shape.
    std::array<GrooveKernelStep, kMaximumInlineGrooveSteps> at_capacity{};
    REQUIRE(InlineGrooveProjector::create({{},
                                           kStraightSwing,
                                           {entry},
                                           std::span<const GrooveKernelStep>(at_capacity),
                                           kGrooveKernelUnitScale,
                                           kGrooveKernelUnitScale}));

    const std::array<GrooveKernelStep, 1> over_entry{GrooveKernelStep{{entry}, 1'000}};
    REQUIRE(InlineGrooveProjector::create({{},
                                           kStraightSwing,
                                           {entry},
                                           std::span<const GrooveKernelStep>(over_entry),
                                           kGrooveKernelUnitScale,
                                           kGrooveKernelUnitScale})
                .error() == InlineGrooveError::InvalidTable);

    const std::array<GrooveKernelStep, 1> loud{
        GrooveKernelStep{{0}, kMaximumGrooveKernelVelocityScale + 1}};
    REQUIRE(InlineGrooveProjector::create({{},
                                           kStraightSwing,
                                           {entry},
                                           std::span<const GrooveKernelStep>(loud),
                                           kGrooveKernelUnitScale,
                                           kGrooveKernelUnitScale})
                .error() == InlineGrooveError::InvalidVelocityScale);

    REQUIRE(InlineGrooveProjector::create(
                {{}, kStraightSwing, {entry}, single, kGrooveKernelUnitScale + 1, 0})
                .error() == InlineGrooveError::InvalidStrength);
    REQUIRE(InlineGrooveProjector::create({{-1}, kStraightSwing, {entry}, single, 0, 0}).error() ==
            InlineGrooveError::InvalidSwingGrid);
    REQUIRE(InlineGrooveProjector::create({{entry}, {3, 2}, {entry}, single, 0, 0}).error() ==
            InlineGrooveError::InvalidSwing);
    // A table width with no entries names nothing, and entries with no width
    // have no position to be read at.
    REQUIRE(InlineGrooveProjector::create({{}, kStraightSwing, {entry}, {}, 0, 0}).error() ==
            InlineGrooveError::InvalidTable);
    REQUIRE(InlineGrooveProjector::create({{}, kStraightSwing, {}, single, 0, 0}).error() ==
            InlineGrooveError::InvalidTable);

    const std::array<GrooveKernelStep, 1> nudge{GrooveKernelStep{{1}, 1'000}};
    const auto edge = InlineGrooveProjector::create({{},
                                                     kStraightSwing,
                                                     {entry},
                                                     std::span<const GrooveKernelStep>(nudge),
                                                     kGrooveKernelUnitScale,
                                                     kGrooveKernelUnitScale});
    REQUIRE(edge);
    REQUIRE(edge.value().apply_timing({std::numeric_limits<std::int64_t>::max()}).error() ==
            InlineGrooveError::RangeExceeded);
}

TEST_CASE("inline groove projection remains allocation-free", "[timebase][groove][rt-safety]") {
    constexpr std::int64_t entry = kTicksPerQuarter / 4;
    const auto steps = authored_sixteenth_groove(entry);
    const std::span<const GrooveKernelStep> table{steps};

    bool created_ok = false;
    bool projected_ok = false;
    std::int64_t accumulated = 0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        const auto created = InlineGrooveProjector::create({{entry * 2},
                                                            kTripletSwing,
                                                            {entry},
                                                            table,
                                                            kGrooveKernelUnitScale,
                                                            kGrooveKernelUnitScale});
        created_ok = static_cast<bool>(created);
        if (created) {
            // Hot-swapping the whole projector by value is part of the path
            // under test, so the copy happens inside the probe too.
            const auto projector = created.value();
            projected_ok = true;
            for (std::int64_t tick = 0; tick < 64 * entry; tick += entry / 8) {
                const auto applied = projector.apply_timing({tick});
                if (!applied) {
                    projected_ok = false;
                    break;
                }
                accumulated += applied.value().value + projector.velocity_scale_at({tick});
            }
        }
        allocations = probe.allocation_count();
    }

    REQUIRE(created_ok);
    REQUIRE(projected_ok);
    REQUIRE(accumulated != 0);
    REQUIRE(allocations == 0);
}
