// AU v2 effect adapter MIDI-input tests.
//
// Covers the 2026-04-22 gap where `PulpAUEffect` inherited from
// `AUEffectBase` (no MIDI path) and the CMake AU type defaulted to `aufx`,
// leaving descriptor-declared `accepts_midi = true` plug-ins silent when
// hosted by Logic / MainStage / GarageBand. See the auv2 skill for the
// full background.
//
// These tests focus on the MIDI-decode helper that sits between
// AUMIDIBase::HandleMIDIEvent and the process() drain. The surrounding
// plumbing — push under mutex, drain at the top of ProcessBufferLists —
// is a handful of lines and a direct mirror of the AU v2 instrument
// adapter; the high-leverage regression here is the byte decode, which
// has to handle status/channel split correctly across the full
// channel-voice message family.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/au_v2_instrument.hpp>  // pulls the MusicDevice SDK (AUMusicLookup)
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioUnit.h>

#include <cstdint>

using namespace pulp;
using pulp::format::au::decode_midi_event;

// Component-entry dispatch contract for MIDI-receiving effects (`aumf`).
//
// The bug this guards: PULP_AU_MIDI_PLUGIN must register through
// ausdk::AUMIDIEffectFactory, NOT ausdk::AUBaseFactory. A factory only
// dispatches the selectors its lookup table carries — AUBaseLookup has no
// MusicDevice selectors, so a `MusicDeviceMIDIEvent` call on an aumf built
// with the base factory returns -4 (unimpErr) and auval fails with
// "-4 IN CALL MusicDeviceMIDIEvent" (the host never delivers a note). The
// adapter class already implements HandleMIDIEvent; the *lookup table* is
// what was wrong. This pins the exact selector the two factories' lookups
// must (and must not) carry, so a regression to the base factory is caught
// in CI instead of at auval/Logic time.
TEST_CASE("AU v2 MIDI-effect lookup dispatches MusicDeviceMIDIEvent; "
          "base lookup does not",
          "[au][midi][au-v2][dispatch]")
{
    // AUMIDIEffectFactory (used by PULP_AU_MIDI_PLUGIN) routes the
    // MusicDevice MIDIEvent selector to a real method.
    REQUIRE(ausdk::AUMIDILookup::Lookup(kMusicDeviceMIDIEventSelect) != nullptr);
    // The plain effect factory (PULP_AU_PLUGIN, aufx) does not — calling
    // MusicDeviceMIDIEvent there is the unimplemented path that broke aumf.
    REQUIRE(ausdk::AUBaseLookup::Lookup(kMusicDeviceMIDIEventSelect) == nullptr);
    // Both still dispatch a core selector — sanity that the lookups are live.
    REQUIRE(ausdk::AUMIDILookup::Lookup(kAudioUnitInitializeSelect) != nullptr);
    REQUIRE(ausdk::AUBaseLookup::Lookup(kAudioUnitInitializeSelect) != nullptr);
}

// The INSTRUMENT (`aumu`) analogue of the dispatch bug above. PULP_AU_INSTRUMENT
// registers through ausdk::AUMusicDeviceFactory (lookup = AUMusicLookup), which
// carries the MusicDevice MIDI selector. The plain PULP_AU_PLUGIN (AUBaseFactory)
// does NOT — so an instrument mistakenly built with the effect macro stamps an
// `aumu` Info.plist but returns -4 (unimpErr) on MusicDeviceMIDIEvent: it loads,
// plays UI-triggered audio, and silently ignores all host MIDI. That is exactly
// the "AU plays slice taps but not MIDI" bug (PulpTempoSampler used PULP_AU_PLUGIN
// before this fix). This pins the selector the instrument factory must carry.
TEST_CASE("AU v2 instrument lookup dispatches MusicDeviceMIDIEvent; "
          "base lookup does not",
          "[au][midi][au-v2][dispatch]")
{
    REQUIRE(ausdk::AUMusicLookup::Lookup(kMusicDeviceMIDIEventSelect) != nullptr);
    REQUIRE(ausdk::AUBaseLookup::Lookup(kMusicDeviceMIDIEventSelect) == nullptr);
    // Sanity: the instrument lookup also dispatches a core selector.
    REQUIRE(ausdk::AUMusicLookup::Lookup(kAudioUnitInitializeSelect) != nullptr);
}

TEST_CASE("AU v2 effect: Control Change decode round-trips channel + data",
          "[au][midi][au-v2][issue-pending]")
{
    // Matches the example the spec calls out: `HandleMIDIEvent(0xB0, 74, 100, 32)`.
    // AUMIDIBase splits the status byte into (0xB0, channel 0) before calling
    // HandleMIDIEvent, so `inStatus` carries the top nibble only and
    // `inChannel` carries the bottom nibble.
    const auto ev = decode_midi_event(/*inStatus=*/0xB0,
                                      /*inChannel=*/0x00,
                                      /*inData1=*/74,
                                      /*inData2=*/100);

    REQUIRE(ev.is_cc());
    REQUIRE(ev.channel() == 0);
    REQUIRE(ev.cc_number() == 74);
    REQUIRE(ev.cc_value() == 100);
    REQUIRE(ev.sample_offset == 0);
}

TEST_CASE("AU v2 effect: CC channel nibble re-combined into status byte",
          "[au][midi][au-v2][issue-pending]")
{
    // Channel 5 on a CC message — exercises the
    // `(status & 0xF0) | (channel & 0x0F)` recombination path.
    const auto ev = decode_midi_event(/*inStatus=*/0xB0,
                                      /*inChannel=*/0x05,
                                      /*inData1=*/7,
                                      /*inData2=*/64);

    REQUIRE(ev.is_cc());
    REQUIRE(ev.channel() == 5);
    REQUIRE(ev.cc_number() == 7);
    REQUIRE(ev.cc_value() == 64);
    // Raw on-the-wire status byte must be 0xB5.
    REQUIRE(ev.data()[0] == 0xB5);
}

TEST_CASE("AU v2 effect: Pitch Bend decode preserves LSB+MSB",
          "[au][midi][au-v2][issue-pending]")
{
    // `HandleMIDIEvent(0xE0, 0, 64, 48)` — pitch bend on channel 0,
    // LSB=0 MSB=64 is ~8192 (center). Shift to MSB=48 (non-center) to
    // catch a swapped-byte bug.
    const auto ev = decode_midi_event(/*inStatus=*/0xE0,
                                      /*inChannel=*/0x00,
                                      /*inData1=*/0,
                                      /*inData2=*/48);

    REQUIRE(ev.is_pitch_bend());
    REQUIRE(ev.channel() == 0);
    REQUIRE(ev.data()[0] == 0xE0);
    REQUIRE(ev.data()[1] == 0);
    REQUIRE(ev.data()[2] == 48);
}

TEST_CASE("AU v2 effect: Note On decode across all channels",
          "[au][midi][au-v2][issue-pending]")
{
    for (uint8_t channel = 0; channel < 16; ++channel) {
        const auto ev = decode_midi_event(/*inStatus=*/0x90,
                                          /*inChannel=*/channel,
                                          /*inData1=*/60,
                                          /*inData2=*/100);
        REQUIRE(ev.is_note_on());
        REQUIRE(ev.channel() == channel);
        REQUIRE(ev.note() == 60);
        REQUIRE(ev.velocity() == 100);
    }
}

TEST_CASE("AU v2 effect: Program Change decode preserves program number",
          "[au][midi][au-v2][issue-pending]")
{
    const auto ev = decode_midi_event(/*inStatus=*/0xC0,
                                      /*inChannel=*/0x03,
                                      /*inData1=*/42,
                                      /*inData2=*/0);
    REQUIRE(ev.is_program_change());
    REQUIRE(ev.channel() == 3);
    REQUIRE(ev.data()[0] == 0xC3);
    REQUIRE(ev.data()[1] == 42);
}

TEST_CASE("AU v2 effect: System message status byte reassembles SDK split",
          "[au][midi][au-v2][issue-pending]")
{
    // AUMIDIBase::MIDIEvent splits the wire-format status byte into a
    // top-nibble `inStatus` and a low-nibble `inChannel` BEFORE calling
    // HandleMIDIEvent — for system messages the same way as for
    // channel-voice. So a host-delivered 0xF8 (timing clock) reaches the
    // decoder as inStatus=0xF0, inChannel=0x08; the decoder must
    // reassemble (top | low) = 0xF8.
    //
    // A regression fixture that passes the wire-format byte directly would
    // also return 0xF8 from a buggy "is_system -> return inStatus unchanged"
    // branch. Model the SDK contract instead: the status arrives split into
    // the 0xF0 top nibble plus the low-nibble channel field.
    SECTION("0xF8 — timing clock") {
        const auto ev = decode_midi_event(/*inStatus=*/0xF0,
                                          /*inChannel=*/0x08,
                                          /*inData1=*/0,
                                          /*inData2=*/0);
        REQUIRE(ev.data()[0] == 0xF8);
    }
    SECTION("0xFA — start") {
        const auto ev = decode_midi_event(0xF0, 0x0A, 0, 0);
        REQUIRE(ev.data()[0] == 0xFA);
    }
    SECTION("0xFC — stop") {
        const auto ev = decode_midi_event(0xF0, 0x0C, 0, 0);
        REQUIRE(ev.data()[0] == 0xFC);
    }
    SECTION("0xF2 — song position pointer (system common)") {
        const auto ev = decode_midi_event(0xF0, 0x02, 0x42, 0x10);
        REQUIRE(ev.data()[0] == 0xF2);
        REQUIRE(ev.data()[1] == 0x42);
        REQUIRE(ev.data()[2] == 0x10);
    }
}

TEST_CASE("AU v2 effect: sysex routing lands in MidiBuffer's sysex sidecar",
          "[au][midi][au-v2][issue-pending]")
{
    // HandleSysEx is trivially wired (copy bytes → add_sysex), so this
    // test verifies the MidiBuffer contract we depend on rather than
    // going through AU construction. If MidiBuffer ever renames / drops
    // the sysex sidecar, the adapter change breaks at compile time AND
    // this test flips red.
    midi::MidiBuffer buf;
    const std::vector<uint8_t> payload{0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};

    buf.add_sysex(payload, /*sample_offset=*/0);

    REQUIRE(buf.sysex_size() == 1);
    REQUIRE(buf.sysex()[0].data == payload);
    REQUIRE(buf.sysex()[0].sample_offset == 0);
}

TEST_CASE("AU v2 render context is explicit realtime for effects and instruments",
          "[au][au-v2][runtime-mode]")
{
    const auto ctx = pulp::format::au::make_render_process_context(
        /*sample_rate=*/48000.0, /*num_samples=*/128);

    REQUIRE(ctx.sample_rate == 48000.0);
    REQUIRE(ctx.num_samples == 128);
    REQUIRE(ctx.process_mode == pulp::format::ProcessMode::Realtime);
    REQUIRE(ctx.render_speed_hint == pulp::format::RenderSpeedHint::Realtime);
    REQUIRE_FALSE(ctx.is_offline());
    REQUIRE_FALSE(ctx.allows_offline_quality_work());
    REQUIRE_FALSE(ctx.is_maintenance_render());
}
