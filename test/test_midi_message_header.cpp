// Regression guard for the "compiles on libc++, breaks on libstdc++" bug class
// at Pulp's CHOC boundary (see pulp/midi/message.hpp).
//
// choc_MIDI.h's Message::operator== calls `memcmp` unqualified and includes no
// string header. The call is type-dependent, so the name can only be found by
// ordinary lookup at the point of the template's DEFINITION — i.e. whatever was
// visible when choc_MIDI.h was parsed. pulp/midi/message.hpp is what makes that
// declaration visible, and this file proves it, by including that header FIRST
// and then instantiating operator==.
//
// INCLUDE ORDER IS THE TEST. pulp/midi/message.hpp must stay in its own leading
// block: if Catch2 (or anything else that drags in <string>) is included first,
// libstdc++ has already declared ::memcmp and the guard silently goes vacuous —
// which is exactly why only some translation units broke the Linux build rather
// than all of them. clang-format's IncludeBlocks default preserves the blank-line
// separated blocks below; do not merge them.
#include <pulp/midi/message.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("choc MIDI message comparison instantiates through pulp/midi/message.hpp",
          "[midi][headers]") {
    // Instantiating Message<ShortMIDIMessageStorage>::operator== is the whole
    // point — without the compensating include this TU fails to COMPILE on
    // GCC/libstdc++. The assertions below just keep it an honest test.
    const auto note_on = pulp::midi::MidiEvent::note_on(0, 60, 100);
    const auto same = pulp::midi::MidiEvent::note_on(0, 60, 100);
    const auto other = pulp::midi::MidiEvent::note_on(0, 64, 100);

    REQUIRE(note_on.message == same.message);
    REQUIRE(note_on.message != other.message);
}
