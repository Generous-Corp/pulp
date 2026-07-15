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
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/ump_sysex7_reassembler.hpp>

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioUnit.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

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

// ── Channel-config negotiation (kAudioUnitProperty_SupportedNumChannels) ─────
//
// build_channel_info derives the AUChannelInfo table a host (and auval) reads to
// discover which (input, output) channel-count pairs the plugin handles. Pure
// over the descriptor, so it is unit-testable without an AudioComponentInstance.
TEST_CASE("AU v2 channel info: stereo effect reports mono+stereo matched pairs",
          "[au][au-v2][channels]")
{
    pulp::format::PluginDescriptor desc;  // default = stereo in / stereo out
    AUChannelInfo info[pulp::format::au::kMaxChannelInfoPairs];
    const UInt32 n = pulp::format::au::build_channel_info(desc, info);

    REQUIRE(n == 2);
    // {1,1} then {2,2}: an effect supports its declared width and the narrower
    // mono flex, always matching in==out.
    REQUIRE(info[0].inChannels == 1);
    REQUIRE(info[0].outChannels == 1);
    REQUIRE(info[1].inChannels == 2);
    REQUIRE(info[1].outChannels == 2);
}

TEST_CASE("AU v2 channel info: mono effect reports only the mono pair",
          "[au][au-v2][channels]")
{
    pulp::format::PluginDescriptor desc;
    desc.input_buses = {{"Main In", 1, false}};
    desc.output_buses = {{"Main Out", 1, false}};
    AUChannelInfo info[pulp::format::au::kMaxChannelInfoPairs];
    const UInt32 n = pulp::format::au::build_channel_info(desc, info);

    REQUIRE(n == 1);
    REQUIRE(info[0].inChannels == 1);
    REQUIRE(info[0].outChannels == 1);
}

TEST_CASE("AU v2 channel info: instrument (0 inputs) reports 0-in / N-out pairs",
          "[au][au-v2][channels]")
{
    pulp::format::PluginDescriptor desc;
    desc.category = pulp::format::PluginCategory::Instrument;
    desc.input_buses = {};                               // no audio input
    desc.output_buses = {{"Main Out", 2, false}};        // stereo synth
    AUChannelInfo info[pulp::format::au::kMaxChannelInfoPairs];
    const UInt32 n = pulp::format::au::build_channel_info(desc, info);

    REQUIRE(n == 2);
    // {0,1} then {0,2}: zero inputs, mono and stereo output widths.
    REQUIRE(info[0].inChannels == 0);
    REQUIRE(info[0].outChannels == 1);
    REQUIRE(info[1].inChannels == 0);
    REQUIRE(info[1].outChannels == 2);
}

TEST_CASE("AU v2 channel info: never exceeds the declared output width",
          "[au][au-v2][channels]")
{
    // A degenerate over-wide declaration clamps into the supported {1,2} flex.
    pulp::format::PluginDescriptor desc;
    desc.input_buses = {{"Main In", 7, false}};
    desc.output_buses = {{"Main Out", 7, false}};
    AUChannelInfo info[pulp::format::au::kMaxChannelInfoPairs];
    const UInt32 n = pulp::format::au::build_channel_info(desc, info);
    REQUIRE(n == 2);
    REQUIRE(info[1].inChannels == 2);
    REQUIRE(info[1].outChannels == 2);
}

TEST_CASE("AU v2 channel info: asymmetric mono-in / stereo-out reports the exact pair",
          "[au][au-v2][channels]")
{
    // A widener (1 in, 2 out) must NOT be hidden behind a matched {1,1}/{2,2}
    // ladder — the host has to see the real asymmetric capability.
    pulp::format::PluginDescriptor desc;
    desc.input_buses = {{"Main In", 1, false}};
    desc.output_buses = {{"Main Out", 2, false}};
    AUChannelInfo info[pulp::format::au::kMaxChannelInfoPairs];
    const UInt32 n = pulp::format::au::build_channel_info(desc, info);

    REQUIRE(n == 1);
    REQUIRE(info[0].inChannels == 1);
    REQUIRE(info[0].outChannels == 2);
}

// ── MIDI output (kAudioUnitProperty_MIDIOutputCallback) ──────────────────────
//
// The render path packs the Processor's output MidiBuffer into a MIDIPacketList
// (built into pre-reserved storage — no audio-thread allocation) and hands it to
// the host callback. MidiOutputPacketBuilder is the delivery buffer; testing it
// directly exercises the exact bytes + sample offsets a host callback receives,
// without an AudioComponentInstance / live AU host. We then drive the builder
// through the host-callback signature to prove the full delivery contract.
namespace {
// Capture struct that mimics a host installing kAudioUnitProperty_MIDIOutputCallback.
struct CapturedMidi {
    uint8_t status = 0, d1 = 0, d2 = 0, length = 0;
    MIDITimeStamp time = 0;
    int packets = 0;
};
}  // namespace

TEST_CASE("AU v2 MIDI out: builder packs a short message with its sample offset",
          "[au][au-v2][midi-out]")
{
    pulp::midi::MidiBuffer midi_out;
    auto cc = pulp::midi::MidiEvent::cc(/*channel=*/2, /*controller=*/74,
                                        /*value=*/100);
    cc.sample_offset = 64;
    midi_out.add(cc);

    pulp::format::au::MidiOutputPacketBuilder builder;
    const MIDIPacketList* list = builder.build(midi_out, /*frame_count=*/256);

    REQUIRE(list != nullptr);
    REQUIRE(list->numPackets == 1);
    const MIDIPacket* pkt = &list->packet[0];
    REQUIRE(pkt->length == 3);
    REQUIRE(pkt->data[0] == 0xB2);   // CC on channel 2
    REQUIRE(pkt->data[1] == 74);
    REQUIRE(pkt->data[2] == 100);
    // The packet timestamp carries the event's in-block sample offset.
    REQUIRE(pkt->timeStamp == 64);
    REQUIRE(builder.dropped == 0);
}

TEST_CASE("AU v2 MIDI out: empty buffer yields no packet list (callback skipped)",
          "[au][au-v2][midi-out]")
{
    pulp::midi::MidiBuffer midi_out;  // nothing emitted
    pulp::format::au::MidiOutputPacketBuilder builder;
    REQUIRE(builder.build(midi_out, /*frame_count=*/256) == nullptr);
}

TEST_CASE("AU v2 MIDI out: multiple events deliver in order to the host callback",
          "[au][au-v2][midi-out]")
{
    pulp::midi::MidiBuffer midi_out;
    auto n_on = pulp::midi::MidiEvent::note_on(/*channel=*/0, /*note=*/60,
                                               /*velocity=*/127);
    n_on.sample_offset = 0;
    auto n_off = pulp::midi::MidiEvent::note_off(/*channel=*/0, /*note=*/60,
                                                 /*velocity=*/0);
    n_off.sample_offset = 128;
    midi_out.add(n_on);
    midi_out.add(n_off);

    pulp::format::au::MidiOutputPacketBuilder builder;
    const MIDIPacketList* list = builder.build(midi_out, /*frame_count=*/256);
    REQUIRE(list != nullptr);
    REQUIRE(list->numPackets == 2);

    // Drive the documented host-callback signature, exactly as the render path
    // does, and confirm the host sees both events with correct bytes + offsets.
    std::vector<CapturedMidi> received;
    AUMIDIOutputCallback cb = [](void* userData, const AudioTimeStamp* /*ts*/,
                                 UInt32 midiOutNum,
                                 const MIDIPacketList* pl) -> OSStatus {
        REQUIRE(midiOutNum == 0);
        auto* out = static_cast<std::vector<CapturedMidi>*>(userData);
        const MIDIPacket* p = &pl->packet[0];
        for (UInt32 i = 0; i < pl->numPackets; ++i) {
            CapturedMidi c;
            c.length = p->length;
            c.status = p->data[0];
            c.d1 = p->length > 1 ? p->data[1] : 0;
            c.d2 = p->length > 2 ? p->data[2] : 0;
            c.time = p->timeStamp;
            out->push_back(c);
            p = MIDIPacketNext(p);
        }
        return noErr;
    };
    AudioTimeStamp render_time{};
    render_time.mSampleTime = 0;
    cb(&received, &render_time, /*midiOutNum=*/0, list);

    REQUIRE(received.size() == 2);
    REQUIRE(received[0].status == 0x90);   // note on, channel 0
    REQUIRE(received[0].d1 == 60);
    REQUIRE(received[0].d2 == 127);
    REQUIRE(received[0].time == 0);
    REQUIRE(received[1].status == 0x80);   // note off, channel 0
    REQUIRE(received[1].d1 == 60);
    REQUIRE(received[1].time == 128);
}

// Plain audio effects (no produces_midi) must NOT advertise a MIDI output, so
// the builder only ever runs for declared MIDI producers. This guards the
// descriptor gate that plugin_produces_midi() reads in the adapter.
TEST_CASE("AU v2 MIDI out: produces_midi flag gates the output surface",
          "[au][au-v2][midi-out]")
{
    pulp::format::PluginDescriptor effect;          // default effect
    REQUIRE_FALSE(effect.produces_midi);

    pulp::format::PluginDescriptor midi_fx;
    midi_fx.category = pulp::format::PluginCategory::MidiEffect;
    midi_fx.accepts_midi = true;
    midi_fx.produces_midi = true;
    REQUIRE(midi_fx.produces_midi);
}

TEST_CASE("AU v2 MIDI out: interleaved short + SysEx deliver in ascending offset order",
          "[au][au-v2][midi-out]")
{
    // SysEx@0 must be delivered BEFORE a note@64 even though the two live in
    // separate sidecars — CoreMIDI packet lists are expected time-ordered, and
    // the host plays them back in list order.
    pulp::midi::MidiBuffer midi_out;
    auto note = pulp::midi::MidiEvent::note_on(/*channel=*/0, /*note=*/60,
                                               /*velocity=*/100);
    note.sample_offset = 64;
    midi_out.add(note);
    const std::vector<uint8_t> id_request{0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    midi_out.add_sysex(id_request, /*sample_offset=*/0);

    pulp::format::au::MidiOutputPacketBuilder builder;
    const MIDIPacketList* list = builder.build(midi_out, /*frame_count=*/256);
    REQUIRE(list != nullptr);
    REQUIRE(list->numPackets == 2);

    // Walk the list and confirm ascending packet timestamps with the SysEx first.
    const MIDIPacket* p0 = &list->packet[0];
    REQUIRE(p0->timeStamp == 0);
    REQUIRE(p0->data[0] == 0xF0);          // SysEx start at offset 0
    const MIDIPacket* p1 = MIDIPacketNext(p0);
    REQUIRE(p1->timeStamp == 64);
    REQUIRE(p1->data[0] == 0x90);          // note-on at offset 64
    REQUIRE(builder.dropped == 0);
}

TEST_CASE("AU v2 MIDI out: offsets are clamped into the current block",
          "[au][au-v2][midi-out]")
{
    pulp::midi::MidiBuffer midi_out;
    auto early = pulp::midi::MidiEvent::cc(0, 7, 10);
    early.sample_offset = -5;              // stray negative
    auto late = pulp::midi::MidiEvent::cc(0, 7, 20);
    late.sample_offset = 9999;             // past the block
    midi_out.add(early);
    midi_out.add(late);

    pulp::format::au::MidiOutputPacketBuilder builder;
    const MIDIPacketList* list = builder.build(midi_out, /*frame_count=*/128);
    REQUIRE(list != nullptr);
    REQUIRE(list->numPackets == 2);
    // Negative clamps to 0; over-block clamps to frame_count - 1 = 127.
    const MIDIPacket* p0 = &list->packet[0];
    REQUIRE(p0->timeStamp == 0);
    const MIDIPacket* p1 = MIDIPacketNext(p0);
    REQUIRE(p1->timeStamp == 127);
}

// Atomic snapshot of the (callback, userData) pair. The pair is written on the
// main thread (SetProperty) and read on the render thread, so a torn read could
// pair a fresh callback with a stale userData. Hammer the publish/consume cycle
// from two threads and assert every observed pair is internally consistent —
// the consumer must never see callback_A with userData_B.
TEST_CASE("AU v2 MIDI out: callback pair publishes atomically (no torn pair)",
          "[au][au-v2][midi-out][realtime]")
{
    // Two distinct (callback, userData) identities; the consumer asserts the
    // observed pair always matches one of them as a whole, never a cross.
    struct Pair {
        AUMIDIOutputCallback cb;
        void* ud;
    };
    static int marker_a = 1, marker_b = 2;
    AUMIDIOutputCallback cb_a = [](void*, const AudioTimeStamp*, UInt32,
                                   const MIDIPacketList*) -> OSStatus {
        return noErr;
    };
    AUMIDIOutputCallback cb_b = [](void*, const AudioTimeStamp*, UInt32,
                                   const MIDIPacketList*) -> OSStatus {
        return 1;
    };
    const Pair pair_a{cb_a, &marker_a};
    const Pair pair_b{cb_b, &marker_b};

    // Mirror the adapter's double-buffer publish exactly.
    std::array<Pair, 2> slots{};
    std::atomic<std::uint8_t> write_slot{0};
    std::atomic<const Pair*> published{nullptr};

    auto publish = [&](const Pair& p) {
        const std::uint8_t s = write_slot.load(std::memory_order_relaxed);
        slots[s] = p;
        published.store(&slots[s], std::memory_order_release);
        write_slot.store(s ^ 1, std::memory_order_relaxed);
    };
    publish(pair_a);

    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};
    std::atomic<long> reads{0};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const Pair* p = published.load(std::memory_order_acquire);
            if (!p) continue;
            // Snapshot the fields, then verify they form one of the two known
            // whole pairs. A torn publish would surface as a cross here.
            AUMIDIOutputCallback c = p->cb;
            void* u = p->ud;
            const bool ok =
                (c == cb_a && u == &marker_a) || (c == cb_b && u == &marker_b);
            if (!ok) torn.store(true, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread writer([&] {
        for (int i = 0; i < 200000; ++i) publish((i & 1) ? pair_b : pair_a);
    });

    writer.join();
    // Let the reader observe a bit more, then stop.
    while (reads.load(std::memory_order_relaxed) < 1000 &&
           !torn.load(std::memory_order_relaxed)) { /* spin briefly */ }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    REQUIRE_FALSE(torn.load(std::memory_order_relaxed));
    REQUIRE(reads.load(std::memory_order_relaxed) > 0);
}

// ---------------------------------------------------------------------------
// Silence-flag contract for generated output.
//
// AUEffectBase::Render hands ioActionFlags to AUInputElement::PullInput, so a
// host that renders silence upstream ORs kAudioUnitRenderAction_OutputIsSilence
// into it before ProcessBufferLists ever runs. The stock
// AUEffectBase::ProcessBufferLists is the code that clears the bit again once a
// kernel produces output — and PulpAUEffect overrides ProcessBufferLists, so
// that clear never happened. The result was a full output buffer labeled
// silent: a host that honours the label substitutes digital silence, which
// silently deletes the output of every generator, oscillator, reverb tail, and
// DC/control-voltage source.
//
// The failure is invisible without this test. PulpAUEffect passes
// inProcessesInPlace = true, so AUEffectBase::Render's
// `if (silence && !ProcessesInPlace()) ZeroBuffer(output)` never fires — the
// buffer really does hold the right samples. Only the flag is wrong, and only
// the host acts on it. A Processor-level "write 0.5, read 0.5" test passes
// while the bug is live.
namespace {

// Writes a constant to every output sample regardless of input. The shape of
// every CV/generator plugin, and the shape the silence bit destroys.
class DcEffectProcessor : public pulp::format::Processor {
public:
    static constexpr float kValue = 0.5f;

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "AUEffectSilenceTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au-effect-silence",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"Main Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t c = 0; c < output.num_channels(); ++c) {
            float* dst = output.channel_ptr(c);
            for (std::size_t n = 0; n < output.num_samples(); ++n) dst[n] = kValue;
        }
    }

    void process(pulp::format::ProcessBuffers& audio,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override {
        if (auto* out = audio.main_output()) {
            pulp::audio::BufferView<const float> empty_input;
            process(*out, empty_input, midi_in, midi_out, context);
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_dc_effect() {
    return std::make_unique<DcEffectProcessor>();
}

struct ScopedDcFactory {
    ScopedDcFactory() : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(create_dc_effect);
    }
    ~ScopedDcFactory() { pulp::format::register_plugin(previous); }
    pulp::format::ProcessorFactory previous;
};

AudioStreamBasicDescription silence_test_format(double sample_rate, UInt32 channels) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = sample_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                       kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    return fmt;
}

}  // namespace

TEST_CASE("AU v2 effect clears OutputIsSilence when the processor generates output",
          "[au][au-v2][effect][silence][dc]")
{
    ScopedDcFactory factory;

    constexpr double kSampleRate = 48000.0;
    constexpr UInt32 kChannels = 2;
    constexpr UInt32 kFrames = 64;

    pulp::format::au::PulpAUEffect effect(nullptr);
    effect.CreateElements();
    REQUIRE(effect.GetInput(0)->SetStreamFormat(
                silence_test_format(kSampleRate, kChannels)) == noErr);
    REQUIRE(effect.GetOutput(0)->SetStreamFormat(
                silence_test_format(kSampleRate, kChannels)) == noErr);
    UInt32 max_frames = kFrames;
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice,
                                       kAudioUnitScope_Global, 0, &max_frames,
                                       sizeof(max_frames)) == noErr);
    REQUIRE(effect.DoInitialize() == noErr);

    // Silent input, as a host feeds a generator sitting on an empty track.
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};

    // AudioBufferList declares mBuffers[1]; a two-channel list is the standard
    // trailing-storage idiom. The static_assert pins the layout the idiom needs.
    struct TwoBufferList {
        AudioBufferList bl;
        AudioBuffer second;
    };
    static_assert(offsetof(TwoBufferList, second) ==
                      offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer),
                  "AudioBufferList trailing-storage layout assumption broken");

    auto make_bufferlist = [](float* a, float* b, TwoBufferList& list) {
        list.bl.mNumberBuffers = 2;
        list.bl.mBuffers[0] = {1, static_cast<UInt32>(kFrames * sizeof(float)), a};
        list.bl.mBuffers[1] = {1, static_cast<UInt32>(kFrames * sizeof(float)), b};
    };

    TwoBufferList input{}, output{};
    make_bufferlist(in_l.data(), in_r.data(), input);
    make_bufferlist(out_l.data(), out_r.data(), output);

    // The host says "what I handed you upstream is digital silence."
    AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_OutputIsSilence;

    REQUIRE(effect.ProcessBufferLists(flags, input.bl, output.bl, kFrames) == noErr);

    // The processor generated a DC level from that silence. The buffer must
    // hold it, and the adapter must retract the host's silence claim — the
    // whole point, and the half that was missing.
    REQUIRE((flags & kAudioUnitRenderAction_OutputIsSilence) == 0);
    for (UInt32 n = 0; n < kFrames; ++n) {
        REQUIRE(out_l[n] == DcEffectProcessor::kValue);
        REQUIRE(out_r[n] == DcEffectProcessor::kValue);
    }

    // A second block must not re-raise it (the flag is per-block, and a host
    // that keeps passing the bit in must keep getting it cleared).
    flags = kAudioUnitRenderAction_OutputIsSilence;
    REQUIRE(effect.ProcessBufferLists(flags, input.bl, output.bl, kFrames) == noErr);
    REQUIRE((flags & kAudioUnitRenderAction_OutputIsSilence) == 0);

    // Unrelated flags the host may have set are preserved: we retract exactly
    // one claim, not the whole word.
    flags = kAudioUnitRenderAction_OutputIsSilence |
            kAudioUnitRenderAction_PreRender;
    REQUIRE(effect.ProcessBufferLists(flags, input.bl, output.bl, kFrames) == noErr);
    REQUIRE((flags & kAudioUnitRenderAction_OutputIsSilence) == 0);
    REQUIRE((flags & kAudioUnitRenderAction_PreRender) != 0);

    effect.DoCleanup();
}
