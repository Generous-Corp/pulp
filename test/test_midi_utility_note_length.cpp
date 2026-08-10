#include "midi_utility_test_support.hpp"

#include <algorithm>
#include <vector>

TEST_CASE("Note length shaping orders retrigger releases before attacks",
          "[midi][utility][ordering]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    auto first = midi::MidiEvent::note_on(0, 60, 100);
    auto retrigger = midi::MidiEvent::note_on(0, 60, 110);
    first.sample_offset = 4;
    retrigger.sample_offset = 12;
    input.add(first);
    input.add(retrigger);

    midi::NoteLengthShaper<4> shaper({100});
    REQUIRE(shaper.process(input, output, {0}, 64).complete);
    REQUIRE(output.size() == 3);
    CHECK(output[0].is_note_on());
    CHECK(output[1].is_note_off());
    CHECK(output[2].is_note_on());
    CHECK(output[1].sample_offset == 12);
    CHECK(output[2].sample_offset == 12);
}

TEST_CASE("Note length scheduling is invariant to callback partitions",
          "[midi][utility][ordering][partition]") {
    struct ScheduledAttack {
        std::int64_t sample;
        std::uint8_t note;
        std::uint8_t velocity;
    };
    struct AbsoluteEvent {
        std::int64_t sample;
        bool attack;
        std::uint8_t channel;
        std::uint8_t note;
        std::uint8_t velocity;

        bool operator==(const AbsoluteEvent&) const = default;
    };

    constexpr std::array attacks{
        ScheduledAttack{5, 60, 100},
        ScheduledAttack{60, 64, 90},
        ScheduledAttack{78, 60, 110},
    };
    constexpr std::int64_t total_samples = 192;

    const auto render = [&](const auto& partitions) {
        midi::NoteLengthShaper<4> shaper({73});
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        NoteBalance balance;
        std::vector<AbsoluteEvent> rendered;
        std::int64_t block_start = 0;
        std::size_t partition_index = 0;

        while (block_start < total_samples) {
            const auto block_samples = static_cast<std::int32_t>(std::min<std::int64_t>(
                partitions[partition_index++ % partitions.size()], total_samples - block_start));
            input.clear();
            for (const auto& attack : attacks) {
                if (attack.sample < block_start ||
                    attack.sample >= block_start + block_samples)
                    continue;
                auto event = midi::MidiEvent::note_on(0, attack.note, attack.velocity);
                event.sample_offset = static_cast<std::int32_t>(attack.sample - block_start);
                REQUIRE(input.add(event));
            }

            REQUIRE(shaper.process(input, output, {block_start}, block_samples).complete);
            balance.feed(output);
            for (const auto& event : output) {
                REQUIRE(event.sample_offset >= 0);
                REQUIRE(event.sample_offset < block_samples);
                REQUIRE((event.is_note_on() || event.is_note_off()));
                rendered.push_back({block_start + event.sample_offset, event.is_note_on(),
                                    event.channel(), event.note(), event.velocity()});
            }
            block_start += block_samples;
        }

        REQUIRE(shaper.empty());
        REQUIRE(balance.balanced());
        return rendered;
    };

    constexpr std::array<std::int32_t, 1> fixed{64};
    constexpr std::array<std::int32_t, 5> irregular{17, 31, 5, 64, 9};
    const auto fixed_events = render(fixed);
    const auto irregular_events = render(irregular);
    const std::vector<AbsoluteEvent> expected{
        {5, true, 0, 60, 100},
        {60, true, 0, 64, 90},
        {78, false, 0, 60, 0},
        {78, true, 0, 60, 110},
        {133, false, 0, 64, 0},
        {151, false, 0, 60, 0},
    };

    REQUIRE(fixed_events == expected);
    REQUIRE(irregular_events == expected);
    REQUIRE(fixed_events == irregular_events);
}

TEST_CASE("Note length shaping treats velocity-zero note-ons as releases",
          "[midi][utility][lifecycle]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    midi::NoteLengthShaper<1> shaper({4096});
    input.add(midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(shaper.process(input, output, {0}, 64).complete);
    REQUIRE(output.size() == 1);

    input.clear();
    input.add(midi::MidiEvent::note_on(0, 60, 0));
    REQUIRE(shaper.process(input, output, {64}, 64).complete);
    CHECK(output.empty());
    REQUIRE(shaper.flush(output).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_note_off());

    SECTION("an unprocessed velocity-zero release clears suppression debt") {
        midi::NoteLengthShaper<1> invalid({0});
        auto zero_capacity = prepared_buffer(0);
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 61, 100));
        REQUIRE_FALSE(invalid.process(input, zero_capacity, {0}, 64).complete);
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 61, 0));
        REQUIRE_FALSE(invalid.process(input, zero_capacity, {64}, 64).complete);
        REQUIRE(invalid.replace_spec({8}, output).complete);
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 61, 100));
        REQUIRE(invalid.process(input, output, {128}, 64).complete);
        REQUIRE(output.size() == 2);
    }
}

TEST_CASE("Note length saturation never schedules a release before its attack",
          "[midi][utility][ordering][boundary]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    auto attack = midi::MidiEvent::note_on(0, 60, 100);
    attack.sample_offset = 10;
    input.add(attack);
    midi::NoteLengthShaper<1> shaper({64});
    REQUIRE(
        shaper.process(input, output, {std::numeric_limits<std::int64_t>::max() - 5}, 32).complete);
    REQUIRE(output.size() == 2);
    CHECK(output[0].is_note_on());
    CHECK(output[1].is_note_off());
    CHECK(output[0].sample_offset == 10);
    CHECK(output[1].sample_offset == 10);
    NoteBalance balance;
    balance.feed(output);
    CHECK(balance.balanced());
}

TEST_CASE("Note length lifecycle operations leave no positive note balance",
          "[midi][utility][lifecycle]") {
    for (const std::string_view transition : {"stop", "seek", "loop", "reset"}) {
        CAPTURE(transition);
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        midi::NoteLengthShaper<2> shaper({4096});
        NoteBalance balance;
        input.add(midi::MidiEvent::note_on(1, 64, 100));
        balance.feed(input);
        REQUIRE(shaper.process(input, output, {0}, 64).complete);
        balance = {};
        balance.feed(output);
        const auto report = transition == "reset" ? shaper.reset(output) : shaper.flush(output);
        REQUIRE(report.complete);
        balance.feed(output);
        CHECK(balance.balanced());
    }

    SECTION("spec swap flushes the old note before adopting the new length") {
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        midi::NoteLengthShaper<2> shaper({4096});
        NoteBalance balance;
        input.add(midi::MidiEvent::note_on(0, 60, 100));
        shaper.process(input, output, {0}, 64);
        balance.feed(output);
        REQUIRE(shaper.replace_spec({8}, output).complete);
        balance.feed(output);
        CHECK(balance.balanced());
    }

    SECTION("a valid spec repairs an invalid construction") {
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        midi::NoteLengthShaper<2> shaper({0});
        REQUIRE_FALSE(shaper.valid());
        REQUIRE(shaper.replace_spec({8}, output).complete);
        REQUIRE(shaper.valid());

        input.add(midi::MidiEvent::note_on(0, 60, 100));
        REQUIRE(shaper.process(input, output, {0}, 64).complete);
        REQUIRE(output.size() == 2);
        CHECK(output[0].is_note_on());
        CHECK(output[1].is_note_off());
        CHECK(output[1].sample_offset == 8);
    }
}

TEST_CASE("Note length overflow remains balanced and processing remains allocation free",
          "[midi][utility][overflow][rt-safety]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    input.add(midi::MidiEvent::note_on(0, 60, 100));
    input.add(midi::MidiEvent::note_on(0, 61, 100));
    midi::NoteLengthShaper<1> shaper({4096});
    NoteBalance balance;

    pulp::test::RtAllocationProbe allocations;
    const auto report = shaper.process(input, output, {0}, 64);
    CHECK_FALSE(allocations.saw_allocation());
    REQUIRE(report.complete);
    balance.feed(output);
    REQUIRE(shaper.flush(output).complete);
    balance.feed(output);
    CHECK(balance.balanced());

    SECTION("a failed retrigger release suppresses the new attack") {
        auto retriggers = prepared_buffer();
        auto one_event_output = prepared_buffer(1);
        retriggers.add(midi::MidiEvent::note_on(0, 70, 100));
        retriggers.add(midi::MidiEvent::note_on(0, 70, 110));
        midi::NoteLengthShaper<2> retrigger_shaper({4096});
        NoteBalance retrigger_balance;
        CHECK_FALSE(retrigger_shaper.process(retriggers, one_event_output, {0}, 64).complete);
        REQUIRE(one_event_output.size() == 1);
        retrigger_balance.feed(one_event_output);
        REQUIRE(retrigger_shaper.flush(output).complete);
        retrigger_balance.feed(output);
        CHECK(retrigger_balance.balanced());
    }

    SECTION("release debt blocks later flush work until it is emitted") {
        auto attacks = prepared_buffer();
        attacks.add(midi::MidiEvent::note_on(0, 60, 100));
        attacks.add(midi::MidiEvent::note_on(0, 61, 100));
        midi::NoteLengthShaper<1> debt_shaper({4096});
        NoteBalance debt_balance;
        REQUIRE(debt_shaper.process(attacks, output, {0}, 64).complete);
        debt_balance.feed(output);

        auto release = prepared_buffer();
        release.add(midi::MidiEvent::note_off(0, 61));
        auto zero_capacity = prepared_buffer(0);
        CHECK_FALSE(debt_shaper.process(release, zero_capacity, {64}, 64).complete);
        CHECK_FALSE(debt_shaper.flush(zero_capacity).complete);
        REQUIRE(debt_shaper.flush(output).complete);
        debt_balance.feed(output);
        CHECK(debt_balance.balanced());
    }

    SECTION("fail-open releases retain forwarded and suppressed attack order") {
        auto one_event_output = prepared_buffer(1);
        auto events = prepared_buffer();

        {
            midi::NoteLengthShaper<0> ordered({4096});
            events.add(midi::MidiEvent::note_on(0, 72, 100));
            REQUIRE(ordered.process(events, one_event_output, {0}, 64).complete);
            events.clear();
            events.add(midi::MidiEvent::cc(0, 7, 100));
            events.add(midi::MidiEvent::note_on(0, 72, 110));
            CHECK_FALSE(ordered.process(events, one_event_output, {64}, 64).complete);
            events.clear();
            events.add(midi::MidiEvent::note_off(0, 72));
            REQUIRE(ordered.process(events, one_event_output, {128}, 64).complete);
            REQUIRE(one_event_output.size() == 1);
            CHECK(one_event_output[0].is_note_off());
            REQUIRE(ordered.process(events, one_event_output, {192}, 64).complete);
            CHECK(one_event_output.empty());
        }

        {
            midi::NoteLengthShaper<0> ordered({4096});
            events.clear();
            events.add(midi::MidiEvent::cc(0, 7, 100));
            events.add(midi::MidiEvent::note_on(0, 73, 100));
            CHECK_FALSE(ordered.process(events, one_event_output, {0}, 64).complete);
            events.clear();
            events.add(midi::MidiEvent::note_on(0, 73, 110));
            REQUIRE(ordered.process(events, one_event_output, {64}, 64).complete);
            events.clear();
            events.add(midi::MidiEvent::note_off(0, 73));
            REQUIRE(ordered.process(events, one_event_output, {128}, 64).complete);
            CHECK(one_event_output.empty());
            REQUIRE(ordered.process(events, one_event_output, {192}, 64).complete);
            REQUIRE(one_event_output.size() == 1);
            CHECK(one_event_output[0].is_note_off());
        }
    }
}

TEST_CASE("Note length fail-open ownership exceeds uint16 depth without losing balance",
          "[midi][utility][overflow][amplification]") {
    constexpr std::size_t attacks = 65536;
    auto input = prepared_buffer(attacks);
    auto output = prepared_buffer(attacks);
    for (std::size_t i = 0; i < attacks; ++i)
        REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));
    midi::NoteLengthShaper<0> shaper({4096});
    const auto process_report = shaper.process(input, output, {0}, 64);
    REQUIRE(process_report.complete);
    CHECK(process_report.emitted == attacks);
    CHECK(process_report.emitted <= input.size() * shaper.contract().maximum_event_amplification);
    NoteBalance balance;
    balance.feed(output);
    const auto flush_report = shaper.flush(output);
    REQUIRE(flush_report.complete);
    CHECK(flush_report.emitted == attacks);
    balance.feed(output);
    CHECK(balance.balanced());
}
