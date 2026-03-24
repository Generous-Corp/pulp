#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/midi.hpp>

using namespace pulp::midi;

TEST_CASE("MidiEvent factory methods", "[midi][message]") {
    SECTION("Note on") {
        auto evt = MidiEvent::note_on(0, 60, 100);
        REQUIRE(evt.is_note_on());
        REQUIRE_FALSE(evt.is_note_off());
        REQUIRE(evt.channel() == 0);
        REQUIRE(evt.note() == 60);
        REQUIRE(evt.velocity() == 100);
        REQUIRE(evt.size == 3);
    }

    SECTION("Note off") {
        auto evt = MidiEvent::note_off(1, 64, 0);
        REQUIRE(evt.is_note_off());
        REQUIRE_FALSE(evt.is_note_on());
        REQUIRE(evt.channel() == 1);
        REQUIRE(evt.note() == 64);
    }

    SECTION("Note on with velocity 0 is note off") {
        auto evt = MidiEvent::note_on(0, 60, 0);
        REQUIRE(evt.is_note_off());
        REQUIRE_FALSE(evt.is_note_on());
    }

    SECTION("Control change") {
        auto evt = MidiEvent::cc(2, 74, 127);
        REQUIRE(evt.is_cc());
        REQUIRE(evt.channel() == 2);
        REQUIRE(evt.cc_number() == 74);
        REQUIRE(evt.cc_value() == 127);
    }

    SECTION("Pitch bend") {
        auto evt = MidiEvent::pitch_bend(0, 8192);
        REQUIRE(evt.is_pitch_bend());
        REQUIRE(evt.channel() == 0);
    }

    SECTION("Program change") {
        auto evt = MidiEvent::program_change(3, 42);
        REQUIRE(evt.is_program_change());
        REQUIRE(evt.channel() == 3);
    }
}

TEST_CASE("MidiBuffer operations", "[midi][buffer]") {
    MidiBuffer buf;

    REQUIRE(buf.empty());
    REQUIRE(buf.size() == 0);

    buf.add(MidiEvent::note_on(0, 60, 100));
    buf.add(MidiEvent::note_on(0, 64, 100));
    buf.add(MidiEvent::note_on(0, 67, 100));

    REQUIRE(buf.size() == 3);
    REQUIRE_FALSE(buf.empty());

    SECTION("Iteration") {
        int count = 0;
        for (const auto& evt : buf) {
            REQUIRE(evt.is_note_on());
            ++count;
        }
        REQUIRE(count == 3);
    }

    SECTION("Sort by sample offset") {
        MidiBuffer sorted_buf;
        auto e1 = MidiEvent::note_on(0, 60, 100);
        e1.sample_offset = 200;
        auto e2 = MidiEvent::note_on(0, 64, 100);
        e2.sample_offset = 50;
        auto e3 = MidiEvent::note_on(0, 67, 100);
        e3.sample_offset = 100;

        sorted_buf.add(e1);
        sorted_buf.add(e2);
        sorted_buf.add(e3);
        sorted_buf.sort();

        REQUIRE(sorted_buf[0].sample_offset == 50);
        REQUIRE(sorted_buf[1].sample_offset == 100);
        REQUIRE(sorted_buf[2].sample_offset == 200);
    }

    SECTION("Clear") {
        buf.clear();
        REQUIRE(buf.empty());
    }
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
TEST_CASE("CoreMIDI system enumerates ports", "[midi][coremidi]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);

    // These may be empty if no MIDI devices are connected, but shouldn't crash
    auto inputs = system->enumerate_inputs();
    auto outputs = system->enumerate_outputs();

    // Just verify the calls succeed
    REQUIRE(true);
}
#endif
