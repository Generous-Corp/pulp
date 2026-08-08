#include "midi_utility_test_support.hpp"

TEST_CASE("Monophonic priorities maintain balanced release-before-attack transitions",
          "[midi][utility][mono]") {
    for (const auto priority : {midi::MonophonicPriority::Low, midi::MonophonicPriority::High,
                                midi::MonophonicPriority::Last}) {
        auto input = prepared_buffer();
        auto output = prepared_buffer();
        midi::MonophonicNoteSelector selector({priority, true, 0.01});
        auto high = midi::MidiEvent::note_on(0, 72, 100);
        auto low = midi::MidiEvent::note_on(0, 48, 100);
        low.sample_offset = 7;
        input.add(high);
        input.add(low);
        REQUIRE(selector.process(input, output).complete);
        REQUIRE(output.size() >= 1);
        const auto expected = priority == midi::MonophonicPriority::High ? 72 : 48;
        CHECK(selector.pitch_state().note == expected);
        if (output.size() == 3) {
            CHECK(output[1].is_note_off());
            CHECK(output[2].is_note_on());
            CHECK(output[1].sample_offset == output[2].sample_offset);
        }
        NoteBalance balance;
        balance.feed(output);
        REQUIRE(selector.flush(output).complete);
        balance.feed(output);
        CHECK(balance.balanced());
    }
}
TEST_CASE("Monophonic selection treats velocity-zero note-ons as releases",
          "[midi][utility][mono][lifecycle]") {
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    midi::MonophonicNoteSelector selector;
    input.add(midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(selector.process(input, output).complete);
    REQUIRE(selector.pitch_state().active);

    input.clear();
    input.add(midi::MidiEvent::note_on(0, 60, 0));
    REQUIRE(selector.process(input, output).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_note_off());
    CHECK_FALSE(selector.pitch_state().active);
}

TEST_CASE("Last-note priority rebases deterministically across serial rollover",
          "[midi][utility][mono][ordering]") {
    STATIC_CHECK_FALSE(midi::monophonic_serial_capacity_supported<std::uint8_t>);
    STATIC_CHECK(midi::monophonic_serial_capacity_supported<std::uint16_t>);
    midi::BasicMonophonicNoteSelector<std::uint16_t> selector(
        {midi::MonophonicPriority::Last, true, 0.0});
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    input.add(midi::MidiEvent::note_on(0, 40, 100));
    REQUIRE(selector.process(input, output).complete);
    for (std::uint32_t i = 0; i < std::numeric_limits<std::uint16_t>::max() - 1; ++i) {
        input.clear();
        input.add(midi::MidiEvent::note_on(0, 60, 100));
        REQUIRE(selector.process(input, output).complete);
        input.clear();
        input.add(midi::MidiEvent::note_off(0, 60));
        REQUIRE(selector.process(input, output).complete);
    }
    input.clear();
    input.add(midi::MidiEvent::note_on(0, 80, 100));
    REQUIRE(selector.process(input, output).complete);
    CHECK(selector.pitch_state().note == 80);
}

TEST_CASE("Monophonic held ownership remains exact beyond uint16 depth",
          "[midi][utility][mono][overflow]") {
    constexpr std::size_t attacks = 65'536;
    auto input = prepared_buffer(attacks);
    auto output = prepared_buffer(2);
    for (std::size_t i = 0; i < attacks; ++i)
        REQUIRE(input.add(midi::MidiEvent::note_on(0, 60, 100)));

    midi::MonophonicNoteSelector selector;
    NoteBalance balance;
    const auto attack_report = selector.process(input, output);
    REQUIRE(attack_report.complete);
    REQUIRE(attack_report.emitted == 1);
    balance.feed(output);

    input.clear();
    for (std::size_t i = 0; i < attacks - 1; ++i)
        REQUIRE(input.add(midi::MidiEvent::note_off(0, 60)));
    REQUIRE(selector.process(input, output).complete);
    CHECK(output.empty());
    CHECK(selector.pitch_state().active);

    input.clear();
    REQUIRE(input.add(midi::MidiEvent::note_off(0, 60)));
    REQUIRE(selector.process(input, output).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].is_note_off());
    balance.feed(output);
    CHECK(balance.balanced());
}
