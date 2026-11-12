#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"
#include <pulp/format/preset_program_router.hpp>
#include <pulp/midi/buffer.hpp>

#include <string>
#include <vector>

using namespace pulp::format;
using namespace pulp::midi;
using pulp::state::BankProgram;

namespace {

void enable(PresetProgramRouter& r, std::uint8_t channel = 0,
            BankSelectMode mode = BankSelectMode::msb_lsb) {
    r.set_settings({true, channel, mode});
}

void send(PresetProgramRouter& r, std::initializer_list<std::vector<std::uint8_t>> messages) {
    for (const auto& m : messages)
        r.on_message(m.data(), m.size());
}

constexpr std::uint8_t CC0 = 0xB0, PC0 = 0xC0;

} // namespace

TEST_CASE("a disabled router ignores Program Change and Bank Select", "[format][preset][program]") {
    PresetProgramRouter r;
    send(r, {{CC0, 0, 1}, {PC0, 5}});
    REQUIRE_FALSE(r.take_request().has_value());
    REQUIRE_FALSE(r.claims_controller(0));
}

TEST_CASE("Program Change alone addresses bank 0", "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r);
    send(r, {{PC0, 5}});
    auto req = r.take_request();
    REQUIRE(req.has_value());
    REQUIRE(req->position == BankProgram{0, 5});
}

TEST_CASE("Bank Select MSB and LSB form a 14-bit bank in either order",
          "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r);
    send(r, {{CC0, 0, 1}, {CC0, 32, 2}, {PC0, 7}});
    REQUIRE(r.take_request()->position == BankProgram{130, 7});

    PresetProgramRouter lsb_first;
    enable(lsb_first);
    send(lsb_first, {{CC0, 32, 2}, {CC0, 0, 1}, {PC0, 7}});
    REQUIRE(lsb_first.take_request()->position == BankProgram{130, 7});
}

TEST_CASE("the latched bank applies to later Program Changes", "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r);
    send(r, {{CC0, 0, 3}, {PC0, 1}});
    REQUIRE(r.take_request()->position == BankProgram{384, 1});
    send(r, {{PC0, 2}}); // no new Bank Select: the latch stays
    REQUIRE(r.take_request()->position == BankProgram{384, 2});
}

TEST_CASE("bank latches are per channel in omni mode", "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r);
    send(r, {{CC0, 32, 1}, {static_cast<std::uint8_t>(CC0 | 1), 32, 2}});
    send(r, {{PC0, 0}});
    REQUIRE(r.take_request()->position.bank == 1);
    send(r, {{static_cast<std::uint8_t>(PC0 | 1), 0}});
    REQUIRE(r.take_request()->position.bank == 2);
}

TEST_CASE("a channel filter ignores every other channel, controllers included",
          "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r, 3);                      // channel 3 is zero-based channel 2
    send(r, {{CC0, 32, 9}, {PC0, 4}}); // channel 1: ignored
    REQUIRE_FALSE(r.take_request().has_value());
    send(r, {{static_cast<std::uint8_t>(CC0 | 2), 32, 5}, {static_cast<std::uint8_t>(PC0 | 2), 4}});
    REQUIRE(r.take_request()->position == BankProgram{5, 4});
    // The ignored channel's Bank Select did not leak into channel 3's latch.
    REQUIRE(r.take_request() == std::nullopt);
}

TEST_CASE("bank modes", "[format][preset][program]") {
    PresetProgramRouter msb_only;
    enable(msb_only, 0, BankSelectMode::msb_only);
    send(msb_only, {{CC0, 0, 9}, {CC0, 32, 5}, {PC0, 1}});
    REQUIRE(msb_only.take_request()->position == BankProgram{9, 1});
    REQUIRE(msb_only.claims_controller(0));
    REQUIRE_FALSE(msb_only.claims_controller(32));

    PresetProgramRouter ignore;
    enable(ignore, 0, BankSelectMode::ignore_bank);
    send(ignore, {{CC0, 0, 9}, {CC0, 32, 5}, {PC0, 1}});
    REQUIRE(ignore.take_request()->position == BankProgram{0, 1});
    REQUIRE_FALSE(ignore.claims_controller(0));

    PresetProgramRouter full;
    enable(full);
    REQUIRE(full.claims_controller(0));
    REQUIRE(full.claims_controller(32));
    REQUIRE_FALSE(full.claims_controller(7));
}

TEST_CASE("a burst of Program Changes leaves only the newest request",
          "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r);
    MidiBuffer block;
    for (std::uint8_t p = 1; p <= 5; ++p) {
        block.add(MidiEvent::cc(0, 32, p));
        block.add({choc::midi::ShortMessage(PC0, p, 0), 0, 0.0});
    }
    r.process(block);
    auto req = r.take_request();
    REQUIRE(req.has_value());
    REQUIRE(req->position == BankProgram{5, 5});
    REQUIRE(req->sequence == 5);
    REQUIRE_FALSE(r.take_request().has_value()); // taking empties the slot

    // The sequence keeps counting across blocks.
    send(r, {{PC0, 9}});
    REQUIRE(r.take_request()->sequence == 6);
}

TEST_CASE("Reset All Controllers leaves Bank Select alone, reset() clears it",
          "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r);
    send(r, {{CC0, 32, 4}, {CC0, 121, 0}, {PC0, 0}});
    REQUIRE(r.take_request()->position.bank == 4);
    r.reset();
    send(r, {{PC0, 0}});
    REQUIRE(r.take_request()->position.bank == 0);
}

TEST_CASE("short and unrelated messages are ignored", "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r);
    send(r, {{PC0}, {CC0, 0}, {0x90, 60, 100}, {0xE0, 0, 64}});
    REQUIRE_FALSE(r.take_request().has_value());
    // A Program Change with data bytes above 127 is masked, not mis-decoded.
    send(r, {{PC0, 0x85}});
    REQUIRE(r.take_request()->position.program == 5);
}

TEST_CASE("the router passes the MIDI through untouched", "[format][preset][program]") {
    PresetProgramRouter r;
    enable(r);
    MidiBuffer block;
    block.add(MidiEvent::note_on(0, 60, 100));
    block.add({choc::midi::ShortMessage(PC0, 3, 0), 0, 0.0});
    r.process(block);
    REQUIRE(block.size() == 2);
    REQUIRE(block[1].data()[0] == PC0);
}

TEST_CASE("processing a block allocates nothing", "[format][preset][program][rt-safety]") {
    PresetProgramRouter r;
    enable(r);
    MidiBuffer block;
    block.reserve_events(64);
    for (std::uint8_t i = 0; i < 16; ++i) {
        block.add(MidiEvent::cc(0, 32, i));
        block.add({choc::midi::ShortMessage(PC0, i, 0), 0, 0.0});
    }

    // Positive control: the probe does see an allocation, so a zero below is a
    // measurement and not a probe that cannot see.
    {
        pulp::test::RtAllocationProbe control;
        // A direct operator new call: the compiler may elide a new-expression
        // whose pointer never escapes, which would blind this control.
        void* block_of_memory = ::operator new(64);
        ::operator delete(block_of_memory);
        REQUIRE(control.saw_allocation());
    }

    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 100; ++i)
        r.process(block);
    r.take_request();
    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("the router reserves the sources it consumes, by mode and channel",
          "[format][preset][program][reservation]") {
    using pulp::state::MidiSource;
    using pulp::state::MidiSourceKind;
    auto src = [](MidiSourceKind k, std::uint16_t n, std::uint8_t ch = MidiSource::kOmni) {
        return MidiSource{k, ch, n};
    };

    PresetProgramRouter off;
    REQUIRE_FALSE(off.claims(src(MidiSourceKind::cc7, 0)));
    REQUIRE_FALSE(off.claims(src(MidiSourceKind::program_change, 3)));

    PresetProgramRouter r;
    enable(r);
    REQUIRE(r.claims(src(MidiSourceKind::cc7, 0)));
    REQUIRE(r.claims(src(MidiSourceKind::cc7, 32)));
    REQUIRE(r.claims(src(MidiSourceKind::cc14, 0)));
    REQUIRE(r.claims(src(MidiSourceKind::program_change, 99)));
    REQUIRE_FALSE(r.claims(src(MidiSourceKind::cc7, 74)));
    REQUIRE_FALSE(r.claims(src(MidiSourceKind::cc14, 1)));
    REQUIRE_FALSE(r.claims(src(MidiSourceKind::pitch_bend, 0)));
    REQUIRE(std::string(r.reason()).size() > 0);

    PresetProgramRouter msb;
    enable(msb, 0, BankSelectMode::msb_only);
    REQUIRE(msb.claims(src(MidiSourceKind::cc7, 0)));
    REQUIRE_FALSE(msb.claims(src(MidiSourceKind::cc7, 32)));
    REQUIRE_FALSE(msb.claims(src(MidiSourceKind::cc14, 0)));

    PresetProgramRouter ignore;
    enable(ignore, 0, BankSelectMode::ignore_bank);
    REQUIRE_FALSE(ignore.claims(src(MidiSourceKind::cc7, 0)));
    REQUIRE(ignore.claims(src(MidiSourceKind::program_change, 0)));

    // A single-channel router (channel 3 = zero-based 2) reserves that channel
    // and any omni source, and nothing on other channels.
    PresetProgramRouter ch3;
    enable(ch3, 3);
    REQUIRE(ch3.claims(src(MidiSourceKind::cc7, 0, 2)));
    REQUIRE(ch3.claims(src(MidiSourceKind::cc7, 0))); // omni overlaps channel 3
    REQUIRE_FALSE(ch3.claims(src(MidiSourceKind::cc7, 0, 5)));
    REQUIRE_FALSE(ch3.claims(src(MidiSourceKind::program_change, 1, 0)));
}
