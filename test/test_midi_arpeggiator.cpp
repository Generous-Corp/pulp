#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/arpeggiator.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace pulp;

constexpr std::uint64_t kSampleRate = 48'000;
constexpr std::int64_t kSixteenthSamples = 6'000;

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

template <typename Arp>
std::vector<AbsoluteMidiEvent>
render(Arp& arp, const timebase::CompiledTempoMap& tempo_map, std::int64_t total_samples,
       std::span<const std::int32_t> partitions, std::span<const AbsoluteMidiEvent> input_events,
       std::span<const std::pair<std::int64_t, midi::ArpeggiatorTransportEvent>> transitions = {},
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
        auto output = prepared_buffer(arp.contract().required_output_events(input.size()));
        auto transition = midi::ArpeggiatorTransportEvent::Continuous;
        if (transition_index < transitions.size() &&
            transitions[transition_index].first == block_start) {
            transition = transitions[transition_index++].second;
        }
        const bool playing = transition != midi::ArpeggiatorTransportEvent::Stopped;
        const midi::ArpeggiatorBlock block{
            .sample_start = {block_start},
            .tick_start = tempo_map.samples_to_ticks({block_start}),
            .sample_count = block_size,
            .sample_rate = {kSampleRate, 1},
            .tempo_bpm = 120.0,
            .tempo_map = &tempo_map,
            .playing = playing,
            .transport_event = transition,
        };
        const auto report = arp.process(input, output, block);
        if (!allow_incomplete)
            REQUIRE(report.complete);
        for (const auto& event : output)
            result.push_back({block_start + event.sample_offset, event});
        block_start += block_size;
    }
    return result;
}

std::vector<std::uint8_t> attacks(const std::vector<AbsoluteMidiEvent>& events) {
    std::vector<std::uint8_t> result;
    for (const auto& event : events)
        if (event.event.is_note_on() && event.event.velocity() != 0)
            result.push_back(event.event.note());
    return result;
}

std::vector<AbsoluteMidiEvent> chord_at_zero(std::initializer_list<int> notes) {
    std::vector<AbsoluteMidiEvent> result;
    for (const auto note : notes)
        result.push_back({0, midi::MidiEvent::note_on(0, static_cast<std::uint8_t>(note), 100)});
    return result;
}

} // namespace

TEST_CASE("Arpeggiator declares bounded state, generation, and valid musical domains",
          "[midi][arpeggiator][contract]") {
    using Arp = midi::Arpeggiator<3, 2, 8, 8>;
    STATIC_CHECK(Arp::contract().maximum_held_notes == 3);
    STATIC_CHECK(Arp::contract().maximum_pattern_notes == 6);
    STATIC_CHECK(Arp::contract().maximum_steps_per_block == 8);
    STATIC_CHECK(Arp::contract().maximum_generated_events_per_block == 102);
    STATIC_CHECK(Arp::contract().required_output_events(7) == 109);

    midi::ArpeggiatorSpec spec;
    CHECK(Arp::valid_spec(spec));
    spec.rate = {0};
    CHECK_FALSE(Arp::valid_spec(spec));
    spec = {};
    spec.octave_count = 3;
    CHECK_FALSE(Arp::valid_spec(spec));
    spec = {};
    spec.gate = {2, 1};
    CHECK_FALSE(Arp::valid_spec(spec));
    spec = {};
    spec.swing = {1, 1};
    CHECK_FALSE(Arp::valid_spec(spec));
}

TEST_CASE("Arpeggiator order modes use exact held-note and octave ordering",
          "[midi][arpeggiator][ordering]") {
    const auto map = constant_tempo_map();
    constexpr std::array partitions{127, 509, 64};

    auto check = [&](midi::ArpeggiatorOrder order, std::initializer_list<int> played,
                     std::initializer_list<int> expected) {
        midi::ArpeggiatorSpec spec;
        spec.order = order;
        spec.gate = {1, 2};
        midi::Arpeggiator<> arp(spec);
        const auto events = chord_at_zero(played);
        const auto output = render(arp, map, kSixteenthSamples * 4 + 1, partitions, events);
        const auto actual = attacks(output);
        REQUIRE(actual.size() >= expected.size());
        CHECK(std::equal(expected.begin(), expected.end(), actual.begin()));
    };

    check(midi::ArpeggiatorOrder::Up, {67, 60, 64}, {60, 64, 67, 60});
    check(midi::ArpeggiatorOrder::Down, {67, 60, 64}, {67, 64, 60, 67});
    check(midi::ArpeggiatorOrder::UpDown, {67, 60, 64}, {60, 64, 67, 64});
    check(midi::ArpeggiatorOrder::AsPlayed, {67, 60, 64}, {67, 60, 64, 67});

    SECTION("octave expansion stays in MIDI range") {
        midi::ArpeggiatorSpec spec;
        spec.octave_count = 4;
        midi::Arpeggiator<> arp(spec);
        const auto output =
            render(arp, map, kSixteenthSamples * 4 + 1, partitions, chord_at_zero({120, 127}));
        CHECK(attacks(output) == std::vector<std::uint8_t>{120, 127, 120, 127, 120});
    }

    SECTION("chord mode emits the whole ordered expansion per boundary") {
        midi::ArpeggiatorSpec spec;
        spec.order = midi::ArpeggiatorOrder::Chord;
        spec.octave_count = 2;
        midi::Arpeggiator<> arp(spec);
        const auto output = render(arp, map, 1, partitions, chord_at_zero({64, 60}));
        CHECK(attacks(output) == std::vector<std::uint8_t>{60, 64, 72, 76});
    }
}

TEST_CASE("Arpeggiator random order is seeded and block-partition invariant",
          "[midi][arpeggiator][determinism][partition]") {
    const auto map = constant_tempo_map();
    midi::ArpeggiatorSpec spec;
    spec.order = midi::ArpeggiatorOrder::Random;
    spec.random_seed = 0x4a93d21u;
    spec.swing = timebase::kTripletSwing;
    spec.gate = {3, 4};
    constexpr std::array small{64, 127, 257};
    constexpr std::array large{2048, 333};
    const auto input = chord_at_zero({60, 64, 67, 71});
    midi::Arpeggiator<> first(spec);
    midi::Arpeggiator<> second(spec);
    const auto a = render(first, map, 48'001, small, input);
    const auto b = render(second, map, 48'001, large, input);
    REQUIRE(a.size() == b.size());
    for (std::size_t index = 0; index < a.size(); ++index)
        CHECK(a[index].identity() == b[index].identity());
}

TEST_CASE("Arpeggiator gate and repeated-note policy define ties and retriggers",
          "[midi][arpeggiator][gate][ordering]") {
    const auto map = constant_tempo_map();
    constexpr std::array<std::int32_t, 1> blocks{static_cast<std::int32_t>(kSixteenthSamples)};
    const auto held = chord_at_zero({60});

    midi::ArpeggiatorSpec retrigger_spec;
    retrigger_spec.gate = {1, 1};
    retrigger_spec.repeated_note = midi::ArpeggiatorRepeatedNotePolicy::Retrigger;
    midi::Arpeggiator<> retrigger(retrigger_spec);
    const auto retriggered = render(retrigger, map, kSixteenthSamples * 2 + 1, blocks, held);
    REQUIRE(retriggered.size() >= 5);
    CHECK(retriggered[1].sample == kSixteenthSamples);
    CHECK(retriggered[1].event.is_note_off());
    CHECK(retriggered[2].sample == kSixteenthSamples);
    CHECK(retriggered[2].event.is_note_on());

    midi::ArpeggiatorSpec tie_spec = retrigger_spec;
    tie_spec.repeated_note = midi::ArpeggiatorRepeatedNotePolicy::Tie;
    midi::Arpeggiator<> tied(tie_spec);
    const auto tied_output = render(tied, map, kSixteenthSamples * 2 + 1, blocks, held);
    CHECK(attacks(tied_output) == std::vector<std::uint8_t>{60});
}

TEST_CASE("Arpeggiator boundary order is explicit when input lands on a step",
          "[midi][arpeggiator][ordering][boundary]") {
    const auto map = constant_tempo_map();
    constexpr std::array<std::int32_t, 2> blocks{static_cast<std::int32_t>(kSixteenthSamples),
                                                 static_cast<std::int32_t>(kSixteenthSamples)};
    const std::array input{
        AbsoluteMidiEvent{0, midi::MidiEvent::note_on(0, 60, 100)},
        AbsoluteMidiEvent{kSixteenthSamples, midi::MidiEvent::note_off(0, 60)},
        AbsoluteMidiEvent{kSixteenthSamples, midi::MidiEvent::note_on(0, 62, 100)},
    };

    midi::ArpeggiatorSpec input_first_spec;
    input_first_spec.boundary_order = midi::ArpeggiatorBoundaryOrder::InputBeforeStep;
    midi::Arpeggiator<> input_first(input_first_spec);
    const auto first = render(input_first, map, kSixteenthSamples + 1, blocks, input);
    CHECK(attacks(first) == std::vector<std::uint8_t>{60, 62});

    midi::ArpeggiatorSpec step_first_spec = input_first_spec;
    step_first_spec.boundary_order = midi::ArpeggiatorBoundaryOrder::StepBeforeInput;
    midi::Arpeggiator<> step_first(step_first_spec);
    const auto second = render(step_first, map, kSixteenthSamples + 1, blocks, input);
    CHECK(attacks(second) == std::vector<std::uint8_t>{60});
    REQUIRE(second.size() == 2);
    CHECK(second[0].event.is_note_on());
    CHECK(second[1].event.is_note_off());
    CHECK(second[0].sample == kSixteenthSamples);
    CHECK(second[1].sample == kSixteenthSamples);
}

TEST_CASE("Arpeggiator held ownership remains balanced through suppression and latch phrases",
          "[midi][arpeggiator][ownership][latch]") {
    const auto map = constant_tempo_map();
    constexpr std::array<std::int32_t, 1> blocks{static_cast<std::int32_t>(kSixteenthSamples)};

    SECTION("an unstarted attack and its release cannot retire an accepted note") {
        midi::Arpeggiator<1> arp;
        const std::array input{
            AbsoluteMidiEvent{0, midi::MidiEvent::note_on(0, 60, 100)},
            AbsoluteMidiEvent{1, midi::MidiEvent::note_on(0, 62, 100)},
            AbsoluteMidiEvent{2, midi::MidiEvent::note_off(0, 62)},
            AbsoluteMidiEvent{kSixteenthSamples + 1, midi::MidiEvent::note_off(0, 60)},
        };
        const auto output = render(arp, map, kSixteenthSamples * 2, blocks, input, {}, true);
        CHECK(attacks(output) == std::vector<std::uint8_t>{60, 60});
        EventLedger ledger;
        for (const auto& event : output)
            ledger.feed(event);
        CHECK(ledger.balanced());
    }

    SECTION("latch holds a released chord and atomically replaces it") {
        midi::ArpeggiatorSpec spec;
        spec.latch = true;
        spec.order = midi::ArpeggiatorOrder::Chord;
        midi::Arpeggiator<> arp(spec);
        const std::array input{
            AbsoluteMidiEvent{0, midi::MidiEvent::note_on(0, 60, 100)},
            AbsoluteMidiEvent{1, midi::MidiEvent::note_off(0, 60)},
            AbsoluteMidiEvent{kSixteenthSamples, midi::MidiEvent::note_on(0, 65, 100)},
        };
        const auto output = render(arp, map, kSixteenthSamples * 2 + 1, blocks, input);
        CHECK(attacks(output) == std::vector<std::uint8_t>{60, 65, 65});
        CHECK(std::none_of(output.begin(), output.end(), [](const AbsoluteMidiEvent& event) {
            return event.sample > kSixteenthSamples && event.event.is_note_on() &&
                   event.event.note() == 60;
        }));
    }

    SECTION("a replacement attack survives old-phrase release debt") {
        midi::ArpeggiatorSpec spec;
        spec.latch = true;
        spec.order = midi::ArpeggiatorOrder::Chord;
        midi::Arpeggiator<> arp(spec);
        auto phrase = prepared_buffer();
        REQUIRE(phrase.add(midi::MidiEvent::note_on(0, 60, 100)));
        REQUIRE(phrase.add(midi::MidiEvent::note_on(0, 64, 100)));
        auto release_60 = midi::MidiEvent::note_off(0, 60);
        release_60.sample_offset = 1;
        REQUIRE(phrase.add(release_60));
        auto release_64 = midi::MidiEvent::note_off(0, 64);
        release_64.sample_offset = 1;
        REQUIRE(phrase.add(release_64));
        auto output = prepared_buffer();
        const midi::ArpeggiatorBlock first{
            .sample_start = {0},
            .tick_start = {0},
            .sample_count = 128,
            .sample_rate = {kSampleRate, 1},
            .tempo_bpm = 120.0,
            .tempo_map = &map,
            .playing = true,
            .transport_event = midi::ArpeggiatorTransportEvent::Started,
        };
        REQUIRE(arp.process(phrase, output, first).complete);
        REQUIRE(arp.sounding_note_count() == 2);
        REQUIRE(arp.physical_note_count() == 0);

        auto replacement = prepared_buffer();
        REQUIRE(replacement.add(midi::MidiEvent::note_on(0, 65, 100)));
        auto full_output = prepared_buffer(1);
        const midi::ArpeggiatorBlock second{
            .sample_start = {128},
            .tick_start = {static_cast<std::int64_t>(
                std::llround(map.fractional_samples_to_ticks(128.0L)))},
            .sample_count = 128,
            .sample_rate = {kSampleRate, 1},
            .tempo_bpm = 120.0,
            .tempo_map = &map,
            .playing = true,
            .transport_event = midi::ArpeggiatorTransportEvent::Continuous,
        };
        const auto report = arp.process(replacement, full_output, second);
        CHECK_FALSE(report.complete);
        CHECK(report.deferred == 1);
        CHECK(arp.physical_note_count() == 1);
        CHECK(arp.held_note_count() == 1);

        auto drain = prepared_buffer();
        REQUIRE(arp.flush_transport(drain).complete);
        REQUIRE(drain.size() == 1);
        CHECK(drain[0].is_note_off());
        CHECK(drain[0].note() == 64);
    }
}

TEST_CASE("Arpeggiator flushes sounding notes on stop seek loop and hot swap",
          "[midi][arpeggiator][transport][lifecycle]") {
    const auto map = constant_tempo_map();
    auto input = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    auto output = prepared_buffer();
    midi::ArpeggiatorSpec spec;
    spec.gate = {1, 1};
    midi::Arpeggiator<> arp(spec);

    auto process = [&](std::int64_t sample, midi::ArpeggiatorTransportEvent event, bool playing,
                       const midi::MidiBuffer& events) {
        const midi::ArpeggiatorBlock block{
            .sample_start = {sample},
            .tick_start = map.samples_to_ticks({sample}),
            .sample_count = 128,
            .sample_rate = {kSampleRate, 1},
            .tempo_bpm = 120.0,
            .tempo_map = &map,
            .playing = playing,
            .transport_event = event,
        };
        return arp.process(events, output, block);
    };

    REQUIRE(process(0, midi::ArpeggiatorTransportEvent::Started, true, input).complete);
    REQUIRE(output.size() == 1);
    auto empty = prepared_buffer();

    for (const auto event :
         {midi::ArpeggiatorTransportEvent::Stopped, midi::ArpeggiatorTransportEvent::Seeked,
          midi::ArpeggiatorTransportEvent::LoopWrapped}) {
        REQUIRE(
            process(128, event, event != midi::ArpeggiatorTransportEvent::Stopped, empty).complete);
        REQUIRE_FALSE(output.empty());
        CHECK(output[0].is_note_off());
        CHECK(output[0].sample_offset == 0);
        REQUIRE(process(0, midi::ArpeggiatorTransportEvent::Started, true, empty).complete);
        REQUIRE_FALSE(output.empty());
        CHECK(output[output.size() - 1].is_note_on());
    }

    auto replacement = spec;
    replacement.order = midi::ArpeggiatorOrder::Down;
    REQUIRE(arp.replace_spec(replacement, output).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_note_off());
    CHECK(arp.spec().order == midi::ArpeggiatorOrder::Down);

    auto invalid = replacement;
    invalid.rate = {0};
    CHECK_FALSE(arp.replace_spec(invalid, output).complete);
    CHECK(arp.spec().order == midi::ArpeggiatorOrder::Down);
}

TEST_CASE("Arpeggiator retains release debt when output capacity is exhausted",
          "[midi][arpeggiator][overflow][ownership]") {
    auto input = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    midi::Arpeggiator<> arp;
    auto one_event = prepared_buffer(1);
    const auto map = constant_tempo_map();
    midi::ArpeggiatorBlock block{
        .sample_start = {0},
        .tick_start = {0},
        .sample_count = static_cast<std::int32_t>(kSixteenthSamples + 1),
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .tempo_map = &map,
        .playing = true,
        .transport_event = midi::ArpeggiatorTransportEvent::Started,
    };
    const auto report = arp.process(input, one_event, block);
    CHECK_FALSE(report.complete);
    REQUIRE(one_event.size() == 1);
    CHECK(one_event[0].is_note_on());

    auto drain = prepared_buffer();
    const auto flush = arp.flush_transport(drain);
    REQUIRE(flush.complete);
    REQUIRE(drain.size() == 1);
    CHECK(drain[0].is_note_off());
    EventLedger ledger;
    ledger.feed({0, one_event[0]});
    ledger.feed({kSixteenthSamples, drain[0]});
    CHECK(ledger.balanced());
}

TEST_CASE("Arpeggiator preserves non-note MIDI and exact sidecars",
          "[midi][arpeggiator][passthrough]") {
    midi::UmpBuffer input_ump;
    midi::UmpBuffer output_ump;
    input_ump.reserve(1);
    output_ump.reserve(1);
    input_ump.set_realtime_capacity_limit(true);
    output_ump.set_realtime_capacity_limit(true);

    auto input = prepared_buffer();
    auto output = prepared_buffer();
    input.attach_ump(&input_ump);
    output.attach_ump(&output_ump);
    REQUIRE(input.add(midi::MidiEvent::cc(3, 17, 91)));
    const std::array<std::uint8_t, 4> sysex{0xf0, 0x7d, 0x23, 0xf7};
    REQUIRE(input.add_sysex_copy(sysex.data(), sysex.size(), 9, 0.5));
    const auto packet = midi::UmpPacket::note_on_2(2, 3, 67, 0xbeef);
    REQUIRE(input_ump.add(packet, 11));

    const auto map = constant_tempo_map();
    midi::Arpeggiator<> arp;
    const midi::ArpeggiatorBlock block{
        .sample_start = {0},
        .tick_start = {0},
        .sample_count = 128,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .tempo_map = &map,
        .playing = true,
        .transport_event = midi::ArpeggiatorTransportEvent::Started,
    };
    REQUIRE(arp.process(input, output, block).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_cc());
    CHECK(output[0].channel() == 3);
    CHECK(output[0].cc_number() == 17);
    CHECK(output[0].cc_value() == 91);
    REQUIRE(output.sysex().size() == 1);
    CHECK(output.sysex()[0].data == std::vector<std::uint8_t>(sysex.begin(), sysex.end()));
    CHECK(output.sysex()[0].sample_offset == 9);
    REQUIRE(output.ump() != nullptr);
    REQUIRE(output.ump()->size() == 1);
    CHECK((*output.ump())[0].packet.words == packet.words);
    CHECK((*output.ump())[0].sample_offset == 11);
}

TEST_CASE("Arpeggiator process path allocates nothing after capacity preparation",
          "[midi][arpeggiator][rt-safety]") {
    auto input = prepared_buffer();
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    REQUIRE(input.add(midi::MidiEvent::note_on(0, 64, 100)));
    auto output = prepared_buffer();
    const auto map = constant_tempo_map();
    midi::ArpeggiatorSpec spec;
    spec.order = midi::ArpeggiatorOrder::Chord;
    midi::Arpeggiator<> arp(spec);
    const midi::ArpeggiatorBlock block{
        .sample_start = {0},
        .tick_start = {0},
        .sample_count = 128,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .tempo_map = &map,
        .playing = true,
        .transport_event = midi::ArpeggiatorTransportEvent::Started,
    };

    pulp::test::RtAllocationProbe allocations;
    const auto report = arp.process(input, output, block);
    CHECK_FALSE(allocations.saw_allocation());
    REQUIRE(report.complete);
    CHECK(output.size() == 2);
}

TEST_CASE("Arpeggiator rejects unordered or invalid blocks without emitting stale output",
          "[midi][arpeggiator][contract][failure]") {
    auto input = prepared_buffer();
    auto later = midi::MidiEvent::note_on(0, 60, 100);
    later.sample_offset = 10;
    auto earlier = midi::MidiEvent::note_on(0, 64, 100);
    earlier.sample_offset = 5;
    REQUIRE(input.add(later));
    REQUIRE(input.add(earlier));
    auto output = prepared_buffer();
    REQUIRE(output.add(midi::MidiEvent::note_on(0, 1, 1)));
    midi::Arpeggiator<> arp;
    const midi::ArpeggiatorBlock block{
        .sample_start = {0},
        .tick_start = {0},
        .sample_count = 128,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .playing = true,
    };
    CHECK_FALSE(arp.process(input, output, block).complete);
    CHECK(output.empty());
}

TEST_CASE("Arpeggiator saturates extreme host timing without signed overflow",
          "[midi][arpeggiator][transport][limits]") {
    STATIC_REQUIRE_FALSE(midi::arpeggiator_detail::distance_exceeds(-1, 1, 2));
    STATIC_REQUIRE(midi::arpeggiator_detail::distance_exceeds(
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), 2));

    midi::Arpeggiator<> discontinuity_arp;
    auto held = prepared_buffer();
    REQUIRE(held.add(midi::MidiEvent::note_on(0, 60, 100)));
    auto output = prepared_buffer();
    const midi::ArpeggiatorBlock started{
        .sample_start = {0},
        .tick_start = {0},
        .sample_count = 128,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .playing = true,
        .transport_event = midi::ArpeggiatorTransportEvent::Started,
    };
    REQUIRE(discontinuity_arp.process(held, output, started).complete);
    REQUIRE(output.size() == 1);
    auto empty = prepared_buffer();
    const midi::ArpeggiatorBlock jumped{
        .sample_start = {128},
        .tick_start = {std::numeric_limits<std::int64_t>::min()},
        .sample_count = 1,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 120.0,
        .playing = true,
        .transport_event = midi::ArpeggiatorTransportEvent::Continuous,
    };
    REQUIRE(discontinuity_arp.process(empty, output, jumped).complete);
    REQUIRE_FALSE(output.empty());
    CHECK(output[0].is_note_off());

    midi::ArpeggiatorSpec unit_tick_spec;
    unit_tick_spec.rate = {1};
    midi::Arpeggiator<> minimum_tick_arp(unit_tick_spec);
    const midi::ArpeggiatorBlock minimum_tick{
        .sample_start = {0},
        .tick_start = {std::numeric_limits<std::int64_t>::min()},
        .sample_count = 1,
        .sample_rate = {kSampleRate, 1},
        .tempo_bpm = 480.0,
        .playing = true,
        .transport_event = midi::ArpeggiatorTransportEvent::Started,
    };
    const auto minimum_report = minimum_tick_arp.process(held, output, minimum_tick);
    CHECK_FALSE(minimum_report.complete);
    CHECK(minimum_report.dropped > 0);

    midi::Arpeggiator<> projection_arp;
    auto input = prepared_buffer();
    const auto process = [&](std::int64_t sample, std::int64_t tick) {
        const midi::ArpeggiatorBlock block{
            .sample_start = {sample},
            .tick_start = {tick},
            .sample_count = 1,
            .sample_rate = {1, std::numeric_limits<std::uint64_t>::max()},
            .tempo_bpm = std::numeric_limits<double>::max(),
            .playing = false,
            .transport_event = midi::ArpeggiatorTransportEvent::Continuous,
        };
        return projection_arp.process(input, output, block);
    };

    REQUIRE(process(0, std::numeric_limits<std::int64_t>::max()).complete);
    REQUIRE(process(1, std::numeric_limits<std::int64_t>::min()).complete);
    CHECK(output.empty());
}
