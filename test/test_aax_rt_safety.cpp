// RT-safety proof for the AAX render path's MIDI-output buffers.
//
// The AAX process callback wraps `processor->process()` in a ScopedNoAlloc
// guard. A supports_midi_output plugin (arpeggiator / MIDI FX / generator)
// appends to `midi_out` from inside process(); if that buffer were not
// pre-reserved + capacity-limited off the render thread, the first add() would
// push_back on a zero-capacity vector and allocate on the AAX render thread —
// the exact hazard the adapter is meant to remove.
//
// aax_runtime.cpp's InstanceState constructor now reserves midi_in/midi_out to
// the capacities below and sets set_realtime_capacity_limit(true). This test
// reproduces that setup with the real RtAllocationProbe interposer and proves
// (a) add()/add_sysex_copy() into the reserved+limited buffers allocate nothing
// on the render-thread append, and (b) the probe genuinely catches the pre-fix
// behavior (an unreserved buffer DOES allocate), so the passing assertion in
// (a) is meaningful.
//
// The probe is armed only around the render-thread append region — never around
// setup (reserve) or Catch2 reporting, both of which legitimately allocate.
// A warmup pass primes any one-time lazy/static init before measuring.
//
// Kept in sync with kAaxMax* in core/format/src/aax_runtime.cpp.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <cstdint>

namespace {

// Mirror of the AAX render-thread MIDI reserve capacities.
constexpr std::size_t kAaxMaxMidiEventsPerBlock = 2048;
constexpr std::size_t kAaxMaxSysexPerBlock = 64;
constexpr std::size_t kAaxMaxSysexPayloadBytes = 512;

// Reserve + capacity-limit exactly as InstanceState's constructor does.
void reserve_like_aax(pulp::midi::MidiBuffer& buf) {
    buf.reserve(kAaxMaxMidiEventsPerBlock, kAaxMaxSysexPerBlock,
                kAaxMaxSysexPayloadBytes);
    buf.set_realtime_capacity_limit(true);
}

// One block of MIDI-out work a supports_midi_output processor would emit from
// inside process(): a dense run of note on/off plus a short sysex copy.
void emit_one_block(pulp::midi::MidiBuffer& midi_out) {
    midi_out.clear();
    midi_out.clear_sysex();
    for (int i = 0; i < 512; ++i) {
        const auto note = static_cast<uint8_t>(36 + (i % 60));
        midi_out.add(pulp::midi::MidiEvent::note_on(0, note, 100));
        midi_out.add(pulp::midi::MidiEvent::note_off(0, note));
    }
    static const uint8_t kSysex[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7};
    midi_out.add_sysex_copy(kSysex, sizeof(kSysex));
}

}  // namespace

TEST_CASE("AAX RT-safety: MIDI-out block append allocates nothing under the no-alloc guard",
          "[format][aax][rt-safety]") {
    pulp::midi::MidiBuffer midi_out;
    reserve_like_aax(midi_out);  // off-thread setup: allocates here, on purpose
    emit_one_block(midi_out);    // warmup: prime any one-time lazy init

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        emit_one_block(midi_out);  // the render-thread region under the guard
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);
}

TEST_CASE("AAX RT-safety: the probe catches the pre-fix unreserved-buffer allocation",
          "[format][aax][rt-safety]") {
    // The old AAX state: a MidiBuffer that was never reserved and never
    // capacity-limited. Appending must push_back into a zero-capacity vector
    // and therefore allocate — proving the probe (and thus the test above) is
    // a real tripwire, not a vacuous pass.
    pulp::midi::MidiBuffer unreserved;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        unreserved.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
        allocations = probe.allocation_count();
    }
    CHECK(allocations > 0);
}

TEST_CASE("AAX RT-safety: capacity-limited midi_out drops past capacity without allocating",
          "[format][aax][rt-safety]") {
    pulp::midi::MidiBuffer midi_out;
    reserve_like_aax(midi_out);
    midi_out.clear();
    midi_out.add(pulp::midi::MidiEvent::note_on(0, 64, 100));  // warmup

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        midi_out.clear();
        // Push well past the reserved event capacity: the limit must make add()
        // drop rather than grow the vector, so still zero allocations.
        for (std::size_t i = 0; i < kAaxMaxMidiEventsPerBlock + 256; ++i) {
            midi_out.add(pulp::midi::MidiEvent::note_on(0, 64, 100));
        }
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);
}
