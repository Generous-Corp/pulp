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
#include <pulp/midi/ump_sysex7_reassembler.hpp>

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

// The AU v2 and AU v3 render blocks reuse bridge-owned MidiBuffer members
// (reserve() + set_realtime_capacity_limit(true) at Initialize /
// allocateRenderResources, clear()+clear_sysex() per block) instead of
// constructing a fresh MidiBuffer every render call. This guards that the
// shared contract those adapters depend on stays allocation-free across blocks:
// once reserved + capacity-limited, repeated clear → add → add_sysex_copy must
// never grow the underlying event / sysex / payload vectors (a vector that does
// not reallocate keeps the same capacity()), and overflow must drop rather than
// allocate.
TEST_CASE("AU adapters: reserved MidiBuffer reuse stays capacity-stable across "
          "blocks (no per-block realloc)",
          "[au][midi][au-v2][au-v3][realtime][capacity]")
{
    // Same capacities the AU v2/v3 adapters reserve (kMaxEventsPerBlock etc).
    constexpr std::size_t kEvents = 2048;
    constexpr std::size_t kSysex = 64;
    constexpr std::size_t kSysexBytes = 512;

    midi::MidiBuffer midi_in;
    midi_in.reserve(kEvents, kSysex, kSysexBytes);
    midi_in.set_realtime_capacity_limit(true);

    const std::size_t event_cap = midi_in.event_capacity();
    const std::size_t sysex_cap = midi_in.sysex_capacity();
    const std::size_t payload_cap = midi_in.sysex_copy_payload_capacity();
    REQUIRE(event_cap >= kEvents);
    REQUIRE(sysex_cap >= kSysex);
    REQUIRE(payload_cap >= kSysexBytes);

    const std::vector<uint8_t> sysex_payload{0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};

    // Drive several "render blocks" — clear + clear_sysex + ingest — exactly as
    // the adapters do. Stay within reserved capacity so nothing is expected to
    // drop on a well-behaved block.
    for (int block = 0; block < 64; ++block) {
        midi_in.clear();
        midi_in.clear_sysex();

        for (int i = 0; i < 16; ++i) {
            auto ev = midi::MidiEvent::cc(0, 7, static_cast<uint8_t>(i & 0x7F));
            ev.sample_offset = i;
            REQUIRE(midi_in.add(ev));
        }
        REQUIRE(midi_in.add_sysex_copy(sysex_payload.data(),
                                       sysex_payload.size(),
                                       /*sample_offset=*/0));

        REQUIRE(midi_in.size() == 16);
        REQUIRE(midi_in.sysex_size() == 1);
        // The bytes round-trip through the pooled payload.
        REQUIRE(midi_in.sysex()[0].data == sysex_payload);

        // The load-bearing assertion: no reallocation happened, so the audio
        // thread did not allocate.
        REQUIRE(midi_in.event_capacity() == event_cap);
        REQUIRE(midi_in.sysex_capacity() == sysex_cap);
        REQUIRE(midi_in.sysex_copy_payload_capacity() == payload_cap);
        REQUIRE(midi_in.dropped_event_count() == 0);
        REQUIRE(midi_in.dropped_sysex_count() == 0);
    }
}

// Overflow within a capacity-limited buffer must DROP (record a drop count),
// never grow the vector — otherwise the "no allocation on the audio thread"
// guarantee the AU render path relies on would silently regress under a MIDI /
// SysEx flood.
TEST_CASE("AU adapters: capacity-limited MidiBuffer drops past reserve without "
          "growing",
          "[au][midi][au-v2][au-v3][realtime][capacity]")
{
    constexpr std::size_t kEvents = 8;
    constexpr std::size_t kSysex = 2;
    constexpr std::size_t kSysexBytes = 8;

    midi::MidiBuffer midi_in;
    midi_in.reserve(kEvents, kSysex, kSysexBytes);
    midi_in.set_realtime_capacity_limit(true);
    const std::size_t event_cap = midi_in.event_capacity();
    const std::size_t sysex_cap = midi_in.sysex_capacity();

    // Push more events than reserved.
    for (std::size_t i = 0; i < kEvents + 4; ++i) {
        midi_in.add(midi::MidiEvent::cc(0, 7, static_cast<uint8_t>(i & 0x7F)));
    }
    REQUIRE(midi_in.size() == kEvents);
    REQUIRE(midi_in.dropped_event_count() == 4);
    REQUIRE(midi_in.event_capacity() == event_cap);  // never grew

    // Push more sysex than reserved.
    const std::vector<uint8_t> payload{0xF0, 0x01, 0x02, 0xF7};
    for (std::size_t i = 0; i < kSysex + 2; ++i) {
        midi_in.add_sysex_copy(payload.data(), payload.size());
    }
    REQUIRE(midi_in.sysex_size() == kSysex);
    REQUIRE(midi_in.dropped_sysex_count() == 2);
    REQUIRE(midi_in.sysex_capacity() == sysex_cap);  // never grew

    // A payload larger than the reserved per-event bound drops too (the AU v3
    // UMP sysex path copies into this same pool).
    std::vector<uint8_t> oversize(kSysexBytes + 4, 0x00);
    oversize.front() = 0xF0;
    oversize.back() = 0xF7;
    const auto sysex_before = midi_in.sysex_size();
    REQUIRE_FALSE(midi_in.add_sysex_copy(oversize.data(), oversize.size()));
    REQUIRE(midi_in.sysex_size() == sysex_before);
}

// The AU v3 render block reassembles a multi-packet UMP sysex7 stream through a
// bridge-owned, pre-reserved UmpSysex7Reassembler and emits each completed
// logical sysex via add_sysex_copy into the pre-reserved payload pool, calling
// reset() before each MIDIEventList event. This guards that:
//   * a sysex split across start → continue → end packets is reassembled and
//     emitted EXACTLY ONCE, complete, with NO capacity growth on the buffer or
//     the reassembler, and
//   * reset() after an incomplete (never-ended) stream clears the partial so the
//     next message is not poisoned by the dropped one.
namespace {
// Build a UMP type-0x3 (sysex7) word0: message-type nibble 0x3 in bits 28-31,
// status nibble in bits 20-23, byte-count in bits 16-19, first two payload
// bytes in bits 8-15 and 0-7.
constexpr uint32_t make_sysex7_word0(uint8_t status, uint8_t count,
                                     uint8_t b0, uint8_t b1) {
    return (uint32_t{0x3} << 28) | (uint32_t{status} << 20) |
           (uint32_t{count} << 16) | (uint32_t{b0} << 8) | uint32_t{b1};
}
// Remaining four payload bytes pack into word1 (bits 24-31, 16-23, 8-15, 0-7).
constexpr uint32_t make_sysex7_word1(uint8_t b2, uint8_t b3,
                                     uint8_t b4, uint8_t b5) {
    return (uint32_t{b2} << 24) | (uint32_t{b3} << 16) |
           (uint32_t{b4} << 8) | uint32_t{b5};
}
}  // namespace

TEST_CASE("AU v3 UMP sysex7: multi-packet reassembly emits once, no growth; "
          "reset clears a dropped partial",
          "[au][midi][au-v3][sysex][realtime][capacity]")
{
    constexpr std::size_t kSysexBytes = 64;
    midi::MidiBuffer sink;
    sink.reserve(/*events=*/8, /*sysex=*/8, /*payload=*/kSysexBytes);
    sink.set_realtime_capacity_limit(true);

    midi::UmpSysex7Reassembler reassembler;
    reassembler.reserve(kSysexBytes);
    const std::size_t event_cap = sink.event_capacity();
    const std::size_t sysex_cap = sink.sysex_capacity();
    const std::size_t payload_cap = sink.sysex_copy_payload_capacity();

    // Mirror the adapter's emit: copy the reassembled payload into the buffer's
    // pre-reserved pool (never add_sysex(vector)).
    auto emit = [](const std::vector<uint8_t>& payload, void* user) {
        auto* s = static_cast<midi::MidiBuffer*>(user);
        s->add_sysex_copy(payload.data(), payload.size(), /*sample_offset=*/0, 0.0);
    };

    SECTION("start → continue → end reassembles to one complete payload") {
        reassembler.reset();
        // 14-byte logical payload split 6 / 6 / 2 across three packets.
        // Status 0x1 = start, 0x2 = continue, 0x3 = end.
        REQUIRE(reassembler.feed_packet(
                    make_sysex7_word0(0x1, 6, 0x01, 0x02),
                    make_sysex7_word1(0x03, 0x04, 0x05, 0x06), emit, &sink) ==
                midi::UmpSysex7Reassembler::Status::start);
        REQUIRE(reassembler.feed_packet(
                    make_sysex7_word0(0x2, 6, 0x07, 0x08),
                    make_sysex7_word1(0x09, 0x0A, 0x0B, 0x0C), emit, &sink) ==
                midi::UmpSysex7Reassembler::Status::continued);
        REQUIRE(reassembler.feed_packet(
                    make_sysex7_word0(0x3, 2, 0x0D, 0x0E),
                    make_sysex7_word1(0, 0, 0, 0), emit, &sink) ==
                midi::UmpSysex7Reassembler::Status::ended);

        // Emitted exactly once, complete, in order.
        REQUIRE(sink.sysex_size() == 1);
        const std::vector<uint8_t> expected{0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                            0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                                            0x0D, 0x0E};
        REQUIRE(sink.sysex()[0].data == expected);
        REQUIRE_FALSE(reassembler.in_progress());

        // No capacity growth anywhere on the hot path.
        REQUIRE(sink.event_capacity() == event_cap);
        REQUIRE(sink.sysex_capacity() == sysex_cap);
        REQUIRE(sink.sysex_copy_payload_capacity() == payload_cap);
        REQUIRE(sink.dropped_sysex_count() == 0);
    }

    SECTION("reset() after an incomplete stream does not poison the next message") {
        // Start a stream and never close it (host hands us a truncated group).
        REQUIRE(reassembler.feed_packet(
                    make_sysex7_word0(0x1, 6, 0xAA, 0xBB),
                    make_sysex7_word1(0xCC, 0xDD, 0xEE, 0xFF), emit, &sink) ==
                midi::UmpSysex7Reassembler::Status::start);
        REQUIRE(reassembler.in_progress());
        REQUIRE(reassembler.partial_size() == 6);
        REQUIRE(sink.sysex_size() == 0);  // nothing emitted yet

        // Adapter calls reset() before the next MIDIEventList event. The partial
        // must be discarded — not prepended to the next message.
        reassembler.reset();
        REQUIRE_FALSE(reassembler.in_progress());
        REQUIRE(reassembler.partial_size() == 0);

        // A fresh single-packet sysex (status 0x0) now emits clean.
        REQUIRE(reassembler.feed_packet(
                    make_sysex7_word0(0x0, 3, 0x11, 0x22),
                    make_sysex7_word1(0x33, 0, 0, 0), emit, &sink) ==
                midi::UmpSysex7Reassembler::Status::single_packet);
        REQUIRE(sink.sysex_size() == 1);
        const std::vector<uint8_t> expected{0x11, 0x22, 0x33};
        REQUIRE(sink.sysex()[0].data == expected);  // not poisoned by 0xAA..0xFF
        REQUIRE(sink.sysex_copy_payload_capacity() == payload_cap);
    }

    SECTION("orphan continue/end without a start is dropped, leaving state clean") {
        reassembler.reset();
        REQUIRE(reassembler.feed_packet(
                    make_sysex7_word0(0x2, 4, 0x77, 0x88),
                    make_sysex7_word1(0x99, 0xAA, 0, 0), emit, &sink) ==
                midi::UmpSysex7Reassembler::Status::dropped);
        REQUIRE(reassembler.feed_packet(
                    make_sysex7_word0(0x3, 2, 0x55, 0x66),
                    make_sysex7_word1(0, 0, 0, 0), emit, &sink) ==
                midi::UmpSysex7Reassembler::Status::dropped);
        REQUIRE_FALSE(reassembler.in_progress());
        REQUIRE(sink.sysex_size() == 0);  // nothing emitted from orphans
    }
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
