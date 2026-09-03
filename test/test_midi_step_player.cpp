#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/step_player.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace pulp;

constexpr std::uint64_t kSampleRate = 48'000;
// Shipped timebase constants: one quarter note is 705'600 ticks, so a
// sixteenth-note step is 176'400 ticks, which is exactly 6'000 samples at
// 120 bpm and 48 kHz. Every grid oracle below derives from these.
constexpr std::int64_t kStepTicks = timebase::kTicksPerQuarter / 4;
constexpr std::int64_t kStepSamples = 6'000;

midi::MidiBuffer prepared_buffer(std::size_t capacity = 512) {
    midi::MidiBuffer buffer;
    buffer.reserve(capacity, 4, 16);
    buffer.set_realtime_capacity_limit(true);
    return buffer;
}

timebase::CompiledTempoMap constant_tempo_map(double bpm = 120.0) {
    const std::array points{timebase::TempoPoint{{0}, bpm, timebase::TempoCurve::Constant}};
    auto compiled =
        timebase::CompiledTempoMap::compile(points, timebase::RationalRate{kSampleRate, 1});
    REQUIRE(compiled);
    return std::move(compiled).value();
}

struct AbsoluteMidiEvent {
    std::int64_t sample = 0;
    midi::MidiEvent event;

    auto identity() const {
        return std::tuple{sample,          event.is_note_on(), event.is_note_off(),
                          event.channel(), event.note(),       event.velocity()};
    }
};

struct EventLedger {
    std::array<int, 16 * 128> depth{};
    bool valid = true;

    void feed(const AbsoluteMidiEvent& absolute) {
        const auto& event = absolute.event;
        auto& value = depth[event.channel() * 128 + event.note()];
        if (event.is_note_on() && event.velocity() != 0) {
            ++value;
        } else if (event.is_note_off() || (event.is_note_on() && event.velocity() == 0)) {
            if (value == 0)
                valid = false;
            else
                --value;
        }
    }

    bool balanced() const {
        return valid &&
               std::all_of(depth.begin(), depth.end(), [](int value) { return value == 0; });
    }
};

template <std::size_t MaxLanes = 8, std::size_t MaxSteps = 32, std::size_t MaxRatchetHits = 8>
struct PlayerFixture {
    using Player = midi::StepPlayer<MaxLanes, MaxSteps, MaxRatchetHits>;
    typename Player::Spec spec{};

    explicit PlayerFixture(std::size_t lane_count = 1) {
        spec.lane_count = lane_count;
        for (std::size_t lane = 0; lane < lane_count; ++lane) {
            auto& lane_spec = spec.lanes[lane];
            lane_spec.channel = static_cast<std::uint8_t>(lane);
            lane_spec.base_note = 60;
            lane_spec.step_count = 4;
            lane_spec.step_duration = {kStepTicks};
        }
    }

    Player make() const {
        return Player(spec);
    }
};

template <typename Player>
std::vector<AbsoluteMidiEvent>
render(Player& player, const timebase::CompiledTempoMap& tempo_map, std::int64_t total_samples,
       std::span<const std::int32_t> partitions, std::span<const AbsoluteMidiEvent> input_events = {},
       std::span<const std::pair<std::int64_t, midi::StepPlayerTransportEvent>> transitions = {},
       bool allow_incomplete = false) {
    std::vector<AbsoluteMidiEvent> result;
    std::size_t partition_index = 0;
    std::size_t input_index = 0;
    std::size_t transition_index = 0;
    std::int64_t block_start = 0;
    while (block_start < total_samples) {
        const auto requested = partitions[partition_index++ % partitions.size()];
        const auto block_size = static_cast<std::int32_t>(
            std::min<std::int64_t>(requested, total_samples - block_start));
        auto input = prepared_buffer();
        while (input_index < input_events.size() &&
               input_events[input_index].sample < block_start + block_size) {
            REQUIRE(input_events[input_index].sample >= block_start);
            auto event = input_events[input_index].event;
            event.sample_offset =
                static_cast<std::int32_t>(input_events[input_index].sample - block_start);
            REQUIRE(input.add(event));
            ++input_index;
        }
        auto output = prepared_buffer(player.contract().required_output_events(input.size()));
        auto transition = midi::StepPlayerTransportEvent::Continuous;
        if (transition_index < transitions.size() &&
            transitions[transition_index].first == block_start) {
            transition = transitions[transition_index++].second;
        }
        const midi::StepPlayerBlock block{
            .sample_start = {block_start},
            .tick_start = tempo_map.samples_to_ticks({block_start}),
            .sample_count = block_size,
            .sample_rate = {kSampleRate, 1},
            .tempo_bpm = 120.0,
            .tempo_map = &tempo_map,
            .playing = transition != midi::StepPlayerTransportEvent::Stopped,
            .transport_event = transition,
        };
        const auto report = player.process(input, output, block);
        if (!allow_incomplete)
            REQUIRE(report.complete);
        for (const auto& event : output)
            result.push_back({block_start + event.sample_offset, event});
        block_start += block_size;
    }
    return result;
}

std::vector<AbsoluteMidiEvent> attacks_of(const std::vector<AbsoluteMidiEvent>& events) {
    std::vector<AbsoluteMidiEvent> result;
    for (const auto& event : events)
        if (event.event.is_note_on() && event.event.velocity() != 0)
            result.push_back(event);
    return result;
}

std::vector<AbsoluteMidiEvent> releases_of(const std::vector<AbsoluteMidiEvent>& events) {
    std::vector<AbsoluteMidiEvent> result;
    for (const auto& event : events)
        if (event.event.is_note_off())
            result.push_back(event);
    return result;
}

midi::StepPlayerStep basic_step() {
    midi::StepPlayerStep step;
    step.on = true;
    return step;
}

} // namespace

TEST_CASE("StepPlayer declares bounded state and validates its configuration",
          "[midi][step-player][contract]") {
    using Player = midi::StepPlayer<>;
    constexpr auto contract = Player::contract();
    STATIC_REQUIRE(contract.maximum_lanes == 8);
    STATIC_REQUIRE(contract.maximum_steps_per_lane == 32);
    STATIC_REQUIRE(contract.overflow == midi::MidiUtilityOverflowPolicy::DropUnstarted);
    STATIC_REQUIRE(contract.same_sample_order ==
                   midi::MidiUtilitySameSampleOrder::ReleaseBeforeAttack);
    STATIC_REQUIRE(contract.transport ==
                   midi::MidiUtilityTransportRequirement::FlushOnDiscontinuity);
    STATIC_REQUIRE(contract.requires_reserved_capacity_limited_output);

    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    REQUIRE(player.valid());

    midi::StepPlayerStep step;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::None);
    CHECK(player.set_step(1, 0, step) == midi::StepPlayerError::InvalidLane);
    CHECK(player.set_step(0, 4, step) == midi::StepPlayerError::InvalidStepIndex);
    step.velocity = 0;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::InvalidVelocity);
    step = {};
    step.gate_percent = 0;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::InvalidGate);
    step = {};
    step.probability_percent = 101;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::InvalidProbability);
    step = {};
    step.ratchet_count = 9;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::InvalidRatchetCount);
    step = {};
    step.pitch_offset = 60;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::InvalidPitchOffset);

    typename Player::Spec invalid_spec;
    invalid_spec.lane_count = 0;
    CHECK_FALSE(Player::valid_spec(invalid_spec));
    invalid_spec.lane_count = 1;
    invalid_spec.lanes[0].step_count = 0;
    CHECK_FALSE(Player::valid_spec(invalid_spec));
    invalid_spec.lanes[0].step_count = 4;
    invalid_spec.lanes[0].step_duration = {0};
    CHECK_FALSE(Player::valid_spec(invalid_spec));
}

TEST_CASE("StepPlayer places steps on exact tick-derived sample positions",
          "[midi][step-player][grid][oracle]") {
    STATIC_REQUIRE(timebase::kTicksPerQuarter == 705'600);
    STATIC_REQUIRE(kStepTicks == 176'400);
    const auto map = constant_tempo_map();
    // The fractional projection is the exact oracle; the integer
    // samples_to_ticks is a truncating convenience for block anchoring.
    const auto projected =
        map.fractional_samples_to_ticks(static_cast<long double>(kStepSamples));
    REQUIRE(std::fabs(static_cast<double>(projected) - 176'400.0) < 1e-6);

    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    for (std::size_t index = 0; index < 4; ++index) {
        auto step = basic_step();
        step.velocity = static_cast<std::uint8_t>(40 + index);
        step.pitch_offset = static_cast<std::int8_t>(index);
        REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
    }

    const std::array partitions{std::int32_t{24'000}};
    const auto events = render(player, map, 24'000, partitions);
    const auto attacks = attacks_of(events);
    REQUIRE(attacks.size() == 4);
    for (std::size_t index = 0; index < 4; ++index) {
        CHECK(attacks[index].sample == static_cast<std::int64_t>(index) * kStepSamples);
        CHECK(attacks[index].event.note() == 60 + index);
        CHECK(attacks[index].event.velocity() == 40 + index);
        CHECK(attacks[index].event.channel() == 0);
    }
    // Full-gate steps release exactly at the next step boundary.
    const auto releases = releases_of(events);
    REQUIRE(releases.size() == 3);
    for (std::size_t index = 0; index < 3; ++index)
        CHECK(releases[index].sample == static_cast<std::int64_t>(index + 1) * kStepSamples);
}

TEST_CASE("StepPlayer per-step gate, pitch, and velocity shape the emitted notes",
          "[midi][step-player][steps]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    auto gated = basic_step();
    gated.gate_percent = 50;
    REQUIRE(player.set_step(0, 0, gated) == midi::StepPlayerError::None);
    auto quiet = basic_step();
    quiet.velocity = 7;
    quiet.pitch_offset = -12;
    REQUIRE(player.set_step(0, 1, quiet) == midi::StepPlayerError::None);

    const std::array partitions{std::int32_t{18'000}};
    const auto events = render(player, map, 18'000, partitions);
    const auto attacks = attacks_of(events);
    REQUIRE(attacks.size() == 2);
    CHECK(attacks[1].event.velocity() == 7);
    CHECK(attacks[1].event.note() == 48);
    const auto releases = releases_of(events);
    REQUIRE(releases.size() == 2);
    // A 50% gate of a 176'400-tick step releases at tick 88'200, sample 3'000.
    CHECK(releases[0].sample == kStepSamples / 2);
    CHECK(releases[0].event.note() == 60);
    CHECK(releases[1].sample == 2 * kStepSamples);
}

TEST_CASE("StepPlayer direction walks visit steps in their documented orders",
          "[midi][step-player][direction]") {
    const auto map = constant_tempo_map();
    const auto run = [&](midi::StepPlayerDirection direction, std::int64_t steps) {
        PlayerFixture<> fixture(1);
        fixture.spec.lanes[0].direction = direction;
        auto player = fixture.make();
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            step.pitch_offset = static_cast<std::int8_t>(index);
            REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
        }
        const std::array partitions{std::int32_t{24'000}};
        const auto events = render(player, map, steps * kStepSamples, partitions);
        std::vector<int> notes;
        for (const auto& attack : attacks_of(events))
            notes.push_back(attack.event.note());
        return notes;
    };

    CHECK(run(midi::StepPlayerDirection::Forward, 6) ==
          std::vector<int>{60, 61, 62, 63, 60, 61});
    CHECK(run(midi::StepPlayerDirection::Reverse, 6) ==
          std::vector<int>{63, 62, 61, 60, 63, 62});
    CHECK(run(midi::StepPlayerDirection::PingPong, 8) ==
          std::vector<int>{60, 61, 62, 63, 62, 61, 60, 61});

    // Random walks stay inside the authored step set and are seed-stable.
    const auto first = run(midi::StepPlayerDirection::Random, 32);
    const auto second = run(midi::StepPlayerDirection::Random, 32);
    REQUIRE(first.size() == 32);
    CHECK(first == second);
    for (const auto note : first)
        CHECK(note >= 60);
    for (const auto note : first)
        CHECK(note <= 63);
}

TEST_CASE("StepPlayer polymeter realigns at the common multiple of lane lengths",
          "[midi][step-player][polymeter][oracle]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(2);
    fixture.spec.lanes[1].step_count = 3;
    auto player = fixture.make();
    for (std::size_t index = 0; index < 4; ++index) {
        auto step = basic_step();
        step.velocity = static_cast<std::uint8_t>(40 + index);
        REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
    }
    for (std::size_t index = 0; index < 3; ++index) {
        auto step = basic_step();
        step.velocity = static_cast<std::uint8_t>(80 + index);
        REQUIRE(player.set_step(1, index, step) == midi::StepPlayerError::None);
    }

    // Lanes of four and three sixteenth steps realign every twelve steps.
    const std::array partitions{std::int32_t{48'000}};
    const auto events = render(player, map, 24 * kStepSamples, partitions);
    const auto attacks = attacks_of(events);
    REQUIRE(attacks.size() == 48);
    const auto shape = [](const AbsoluteMidiEvent& event) {
        return std::tuple{event.event.is_note_on(), event.event.channel(), event.event.note(),
                          event.event.velocity()};
    };
    for (std::size_t index = 0; index < 24; ++index) {
        CHECK(shape(attacks[index]) == shape(attacks[index + 24]));
        CHECK(attacks[index + 24].sample - attacks[index].sample == 12 * kStepSamples);
    }
}

TEST_CASE("StepPlayer probability zero never fires and coordinate draws are deterministic",
          "[midi][step-player][probability][oracle]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);

    auto never = fixture.make();
    auto silent = basic_step();
    silent.probability_percent = 0;
    for (std::size_t index = 0; index < 4; ++index)
        REQUIRE(never.set_step(0, index, silent) == midi::StepPlayerError::None);
    const std::array partitions{std::int32_t{24'000}};
    CHECK(attacks_of(render(never, map, 24'000, partitions)).empty());

    auto certain = fixture.make();
    auto always = basic_step();
    always.probability_percent = 100;
    for (std::size_t index = 0; index < 4; ++index)
        REQUIRE(certain.set_step(0, index, always) == midi::StepPlayerError::None);
    CHECK(attacks_of(render(certain, map, 24'000, partitions)).size() == 4);

    auto fifty_a = fixture.make();
    auto fifty_b = fixture.make();
    auto maybe = basic_step();
    maybe.probability_percent = 50;
    REQUIRE(fifty_a.set_step(0, 0, maybe) == midi::StepPlayerError::None);
    REQUIRE(fifty_b.set_step(0, 0, maybe) == midi::StepPlayerError::None);
    const auto first = render(fifty_a, map, 24 * kStepSamples, partitions);
    const auto second = render(fifty_b, map, 24 * kStepSamples, partitions);
    REQUIRE(first.size() == second.size());
    for (std::size_t index = 0; index < first.size(); ++index)
        CHECK(first[index].identity() == second[index].identity());
}

TEST_CASE("StepPlayer ratchet bursts subdivide the step interval exactly",
          "[midi][step-player][ratchet][oracle]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);

    auto single = fixture.make();
    auto one = basic_step();
    one.ratchet_count = 1;
    REQUIRE(single.set_step(0, 0, one) == midi::StepPlayerError::None);
    const std::array partitions{std::int32_t{6'000}};
    const auto single_attacks = attacks_of(render(single, map, kStepSamples, partitions));
    REQUIRE(single_attacks.size() == 1);
    CHECK(single_attacks[0].sample == 0);

    auto burst = fixture.make();
    auto four = basic_step();
    four.ratchet_count = 4;
    four.gate_percent = 50;
    REQUIRE(burst.set_step(0, 0, four) == midi::StepPlayerError::None);
    const auto events = render(burst, map, kStepSamples, partitions);
    const auto attacks = attacks_of(events);
    REQUIRE(attacks.size() == 4);
    // The 176'400-tick interval splits into exact quarters: 44'100 ticks is
    // 1'500 samples at 120 bpm and 48 kHz.
    for (std::size_t hit = 0; hit < 4; ++hit)
        CHECK(attacks[hit].sample == static_cast<std::int64_t>(hit) * 1'500);
    // Each hit releases before the next begins; the 50% gate of one quarter
    // step is 22'050 ticks, which is 750 samples.
    const auto releases = releases_of(events);
    REQUIRE(releases.size() == 4);
    for (std::size_t hit = 0; hit < 4; ++hit) {
        CHECK(releases[hit].sample == static_cast<std::int64_t>(hit) * 1'500 + 750);
        CHECK(releases[hit].event.note() == attacks[hit].event.note());
    }
}

TEST_CASE("StepPlayer tie extends a sounding note and slide overlaps a pitch change",
          "[midi][step-player][continuity]") {
    const auto map = constant_tempo_map();
    const std::array partitions{std::int32_t{24'000}};

    PlayerFixture<> tie_fixture(1);
    auto tied = tie_fixture.make();
    REQUIRE(tied.set_step(0, 0, basic_step()) == midi::StepPlayerError::None);
    auto tie_step = basic_step();
    tie_step.tie = true;
    REQUIRE(tied.set_step(0, 1, tie_step) == midi::StepPlayerError::None);
    auto different = basic_step();
    different.pitch_offset = 2;
    REQUIRE(tied.set_step(0, 2, different) == midi::StepPlayerError::None);

    const auto tie_events = render(tied, map, 24'000, partitions);
    const auto tie_attacks = attacks_of(tie_events);
    // Step one is tied into step zero, so no attack happens at its boundary.
    REQUIRE(tie_attacks.size() == 2);
    CHECK(tie_attacks[0].sample == 0);
    CHECK(tie_attacks[0].event.note() == 60);
    CHECK(tie_attacks[1].sample == 2 * kStepSamples);
    CHECK(tie_attacks[1].event.note() == 62);
    const auto tie_releases = releases_of(tie_events);
    REQUIRE(tie_releases.size() == 2);
    // The tied note spans steps zero and one with a single attack.
    CHECK(tie_releases[0].sample == 2 * kStepSamples);
    CHECK(tie_releases[0].event.note() == 60);
    CHECK(tie_releases[1].sample == 3 * kStepSamples);

    PlayerFixture<> slide_fixture(1);
    auto sliding = slide_fixture.make();
    REQUIRE(sliding.set_step(0, 0, basic_step()) == midi::StepPlayerError::None);
    auto slide_step = basic_step();
    slide_step.slide = true;
    slide_step.pitch_offset = 2;
    REQUIRE(sliding.set_step(0, 1, slide_step) == midi::StepPlayerError::None);

    const auto slide_events = render(sliding, map, 24'000, partitions);
    const auto slide_attacks = attacks_of(slide_events);
    REQUIRE(slide_attacks.size() >= 2);
    CHECK(slide_attacks[1].sample == kStepSamples);
    // The previous note releases strictly after the new attack, so a
    // downstream voice can glide across the boundary.
    const auto first_release = std::find_if(slide_events.begin(), slide_events.end(),
                                            [](const AbsoluteMidiEvent& event) {
                                                return event.event.is_note_off() &&
                                                       event.event.note() == 60;
                                            });
    REQUIRE(first_release != slide_events.end());
    CHECK(first_release->sample > kStepSamples);
}

TEST_CASE("StepPlayer choke groups cut sounding notes on peer lanes",
          "[midi][step-player][choke]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(2);
    fixture.spec.lanes[0].choke_group = 1;
    fixture.spec.lanes[1].choke_group = 1;
    fixture.spec.lanes[1].base_note = 64;
    // Lane 1 runs thirty-second steps; its first step is silent and its second
    // fires half a step after lane 0's attack.
    fixture.spec.lanes[1].step_duration = {kStepTicks / 2};
    auto player = fixture.make();
    REQUIRE(player.set_step(0, 0, basic_step()) == midi::StepPlayerError::None);
    REQUIRE(player.set_step(1, 1, basic_step()) == midi::StepPlayerError::None);

    const std::array partitions{std::int32_t{6'000}};
    const auto events = render(player, map, kStepSamples, partitions);
    const auto attacks = attacks_of(events);
    REQUIRE(attacks.size() == 2);
    CHECK(attacks[0].sample == 0);
    CHECK(attacks[0].event.note() == 60);
    CHECK(attacks[1].sample == kStepSamples / 2);
    CHECK(attacks[1].event.note() == 64);
    // Lane 0's note is choked exactly at lane 1's attack, not at its own gate.
    const auto releases = releases_of(events);
    REQUIRE(!releases.empty());
    CHECK(releases[0].sample == kStepSamples / 2);
    CHECK(releases[0].event.note() == 60);
}

TEST_CASE("StepPlayer flush releases every owned note and stays balanced",
          "[midi][step-player][flush]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    REQUIRE(player.set_step(0, 0, basic_step()) == midi::StepPlayerError::None);

    const std::array partitions{std::int32_t{3'000}};
    auto events = render(player, map, kStepSamples, partitions);
    CHECK(player.sounding_note_count() == 1);

    auto flush_output = prepared_buffer();
    const auto report = player.flush_transport(flush_output);
    REQUIRE(report.complete);
    REQUIRE(flush_output.size() == 1);
    CHECK(flush_output[0].is_note_off());
    CHECK(flush_output[0].note() == 60);
    CHECK(player.sounding_note_count() == 0);
    for (const auto& event : flush_output)
        events.push_back({kStepSamples, event});

    EventLedger ledger;
    for (const auto& event : events)
        ledger.feed(event);
    CHECK(ledger.balanced());
}

TEST_CASE("StepPlayer treats transport jumps as flush plus resync without stuck notes",
          "[midi][step-player][transport]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    for (std::size_t index = 0; index < 4; ++index) {
        auto step = basic_step();
        step.pitch_offset = static_cast<std::int8_t>(index);
        REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
    }

    EventLedger ledger;
    // The jump targets tick 2.5 steps while the sample clock keeps running, so
    // block two opens with the owed release and the resynced grid.
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    midi::StepPlayerBlock first_block{
        .sample_start = {0},
        .tick_start = {0},
        .sample_count = 24'000,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .tempo_map = &map,
        .playing = true,
        .transport_event = midi::StepPlayerTransportEvent::Started,
    };
    REQUIRE(player.process(input, output, first_block).complete);
    for (const auto& event : output)
        ledger.feed({event.sample_offset, event});
    CHECK(player.sounding_note_count() == 1);

    auto jumped_output = prepared_buffer();
    midi::StepPlayerBlock jumped_block{
        .sample_start = {24'000},
        .tick_start = {2 * kStepTicks + kStepTicks / 2},
        .sample_count = 24'000,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .tempo_map = &map,
        .playing = true,
        .transport_event = midi::StepPlayerTransportEvent::Seeked,
    };
    REQUIRE(player.process(input, jumped_output, jumped_block).complete);
    REQUIRE(jumped_output.size() >= 2);
    // The sounding note from before the jump is released at the block start...
    CHECK(jumped_output[0].is_note_off());
    CHECK(jumped_output[0].sample_offset == 0);
    // ...and the lane resyncs to the grid: the next step boundary after the
    // jump target is three steps in, which is 3'000 samples into this block.
    const auto attacks = [&] {
        std::vector<midi::MidiEvent> result;
        for (const auto& event : jumped_output)
            if (event.is_note_on())
                result.push_back(event);
        return result;
    }();
    REQUIRE(!attacks.empty());
    CHECK(attacks[0].sample_offset == 3'000);
    CHECK(attacks[0].note() == 63);
    for (const auto& event : jumped_output)
        ledger.feed({24'000 + event.sample_offset, event});

    auto flush_output = prepared_buffer();
    REQUIRE(player.flush_transport(flush_output).complete);
    for (const auto& event : flush_output)
        ledger.feed({48'000, event});
    CHECK(ledger.balanced());
    CHECK(player.sounding_note_count() == 0);
}

TEST_CASE("StepPlayer is exact across callback partitions",
          "[midi][step-player][partition]") {
    const auto map = constant_tempo_map();
    const auto build = [] {
        PlayerFixture<> fixture(2);
        fixture.spec.random_seed = 0x5eed;
        fixture.spec.lanes[0].direction = midi::StepPlayerDirection::PingPong;
        fixture.spec.lanes[1].direction = midi::StepPlayerDirection::Random;
        fixture.spec.lanes[1].step_count = 3;
        auto player = fixture.make();
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            step.pitch_offset = static_cast<std::int8_t>(index);
            step.ratchet_count = static_cast<std::uint8_t>(1 + (index % 3));
            REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
        }
        for (std::size_t index = 0; index < 3; ++index) {
            auto step = basic_step();
            step.probability_percent = static_cast<std::uint8_t>(25 + 25 * index);
            step.gate_percent = 75;
            REQUIRE(player.set_step(1, index, step) == midi::StepPlayerError::None);
        }
        return player;
    };

    constexpr std::int64_t kTotal = 48'000;
    auto whole = build();
    const std::array single{std::int32_t{48'000}};
    const auto reference = render(whole, map, kTotal, single);

    auto fine = build();
    const std::array even{std::int32_t{512}};
    const auto chopped = render(fine, map, kTotal, even);

    auto ragged = build();
    const std::array odd{std::int32_t{137}, std::int32_t{999}, std::int32_t{4'096},
                         std::int32_t{1}};
    const auto uneven = render(ragged, map, kTotal, odd);

    REQUIRE(reference.size() == chopped.size());
    REQUIRE(reference.size() == uneven.size());
    for (std::size_t index = 0; index < reference.size(); ++index) {
        CHECK(reference[index].identity() == chopped[index].identity());
        CHECK(reference[index].identity() == uneven[index].identity());
    }
}

TEST_CASE("StepPlayer refuses ledger overflow by dropping the newest attack",
          "[midi][step-player][overflow]") {
    const auto map = constant_tempo_map();
    // One lane with a single ratchet slot holds two sounding notes; a slide at
    // every step keeps both alive, so the third attack has no ledger slot.
    PlayerFixture<1, 4, 1> fixture(1);
    auto player = fixture.make();
    for (std::size_t index = 0; index < 4; ++index) {
        auto step = basic_step();
        step.pitch_offset = static_cast<std::int8_t>(index);
        if (index > 0)
            step.slide = true;
        REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
    }

    const std::array partitions{std::int32_t{24'000}};
    const auto events = render(player, map, 24'000, partitions, {}, {}, true);
    const auto attacks = attacks_of(events);
    CHECK(attacks.size() < 4);
    CHECK(player.sounding_note_count() <= 2);
    // Refused attacks never strand a note: everything emitted still balances.
    auto flush_output = prepared_buffer();
    REQUIRE(player.flush_transport(flush_output).complete);
    EventLedger ledger;
    for (const auto& event : events)
        ledger.feed(event);
    for (const auto& event : flush_output)
        ledger.feed({24'000, event});
    CHECK(ledger.balanced());
}

TEST_CASE("StepPlayer passes input through unchanged while generating",
          "[midi][step-player][passthrough]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    REQUIRE(player.set_step(0, 0, basic_step()) == midi::StepPlayerError::None);

    auto controller = midi::MidiEvent::cc(3, 17, 91);
    const std::array input_events{AbsoluteMidiEvent{9, controller}};
    const std::array partitions{std::int32_t{24'000}};
    const auto events = render(player, map, 24'000, partitions, input_events);
    const auto forwarded = std::find_if(events.begin(), events.end(),
                                        [](const AbsoluteMidiEvent& event) {
                                            return event.event.is_cc();
                                        });
    REQUIRE(forwarded != events.end());
    CHECK(forwarded->sample == 9);
    CHECK(forwarded->event.channel() == 3);
    CHECK(forwarded->event.cc_number() == 17);
    CHECK(forwarded->event.cc_value() == 91);
    CHECK(!attacks_of(events).empty());
}

TEST_CASE("StepPlayer process path allocates nothing after capacity preparation",
          "[midi][step-player][rt-safety]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(2);
    fixture.spec.random_seed = 42;
    auto player = fixture.make();
    for (std::size_t lane = 0; lane < 2; ++lane)
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            step.ratchet_count = static_cast<std::uint8_t>(1 + index);
            step.slide = index % 2 == 1;
            REQUIRE(player.set_step(lane, index, step) == midi::StepPlayerError::None);
        }

    auto input = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::cc(0, 1, 64)));
    auto output = prepared_buffer(player.contract().required_output_events(input.size()));
    const midi::StepPlayerBlock warm_block{
        .sample_start = {0},
        .tick_start = {0},
        .sample_count = 512,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .tempo_map = &map,
        .playing = true,
        .transport_event = midi::StepPlayerTransportEvent::Started,
    };
    REQUIRE(player.process(input, output, warm_block).complete);

    auto steady_output = prepared_buffer(player.contract().required_output_events(input.size()));
    const midi::StepPlayerBlock steady_block{
        .sample_start = {512},
        .tick_start = map.samples_to_ticks({512}),
        .sample_count = 512,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .tempo_map = &map,
        .playing = true,
        .transport_event = midi::StepPlayerTransportEvent::Continuous,
    };
    pulp::test::RtAllocationProbe allocations;
    const auto report = player.process(input, steady_output, steady_block);
    CHECK_FALSE(allocations.saw_allocation());
    CHECK(report.complete);
}

TEST_CASE("StepPlayer nudges a step off the grid by its timing offset",
          "[midi][step-player][grid][oracle]") {
    // The offset is what lets a caller express swing, a groove table or a
    // per-step nudge without running its own scheduler. A quarter is 705'600
    // ticks and a sixteenth step is 176'400, which is exactly 6'000 samples at
    // 120 bpm / 48 kHz, so a nudge of kStepTicks/4 is exactly 1'500 samples.
    constexpr std::int32_t kNudgeTicks = static_cast<std::int32_t>(kStepTicks / 4);
    constexpr std::int64_t kNudgeSamples = kStepSamples / 4;
    STATIC_REQUIRE(kNudgeTicks == 44'100);

    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    // Alternate steps are pushed late, which is the shape swing has.
    for (std::size_t index = 0; index < 4; ++index) {
        auto step = basic_step();
        step.timing_offset_ticks = (index % 2 == 1) ? kNudgeTicks : 0;
        REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
    }

    const std::array partitions{std::int32_t{24'000}};
    const auto events = render(player, map, 24'000, partitions);
    const auto attacks = attacks_of(events);
    REQUIRE(attacks.size() == 4);
    for (std::size_t index = 0; index < 4; ++index) {
        const std::int64_t grid = static_cast<std::int64_t>(index) * kStepSamples;
        const std::int64_t expected = grid + (index % 2 == 1 ? kNudgeSamples : 0);
        CHECK(attacks[index].sample == expected);
    }
    // Sensitivity control: the nudged steps really did move off the grid, so a
    // kernel that ignored the field could not satisfy the assertions above.
    CHECK(attacks[1].sample != 1 * kStepSamples);
    CHECK(attacks[3].sample != 3 * kStepSamples);
}

TEST_CASE("StepPlayer keeps a nudged step's gate length", "[midi][step-player][grid]") {
    // The release moves with the attack. A step nudged late must not be
    // shortened into the next step's boundary.
    constexpr std::int32_t kNudgeTicks = static_cast<std::int32_t>(kStepTicks / 4);
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);
    fixture.spec.lanes[0].step_count = 2;
    auto player = fixture.make();
    for (std::size_t index = 0; index < 2; ++index) {
        auto step = basic_step();
        step.gate_percent = 50;
        step.timing_offset_ticks = kNudgeTicks;
        REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
    }
    const std::array partitions{std::int32_t{12'000}};
    const auto events = render(player, map, 12'000, partitions);
    const auto attacks = attacks_of(events);
    const auto releases = releases_of(events);
    REQUIRE(attacks.size() >= 1);
    REQUIRE(releases.size() >= 1);
    // A 50% gate on a 6'000-sample step is 3'000 samples, regardless of nudge.
    CHECK(releases[0].sample - attacks[0].sample == kStepSamples / 2);
}

TEST_CASE("StepPlayer rejects a timing offset beyond its declared bound",
          "[midi][step-player][parity]") {
    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    auto step = basic_step();

    step.timing_offset_ticks = midi::StepPlayer<>::kMaximumTimingOffsetTicks;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::None);

    // One past the bound is refused rather than silently honoured, because a
    // nudge that large reorders the grid against itself.
    step.timing_offset_ticks = midi::StepPlayer<>::kMaximumTimingOffsetTicks + 1;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::InvalidTimingOffset);

    // An early nudge is refused, not clamped. A step is discovered when its own
    // interval opens, so pulling it earlier asks it to sound before the block
    // that found it — and whether it then survived would depend on where the
    // host put its callback boundaries, which is what this kernel promises not
    // to depend on. Refusing says so at authoring time instead of producing a
    // stream that is right at one block size and wrong at another.
    step.timing_offset_ticks = -1;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::InvalidTimingOffset);
    step.timing_offset_ticks = -midi::StepPlayer<>::kMaximumTimingOffsetTicks;
    CHECK(player.set_step(0, 0, step) == midi::StepPlayerError::InvalidTimingOffset);

    // The bound must be wide enough to express swing on a sixteenth grid, which
    // a 16-bit field could not: it would cap at 32'767 of the 176'400 ticks a
    // sixteenth spans.
    STATIC_REQUIRE(midi::StepPlayer<>::kMaximumTimingOffsetTicks > kStepTicks / 2);
}

TEST_CASE("StepPlayer with a zero timing offset is unchanged", "[midi][step-player][parity]") {
    // The bypass identity: adding the field must not move anything that does
    // not ask to be moved.
    const auto map = constant_tempo_map();
    auto run = [&](bool set_offset) {
        PlayerFixture<> fixture(1);
        auto player = fixture.make();
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            if (set_offset)
                step.timing_offset_ticks = 0;
            player.set_step(0, index, step);
        }
        const std::array partitions{std::int32_t{24'000}};
        return attacks_of(render(player, map, 24'000, partitions));
    };
    const auto without = run(false);
    const auto with_zero = run(true);
    REQUIRE(without.size() == with_zero.size());
    for (std::size_t index = 0; index < without.size(); ++index)
        CHECK(without[index].sample == with_zero[index].sample);
    for (std::size_t index = 0; index < without.size(); ++index)
        CHECK(without[index].sample == static_cast<std::int64_t>(index) * kStepSamples);
}

TEST_CASE("StepPlayer nudged steps are exact across callback partitions",
          "[midi][step-player][partition][grid]") {
    // One sample spans 29.4 ticks at 120 bpm / 48 kHz, so sweeping the nudge
    // walks a step's onset across every sub-sample phase — including the half
    // sample where rounding decides between two neighbouring samples. A host
    // is free to hand the same transport to the kernel in any block sizes it
    // likes, so the phase a nudged onset lands on must not depend on where the
    // callback boundaries happen to fall.
    const auto map = constant_tempo_map();
    const auto build = [](std::int32_t nudge) {
        PlayerFixture<> fixture(1);
        auto player = fixture.make();
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            step.pitch_offset = static_cast<std::int8_t>(index);
            // Offsets are late-only, so a groove nudges the off-beats and
            // leaves the down-beats on the grid — the shape a swing plus a
            // groove table produces.
            step.timing_offset_ticks = index % 2 == 1 ? nudge : 0;
            REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
        }
        return player;
    };

    constexpr std::int64_t kTotal = 24'000;
    const std::array single{std::int32_t{24'000}};
    const std::array even{std::int32_t{512}};
    const std::array odd{std::int32_t{137}, std::int32_t{999}, std::int32_t{4'096},
                         std::int32_t{1}};

    // The last two exceed a sixteenth's own interval, so they exercise the bound
    // as well as the projection: an over-wide nudge is held inside the step it
    // belongs to rather than reaching the next one.
    for (const std::int32_t nudge : {1, 7, 14, 15, 22, 29, 30, 44, 59, 100, 1'471, 44'100, 176'400,
                                     midi::StepPlayer<>::kMaximumTimingOffsetTicks}) {
        INFO("nudge ticks = " << nudge);
        auto whole = build(nudge);
        const auto reference = render(whole, map, kTotal, single);
        auto fine = build(nudge);
        const auto chopped = render(fine, map, kTotal, even);
        auto ragged = build(nudge);
        const auto uneven = render(ragged, map, kTotal, odd);

        REQUIRE(reference.size() == chopped.size());
        REQUIRE(reference.size() == uneven.size());
        for (std::size_t index = 0; index < reference.size(); ++index) {
            INFO("event index = " << index << " reference sample = " << reference[index].sample
                                  << " even sample = " << chopped[index].sample
                                  << " ragged sample = " << uneven[index].sample);
            CHECK(reference[index].identity() == chopped[index].identity());
            CHECK(reference[index].identity() == uneven[index].identity());
        }
    }
}

TEST_CASE("StepPlayer holds an over-wide nudge inside the step that owns it",
          "[midi][step-player][grid]") {
    // The declared bound is a quarter note, but a step on a sixteenth grid owns
    // only a sixteenth of it. A nudge wider than the step's own interval is
    // accepted at authoring time and held at fire time, so an authored groove
    // can never push one step past the next one's grid position — which is what
    // would reorder the sequence against itself.
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(1);
    auto player = fixture.make();
    for (std::size_t index = 0; index < 4; ++index) {
        auto step = basic_step();
        step.pitch_offset = static_cast<std::int8_t>(index);
        step.timing_offset_ticks = midi::StepPlayer<>::kMaximumTimingOffsetTicks;
        REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
    }

    const std::array whole{std::int32_t{30'000}};
    const auto attacks = attacks_of(render(player, map, 30'000, whole));

    // Four steps' displaced onsets fall inside this window; the fifth lands on
    // the limit and belongs to the next call. Asserting the count first means a
    // clamp that swallowed a step could not make the ordering below look right.
    REQUIRE(attacks.size() == 4);
    for (std::size_t index = 0; index < attacks.size(); ++index) {
        const auto grid = static_cast<std::int64_t>(index) * kStepSamples;
        INFO("step " << index << " attack sample = " << attacks[index].sample);
        // Displaced off its own grid position, and no further than the next
        // one. The bound is strict in ticks, which is the kernel's coordinate;
        // one interval short of a sixteenth is 176'399 of 176'400 ticks, and
        // that rounds onto the next grid sample rather than past it.
        CHECK(attacks[index].sample > grid);
        CHECK(attacks[index].sample <= grid + kStepSamples);
        if (index > 0)
            CHECK(attacks[index].sample > attacks[index - 1].sample);
    }
}

TEST_CASE("StepPlayer places a nudged onset independently of where a block begins",
          "[midi][step-player][partition][grid]") {
    // A block reports its own start in both samples and ticks, and the two do
    // not name the same instant. A tick is finer than a sample here -- 29.4 of
    // them span one -- so many ticks map to the same sample, and the tick a
    // host reports is the first of them. That start therefore sits up to half a
    // sample below where the block actually begins, by an amount that depends
    // on the sample it began on. Projecting an onset relative to it folds that
    // offset into the answer, which is enough to move the onset by a whole
    // sample depending only on where the host chose to break its callbacks.
    // Sweeping the leading block size walks the boundary through every residue
    // and so through the offset's full range.
    //
    // The grid alone cannot show this: a sixteenth is exactly 6'000 samples, so
    // an un-nudged onset always lands dead on one and has half a sample of room
    // on either side. It takes a nudge to move an onset close enough to a
    // sample boundary for the offset to carry it across, which is why this
    // arrived with the offset.
    const auto map = constant_tempo_map();
    const auto build = [](std::int32_t nudge) {
        PlayerFixture<> fixture(1);
        auto player = fixture.make();
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            step.pitch_offset = static_cast<std::int8_t>(index);
            step.timing_offset_ticks = nudge;
            REQUIRE(player.set_step(0, index, step) == midi::StepPlayerError::None);
        }
        return player;
    };

    constexpr std::int64_t kTotal = 24'000;
    const std::array single{std::int32_t{24'000}};

    // Each of these lands the second step's onset within the offset's reach of
    // the boundary between two samples, so a projection that carries the offset
    // reports a different sample from one that does not.
    for (const std::int32_t nudge : {30, 59, 89, 118, 177, 176'443}) {
        INFO("nudge ticks = " << nudge);
        auto whole = build(nudge);
        const auto reference = render(whole, map, kTotal, single);
        REQUIRE(!reference.empty());

        for (std::int32_t phase = 1; phase <= 40; ++phase) {
            const std::array partitions{phase, std::int32_t{4'096}};
            auto shifted = build(nudge);
            const auto observed = render(shifted, map, kTotal, partitions);
            INFO("leading block = " << phase);
            REQUIRE(observed.size() == reference.size());
            for (std::size_t index = 0; index < reference.size(); ++index) {
                INFO("event index = " << index << " reference sample = " << reference[index].sample
                                      << " observed sample = " << observed[index].sample);
                CHECK(reference[index].identity() == observed[index].identity());
            }
        }
    }
}

TEST_CASE("StepPlayer publishes the coordinate a lane's walk selects",
          "[midi][step-player][coordinate]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(2);
    fixture.spec.lanes[1].direction = midi::StepPlayerDirection::Reverse;
    auto player = fixture.make();
    for (std::size_t lane = 0; lane < 2; ++lane)
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            step.pitch_offset = static_cast<std::int8_t>(index);
            REQUIRE(player.set_step(lane, index, step) == midi::StepPlayerError::None);
        }

    // The query is pure in the tick: it answers before any block is processed.
    CHECK_FALSE(player.upcoming_coordinate(0).has_value());
    CHECK_FALSE(player.coordinate_at(2, {0}).has_value());

    for (std::int64_t ordinal = 0; ordinal < 9; ++ordinal) {
        const timebase::TickPosition tick{ordinal * kStepTicks};
        const auto forward = player.coordinate_at(0, tick);
        REQUIRE(forward.has_value());
        CHECK(forward->ordinal == ordinal);
        CHECK(forward->cycle == static_cast<std::uint64_t>(ordinal / 4));
        CHECK(forward->index == static_cast<std::size_t>(ordinal % 4));
        CHECK(forward->following_tick.value == tick.value + kStepTicks);

        const auto reverse = player.coordinate_at(1, tick);
        REQUIRE(reverse.has_value());
        CHECK(reverse->index == static_cast<std::size_t>(3 - (ordinal % 4)));
    }

    // The coordinate a lane reports as upcoming is the one it goes on to play.
    auto input = prepared_buffer();
    auto output = prepared_buffer(player.contract().required_output_events(0));
    const midi::StepPlayerBlock block{
        .sample_start = {0},
        .tick_start = map.samples_to_ticks({0}),
        .sample_count = 1,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .tempo_map = &map,
        .playing = true,
        .transport_event = midi::StepPlayerTransportEvent::Continuous,
    };
    REQUIRE(player.process(input, output, block).complete);
    const auto upcoming = player.upcoming_coordinate(0);
    REQUIRE(upcoming.has_value());
    CHECK(upcoming->index == 1);
    CHECK(upcoming->step_tick.value == kStepTicks);
}

TEST_CASE("StepPlayer coordinate query names the step the lane actually fires",
          "[midi][step-player][coordinate]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(2);
    fixture.spec.lanes[1].direction = midi::StepPlayerDirection::PingPong;
    auto player = fixture.make();
    for (std::size_t lane = 0; lane < 2; ++lane)
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            // The pitch names the step, so a fired note identifies the index.
            step.pitch_offset = static_cast<std::int8_t>(index);
            REQUIRE(player.set_step(lane, index, step) == midi::StepPlayerError::None);
        }

    // Coordinates are read up front, before the walk has advanced at all: a
    // query that merely echoed playback state could not answer here.
    std::vector<std::size_t> predicted[2];
    for (std::size_t lane = 0; lane < 2; ++lane)
        for (std::int64_t ordinal = 0; ordinal < 10; ++ordinal) {
            const auto coordinate = player.coordinate_at(lane, {ordinal * kStepTicks});
            REQUIRE(coordinate.has_value());
            predicted[lane].push_back(coordinate->index);
        }

    const std::int32_t partitions[] = {256, 64, 512};
    const auto samples = map.ticks_to_samples({10 * kStepTicks}).value;
    const auto attacks = attacks_of(render(player, map, samples, partitions));

    for (std::size_t lane = 0; lane < 2; ++lane) {
        std::size_t ordinal = 0;
        for (const auto& attack : attacks) {
            if (attack.event.channel() != static_cast<std::uint8_t>(lane))
                continue;
            REQUIRE(ordinal < predicted[lane].size());
            const auto expected =
                static_cast<int>(fixture.spec.lanes[lane].base_note) +
                static_cast<int>(predicted[lane][ordinal]);
            INFO("lane " << lane << " ordinal " << ordinal);
            CHECK(static_cast<int>(attack.event.note()) == expected);
            ++ordinal;
        }
        // A lane that never spoke would pass the loop above vacuously.
        CHECK(ordinal == 10);
    }

    // A ping-pong lane's walk is not a modulo, so the two lanes must disagree —
    // otherwise the check above would hold for any index the query invented.
    CHECK(predicted[0] != predicted[1]);
}

TEST_CASE("StepPlayer coordinate query names the step a random walk fires",
          "[midi][step-player][coordinate]") {
    const auto map = constant_tempo_map();
    PlayerFixture<> fixture(2);
    fixture.spec.random_seed = 0x5eed;
    fixture.spec.lanes[1].direction = midi::StepPlayerDirection::Random;
    auto player = fixture.make();
    for (std::size_t lane = 0; lane < 2; ++lane)
        for (std::size_t index = 0; index < 4; ++index) {
            auto step = basic_step();
            // The pitch names the step, so a fired note identifies the index.
            step.pitch_offset = static_cast<std::int8_t>(index);
            REQUIRE(player.set_step(lane, index, step) == midi::StepPlayerError::None);
        }

    constexpr std::int64_t kOrdinals = 16;
    std::vector<std::size_t> predicted;
    for (std::int64_t ordinal = 0; ordinal < kOrdinals; ++ordinal) {
        const auto coordinate = player.coordinate_at(1, {ordinal * kStepTicks});
        REQUIRE(coordinate.has_value());
        predicted.push_back(coordinate->index);
    }

    // The random walk reads the grid coordinate rather than consuming RNG
    // state, so asking again — and out of order — must answer the same.
    for (std::int64_t ordinal = kOrdinals - 1; ordinal >= 0; --ordinal) {
        const auto repeat = player.coordinate_at(1, {ordinal * kStepTicks});
        REQUIRE(repeat.has_value());
        CHECK(repeat->index == predicted[static_cast<std::size_t>(ordinal)]);
    }

    const std::int32_t partitions[] = {256, 64, 512};
    const auto samples = map.ticks_to_samples({kOrdinals * kStepTicks}).value;
    const auto attacks = attacks_of(render(player, map, samples, partitions));

    std::size_t ordinal = 0;
    for (const auto& attack : attacks) {
        if (attack.event.channel() != 1)
            continue;
        REQUIRE(ordinal < predicted.size());
        const auto expected = static_cast<int>(fixture.spec.lanes[1].base_note) +
                              static_cast<int>(predicted[ordinal]);
        INFO("random ordinal " << ordinal);
        CHECK(static_cast<int>(attack.event.note()) == expected);
        ++ordinal;
    }
    // A lane that never spoke would pass the loop above vacuously.
    CHECK(ordinal == static_cast<std::size_t>(kOrdinals));

    // If the walk had degenerated to the forward modulo, agreeing with it
    // would say nothing about the random branch the query has to reproduce.
    bool differs = false;
    for (std::int64_t index = 0; index < kOrdinals; ++index)
        differs = differs || predicted[static_cast<std::size_t>(index)] !=
                                 static_cast<std::size_t>(index % 4);
    CHECK(differs);
}
