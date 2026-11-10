// MidiLearnEngine: mapping publication, routing for every source kind,
// relative encoders, takeover, the UI-thread learn flow, real-time allocation
// safety, and a UI-versus-audio-thread hammer.

#include <pulp/state/midi_feedback.hpp>
#include <pulp/state/midi_learn_engine.hpp>
#include <pulp/state/store.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <thread>

using namespace pulp::state;
using Catch::Matchers::WithinAbs;

namespace {

constexpr ParamID kMix = 1;
constexpr ParamID kPan = 2;
constexpr ParamID kGain = 3;

void populate(StateStore& s) {
    s.add_parameter({.id = kMix, .name = "Mix", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    s.add_parameter({.id = kPan, .name = "Pan", .unit = "", .range = {-1.0f, 1.0f, 0.0f, 0.0f}});
    s.add_parameter({.id = kGain, .name = "Gain", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
}

RawMidi cc(uint8_t ch, uint8_t n, uint8_t v) {
    return {static_cast<uint8_t>(0xB0 | ch), n, v};
}

MidiMapping map_to(MidiSource src, ParamID id) {
    MidiMapping m;
    m.source = src;
    m.id = id;
    return m;
}

float norm(StateStore& s, ParamID id) {
    return s.get_normalized(id);
}

// One block with one message.
void send(MidiLearnEngine& e, StateStore& s, const RawMidi& m) {
    e.begin_block();
    e.route(s, m);
}

} // namespace

TEST_CASE("engine: nothing mapped changes nothing", "[state][midi-learn][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    e.begin_block();
    CHECK_FALSE(e.route(s, cc(0, 74, 127)));
    CHECK_THAT(norm(s, kMix), WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("engine: a 7-bit CC drives its parameter through the window",
          "[state][midi-learn][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    auto m = map_to({MidiSourceKind::cc7, 0, 74}, kMix);
    m.scale = {0.25f, 0.75f};
    REQUIRE(e.set_mapping(m));

    send(e, s, cc(0, 74, 127));
    CHECK_THAT(norm(s, kMix), WithinAbs(0.75f, 1e-5f));
    send(e, s, cc(0, 74, 0));
    CHECK_THAT(norm(s, kMix), WithinAbs(0.25f, 1e-5f));

    // Wrong channel or number does not drive it.
    send(e, s, cc(1, 74, 127));
    send(e, s, cc(0, 75, 127));
    CHECK_THAT(norm(s, kMix), WithinAbs(0.25f, 1e-5f));

    // Inverted window.
    auto inv = map_to({MidiSourceKind::cc7, MidiSource::kOmni, 7}, kGain);
    inv.scale = {1.0f, 0.0f};
    REQUIRE(e.set_mapping(inv));
    send(e, s, cc(5, 7, 127));
    CHECK_THAT(norm(s, kGain), WithinAbs(0.0f, 1e-5f));
    send(e, s, cc(9, 7, 0));
    CHECK_THAT(norm(s, kGain), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("engine: edits publish on the next block, not before", "[state][midi-learn][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    e.begin_block();
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 1}, kMix)));
    CHECK_FALSE(e.route(s, cc(0, 1, 127))); // still the old table inside this block
    e.begin_block();
    CHECK(e.route(s, cc(0, 1, 127)));

    e.clear(kMix);
    e.begin_block();
    CHECK_FALSE(e.route(s, cc(0, 1, 0)));
}

TEST_CASE("engine: 14-bit CC uses the LSB when present", "[state][midi-learn][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc14, 0, 1}, kMix)));

    // MSB alone: 7-bit, full range.
    send(e, s, cc(0, 1, 127));
    CHECK_THAT(norm(s, kMix), WithinAbs(1.0f, 1e-5f));

    // With LSB: 14-bit resolution.
    send(e, s, cc(0, 33, 0));
    send(e, s, cc(0, 1, 64));
    CHECK_THAT(norm(s, kMix), WithinAbs((64 << 7) / 16383.0f, 1e-5f));
    send(e, s, cc(0, 33, 1));
    CHECK_THAT(norm(s, kMix), WithinAbs(((64 << 7) | 1) / 16383.0f, 1e-5f));
}

TEST_CASE("engine: NRPN, pitch bend and pressure sources", "[state][midi-learn][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::nrpn, 0, (3 << 7) | 4}, kMix)));
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::pitch_bend, 0, 0}, kPan)));
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::channel_pressure, 0, 0}, kGain)));
    e.begin_block();

    e.route(s, cc(0, 99, 3));
    e.route(s, cc(0, 98, 4));
    e.route(s, cc(0, 6, 127));
    CHECK_THAT(norm(s, kMix), WithinAbs(1.0f, 1e-5f));

    e.route(s, RawMidi{0xE0, 0x7F, 0x7F});
    CHECK_THAT(norm(s, kPan), WithinAbs(1.0f, 1e-5f));
    e.route(s, RawMidi{0xE0, 0x00, 0x00});
    CHECK_THAT(norm(s, kPan), WithinAbs(0.0f, 1e-5f));

    e.route(s, RawMidi{0xD0, 127, 0});
    CHECK_THAT(norm(s, kGain), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("engine: relative encoders move the parameter by ticks",
          "[state][midi-learn][engine][relative]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    auto m = map_to({MidiSourceKind::cc7, 0, 16}, kMix);
    m.mode = RelativeMode::twos_complement;
    m.relative = {0.01f, 0.0f};
    REQUIRE(e.set_mapping(m));

    send(e, s, cc(0, 16, 5)); // +5
    CHECK_THAT(norm(s, kMix), WithinAbs(0.55f, 1e-5f));
    send(e, s, cc(0, 16, 127)); // -1
    CHECK_THAT(norm(s, kMix), WithinAbs(0.54f, 1e-5f));

    // Offset-64, with an inverted window and clamping at the window's ends.
    auto o = map_to({MidiSourceKind::cc7, 0, 17}, kGain);
    o.mode = RelativeMode::binary_offset;
    o.relative = {0.1f, 0.0f};
    o.scale = {0.8f, 0.2f};
    REQUIRE(e.set_mapping(o));
    send(e, s, cc(0, 17, 66));                          // +2 ticks, inverted => down
    CHECK_THAT(norm(s, kGain), WithinAbs(0.2f, 1e-5f)); // clamped at the window floor
    for (int i = 0; i < 20; ++i)
        send(e, s, cc(0, 17, 62));                      // -2 each => up
    CHECK_THAT(norm(s, kGain), WithinAbs(0.8f, 1e-5f)); // clamped at the window ceiling
}

TEST_CASE("engine: NRPN increment and decrement are relative",
          "[state][midi-learn][engine][relative]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    auto m = map_to({MidiSourceKind::nrpn, 0, 5}, kMix);
    m.relative = {0.1f, 0.0f};
    REQUIRE(e.set_mapping(m));
    e.begin_block();
    e.route(s, cc(0, 99, 0));
    e.route(s, cc(0, 98, 5));
    e.route(s, cc(0, 96, 0));
    CHECK_THAT(norm(s, kMix), WithinAbs(0.6f, 1e-5f));
    e.route(s, cc(0, 97, 2));
    CHECK_THAT(norm(s, kMix), WithinAbs(0.4f, 1e-5f));
}

TEST_CASE("engine: pickup holds the parameter until the controller reaches it",
          "[state][midi-learn][engine][takeover]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    auto m = map_to({MidiSourceKind::cc7, 0, 74}, kMix);
    m.takeover = TakeoverMode::pickup;
    REQUIRE(e.set_mapping(m));

    send(e, s, cc(0, 74, 10)); // far below 0.5
    send(e, s, cc(0, 74, 30));
    CHECK_THAT(norm(s, kMix), WithinAbs(0.5f, 1e-6f)); // untouched
    send(e, s, cc(0, 74, 80));                         // crossed
    CHECK_THAT(norm(s, kMix), WithinAbs(80 / 127.0f, 1e-5f));
    send(e, s, cc(0, 74, 100));
    CHECK_THAT(norm(s, kMix), WithinAbs(100 / 127.0f, 1e-5f));

    // Automation moves the parameter away: the controller must pick up again.
    s.set_normalized_rt(kMix, 0.1f);
    send(e, s, cc(0, 74, 120));
    CHECK_THAT(norm(s, kMix), WithinAbs(0.1f, 1e-5f));
    send(e, s, cc(0, 74, 5)); // crosses 0.1 on the way down
    CHECK_THAT(norm(s, kMix), WithinAbs(5 / 127.0f, 1e-5f));
}

TEST_CASE("engine: takeover state follows a mapping across table edits",
          "[state][midi-learn][engine][takeover]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    auto m = map_to({MidiSourceKind::cc7, 0, 74}, kMix);
    m.takeover = TakeoverMode::pickup;
    REQUIRE(e.set_mapping(m));
    send(e, s, cc(0, 74, 10));
    send(e, s, cc(0, 74, 90)); // engaged
    REQUIRE_THAT(norm(s, kMix), WithinAbs(90 / 127.0f, 1e-5f));

    // Unrelated edit: the existing mapping stays engaged.
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 1}, kGain)));
    send(e, s, cc(0, 74, 20));
    CHECK_THAT(norm(s, kMix), WithinAbs(20 / 127.0f, 1e-5f));
}

TEST_CASE("engine: table capacity and replacement rules", "[state][midi-learn][engine]") {
    MidiLearnEngine e;
    for (uint32_t i = 0; i < kMaxMidiMappings; ++i)
        REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, static_cast<uint16_t>(i)}, 1)));
    CHECK_FALSE(e.set_mapping(map_to({MidiSourceKind::poly_pressure, 0, 1}, 1)));
    CHECK(e.mappings().size() == kMaxMidiMappings);

    // The same source replaces in place even when full.
    auto again = map_to({MidiSourceKind::cc7, 0, 3}, 2);
    REQUIRE(e.set_mapping(again));
    CHECK(e.mappings().size() == kMaxMidiMappings);

    CHECK(e.clear(2).size() == 1);
    CHECK(e.clear_source({MidiSourceKind::cc7, 0, 4}).size() == 1);
    CHECK(e.clear_all().size() == kMaxMidiMappings - 2);
    CHECK(e.mappings().empty());
}

TEST_CASE("learn: bind after the controller settles", "[state][midi-learn][engine][learn]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    e.begin_learn(kMix, 1000);
    CHECK(e.learn_state().phase == MidiLearnState::Phase::listening);
    CHECK(e.learn_state().param == kMix);

    send(e, s, cc(3, 74, 20));
    send(e, s, cc(3, 74, 60));
    CHECK_FALSE(e.poll_learn(1100).has_value()); // still within the settle time
    send(e, s, cc(3, 74, 90));
    CHECK_FALSE(e.poll_learn(1200).has_value());

    auto out = e.poll_learn(1500);
    REQUIRE(out.has_value());
    CHECK(out->bound);
    CHECK(out->mapping.source == MidiSource{MidiSourceKind::cc7, 3, 74});
    CHECK(out->mapping.id == kMix);
    CHECK(out->replaced.empty());
    CHECK(e.learn_state().phase == MidiLearnState::Phase::bound);
    CHECK(e.learn_state().source == out->mapping.source);

    // The mapping works from the next block, and learning stopped.
    send(e, s, cc(3, 74, 127));
    CHECK_THAT(norm(s, kMix), WithinAbs(1.0f, 1e-5f));
    send(e, s, cc(3, 80, 5));
    CHECK_FALSE(e.poll_learn(5000).has_value());
}

TEST_CASE("learn: omni option, relative detection and replacement report",
          "[state][midi-learn][engine][learn]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 1}, kMix))); // same parameter
    REQUIRE(
        e.set_mapping(map_to({MidiSourceKind::cc7, 2, 16}, kGain))); // same source as the new one

    MidiLearnOptions opt;
    opt.omni = true;
    e.begin_learn(kMix, 0, opt);
    for (uint8_t v : {1, 127, 1, 1, 127, 1})
        send(e, s, cc(2, 16, v));
    auto out = e.poll_learn(10);
    CHECK_FALSE(out.has_value());
    out = e.poll_learn(1000);
    REQUIRE(out.has_value());
    CHECK(out->mapping.source == MidiSource{MidiSourceKind::cc7, MidiSource::kOmni, 16});
    CHECK(out->mapping.mode == RelativeMode::twos_complement);
    CHECK(out->replaced.size() == 2);

    CHECK(e.mappings().size() == 1);
    CHECK(e.mapping_for(kMix)->source.number == 16);
    CHECK_FALSE(e.mapping_for(kGain).has_value());
}

TEST_CASE("learn: cancel, timeout, restart and non-learnable input",
          "[state][midi-learn][engine][learn]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;

    e.begin_learn(kMix, 0);
    send(e, s, cc(0, 5, 10));
    e.cancel_learn();
    CHECK(e.learn_state().phase == MidiLearnState::Phase::idle);
    CHECK_FALSE(e.poll_learn(10000).has_value());
    CHECK(e.mappings().empty());

    // Nothing arrives: the listen times out.
    MidiLearnOptions opt;
    opt.timeout_ms = 500;
    e.begin_learn(kMix, 0, opt);
    CHECK_FALSE(e.poll_learn(499).has_value());
    CHECK(e.learn_state().phase == MidiLearnState::Phase::listening);
    CHECK_FALSE(e.poll_learn(500).has_value());
    CHECK(e.learn_state().phase == MidiLearnState::Phase::idle);

    // Notes are not learnable; a new begin_learn discards stale messages.
    e.begin_learn(kMix, 0);
    send(e, s, RawMidi{0x90, 60, 100});
    CHECK_FALSE(e.poll_learn(1000).has_value());
    e.begin_learn(kGain, 2000);
    send(e, s, cc(0, 9, 100));
    CHECK_FALSE(e.poll_learn(2100).has_value());
    auto out = e.poll_learn(3000);
    REQUIRE(out.has_value());
    CHECK(out->mapping.id == kGain);
    CHECK(out->mapping.source.number == 9);
}

TEST_CASE("learn: an NRPN is learned from selection plus data",
          "[state][midi-learn][engine][learn]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    e.begin_learn(kPan, 0);
    e.begin_block();
    e.route(s, cc(1, 99, 2));
    e.route(s, cc(1, 98, 3));
    e.route(s, cc(1, 6, 50));
    CHECK_FALSE(e.poll_learn(100).has_value());
    auto out = e.poll_learn(1000);
    REQUIRE(out.has_value());
    CHECK(out->mapping.source == MidiSource{MidiSourceKind::nrpn, 1, (2 << 7) | 3});
}

TEST_CASE("learn: existing mappings keep working while listening",
          "[state][midi-learn][engine][learn]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 1}, kGain)));
    e.begin_learn(kMix, 0);
    send(e, s, cc(0, 1, 127));
    CHECK_THAT(norm(s, kGain), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("engine: routing and block adoption allocate nothing",
          "[state][midi-learn][engine][rt-safety]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 74}, kMix)));
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc14, 0, 1}, kPan)));
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::nrpn, 0, 5}, kGain)));
    e.begin_block();
    e.begin_learn(kMix, 0);
    s.set_normalized_rt(kMix, 0.3f); // warm parameter-index lookups

    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 200; ++i) {
            e.begin_block();
            e.route(s, cc(0, 74, static_cast<uint8_t>(i & 127)));
            e.route(s, cc(0, 1, 64));
            e.route(s, cc(0, 33, 5));
            e.route(s, cc(0, 99, 0));
            e.route(s, cc(0, 98, 5));
            e.route(s, cc(0, 96, 0));
            e.route(s, RawMidi{0xE0, 1, 2});
        }
        CHECK(probe.allocation_count() == 0);
    }

    // Adopting a freshly published table is allocation-free on the audio side.
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 20}, kGain)));
    {
        pulp::test::RtAllocationProbe probe;
        e.begin_block();
        e.route(s, cc(0, 20, 100));
        CHECK(probe.allocation_count() == 0);
    }
}

TEST_CASE("engine: UI edits and learning race the audio thread safely",
          "[state][midi-learn][engine][rt-safety]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 74}, kMix)));

    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};
    std::thread audio([&] {
        uint8_t v = 0;
        while (!stop.load(std::memory_order_acquire)) {
            e.begin_block();
            for (int i = 0; i < 8; ++i) {
                e.route(s, cc(0, static_cast<uint8_t>(60 + (v & 31)), v));
                ++v;
            }
            for (ParamID id : {kMix, kPan, kGain}) {
                const float x = s.get_normalized(id);
                if (!(x >= 0.0f && x <= 1.0f))
                    bad.store(true);
            }
        }
    });

    uint64_t now = 0;
    for (int i = 0; i < 3000; ++i) {
        const auto src = MidiSource{MidiSourceKind::cc7, 0, static_cast<uint16_t>(60 + (i % 32))};
        switch (i % 5) {
        case 0:
            e.set_mapping(map_to(src, kMix + (i % 3)));
            break;
        case 1:
            e.clear_source(src);
            break;
        case 2:
            e.begin_learn(kGain, now);
            break;
        case 3:
            e.poll_learn(now += 300);
            break;
        default:
            e.cancel_learn();
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    CHECK_FALSE(bad.load());

    // After the storm the engine still maps correctly.
    e.clear_all();
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 74}, kMix)));
    send(e, s, cc(0, 74, 127));
    CHECK_THAT(norm(s, kMix), WithinAbs(1.0f, 1e-5f));
}

// ── Feedback ──

namespace {

struct Sent {
    std::vector<RawMidi> messages;
    void operator()(const RawMidi& m) {
        messages.push_back(m);
    }
};

MidiMapping fb(MidiSource src, ParamID id) {
    auto m = map_to(src, id);
    m.feedback = true;
    return m;
}

constexpr auto kAllKinds = kFeedbackEncodableKinds;

} // namespace

TEST_CASE("feedback encoding: each kind produces the controller's own message",
          "[state][midi-learn][feedback]") {
    RawMidi out[kMaxFeedbackMessages];

    auto m = map_to({MidiSourceKind::cc7, 3, 74}, 1);
    REQUIRE(encode_feedback(m, 1.0f, out) == 1);
    CHECK((out[0] == RawMidi{0xB3, 74, 127}));
    REQUIRE(encode_feedback(m, 0.5f, out) == 1);
    CHECK(out[0].data2 == 64);

    m = map_to({MidiSourceKind::cc14, 0, 7}, 1);
    REQUIRE(encode_feedback(m, 1.0f, out) == 2);
    CHECK((out[0] == RawMidi{0xB0, 7, 127}));
    CHECK((out[1] == RawMidi{0xB0, 39, 127}));
    REQUIRE(encode_feedback(m, 8193.0f / 16383.0f, out) == 2);
    CHECK(out[0].data2 == 64);
    CHECK(out[1].data2 == 1);

    m = map_to({MidiSourceKind::nrpn, 1, (3 << 7) | 4}, 1);
    REQUIRE(encode_feedback(m, 1.0f, out) == 4);
    CHECK((out[0] == RawMidi{0xB1, 99, 3}));
    CHECK((out[1] == RawMidi{0xB1, 98, 4}));
    CHECK((out[2] == RawMidi{0xB1, 6, 127}));
    CHECK((out[3] == RawMidi{0xB1, 38, 127}));

    m = map_to({MidiSourceKind::rpn, 0, 2}, 1);
    REQUIRE(encode_feedback(m, 0.0f, out) == 4);
    CHECK(out[0].data1 == 101);
    CHECK(out[1].data1 == 100);

    m = map_to({MidiSourceKind::pitch_bend, 2, 0}, 1);
    REQUIRE(encode_feedback(m, 1.0f, out) == 1);
    CHECK((out[0] == RawMidi{0xE2, 127, 127}));

    m = map_to({MidiSourceKind::channel_pressure, 0, 0}, 1);
    REQUIRE(encode_feedback(m, 1.0f, out) == 1);
    CHECK((out[0] == RawMidi{0xD0, 127, 0}));

    m = map_to({MidiSourceKind::poly_pressure, 0, 61}, 1);
    REQUIRE(encode_feedback(m, 1.0f, out) == 1);
    CHECK((out[0] == RawMidi{0xA0, 61, 127}));

    // An omni mapping sends on channel 1. Program change is a trigger, not a position.
    m = map_to({MidiSourceKind::cc7, MidiSource::kOmni, 9}, 1);
    REQUIRE(encode_feedback(m, 1.0f, out) == 1);
    CHECK(out[0].channel() == 0);
    m = map_to({MidiSourceKind::program_change, 0, 3}, 1);
    CHECK(encode_feedback(m, 1.0f, out) == 0);
}

TEST_CASE("feedback: the controller position inverts the window", "[state][midi-learn][feedback]") {
    auto m = map_to({MidiSourceKind::cc7, 0, 1}, 1);
    m.scale = {0.25f, 0.75f};
    CHECK_THAT(*controller_position(m, 0.5f), WithinAbs(0.5f, 1e-6f));
    CHECK_THAT(*controller_position(m, 0.75f), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(*controller_position(m, 0.0f), WithinAbs(0.0f, 1e-6f)); // outside: clamps

    m.scale = {1.0f, 0.0f}; // inverted
    CHECK_THAT(*controller_position(m, 1.0f), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(*controller_position(m, 0.25f), WithinAbs(0.75f, 1e-6f));

    m.scale = {0.5f, 0.5f}; // flat: no position
    CHECK_FALSE(controller_position(m, 0.5f).has_value());
}

TEST_CASE("feedback: first call syncs, then only changes are sent",
          "[state][midi-learn][feedback][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(fb({MidiSourceKind::cc7, 0, 74}, kMix)));
    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 75}, kGain))); // feedback off
    e.begin_block();

    Sent sent;
    e.emit_feedback(s, kAllKinds, sent);
    REQUIRE(sent.messages.size() == 1);
    CHECK((sent.messages[0] == RawMidi{0xB0, 74, 64})); // Mix default 0.5

    sent.messages.clear();
    e.emit_feedback(s, kAllKinds, sent);
    CHECK(sent.messages.empty());

    // Below half a step of the controller's resolution: nothing.
    s.set_normalized_rt(kMix, 0.5f + 0.2f / 127.0f);
    e.emit_feedback(s, kAllKinds, sent);
    CHECK(sent.messages.empty());

    s.set_normalized_rt(kMix, 1.0f);
    e.emit_feedback(s, kAllKinds, sent);
    REQUIRE(sent.messages.size() == 1);
    CHECK(sent.messages[0].data2 == 127);

    // At most one update per mapping per call, however far it moved.
    sent.messages.clear();
    s.set_normalized_rt(kMix, 0.1f);
    s.set_normalized_rt(kMix, 0.9f);
    e.emit_feedback(s, kAllKinds, sent);
    CHECK(sent.messages.size() == 1);
}

TEST_CASE("feedback: a value the controller just wrote is not echoed back",
          "[state][midi-learn][feedback][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(fb({MidiSourceKind::cc7, 0, 74}, kMix)));
    e.begin_block();
    Sent sent;
    e.emit_feedback(s, kAllKinds, sent); // initial sync
    sent.messages.clear();

    e.begin_block();
    e.route(s, cc(0, 74, 100)); // the controller moves the parameter
    e.emit_feedback(s, kAllKinds, sent);
    CHECK(sent.messages.empty());

    // Automation moves it afterwards: that is sent.
    s.set_normalized_rt(kMix, 0.2f);
    e.emit_feedback(s, kAllKinds, sent);
    REQUIRE(sent.messages.size() == 1);
    CHECK(sent.messages[0].data2 == static_cast<uint8_t>(std::lround(0.2f * 127.0f)));

    // The parameter returns to the value the controller wrote (reset, preset
    // recall, automation): it is sent, so the fader does not stay at 0.2.
    sent.messages.clear();
    s.set_normalized_rt(kMix, 100.0f / 127.0f);
    e.emit_feedback(s, kAllKinds, sent);
    REQUIRE(sent.messages.size() == 1);
    CHECK(sent.messages[0].data2 == 100);

    // A relative encoder's move is not echoed either.
    auto r = fb({MidiSourceKind::cc7, 0, 16}, kGain);
    r.mode = RelativeMode::twos_complement;
    r.relative = {0.05f, 0.0f};
    REQUIRE(e.set_mapping(r));
    e.begin_block();
    sent.messages.clear();
    e.emit_feedback(s, kAllKinds, sent); // sync for the new mapping
    sent.messages.clear();
    e.route(s, cc(0, 16, 3));
    e.emit_feedback(s, kAllKinds, sent);
    CHECK(sent.messages.empty());
}

TEST_CASE("feedback: only kinds the format can carry are sent",
          "[state][midi-learn][feedback][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(fb({MidiSourceKind::cc7, 0, 74}, kMix)));
    REQUIRE(e.set_mapping(fb({MidiSourceKind::cc14, 0, 1}, kPan)));
    REQUIRE(e.set_mapping(fb({MidiSourceKind::nrpn, 0, 5}, kGain)));
    e.begin_block();

    Sent sent;
    e.emit_feedback(s, MidiSourceKindMask::of({MidiSourceKind::cc7}), sent);
    REQUIRE(sent.messages.size() == 1);
    CHECK(sent.messages[0].data1 == 74);

    // Nothing allowed: nothing sent, and nothing is remembered as sent.
    MidiLearnEngine none;
    REQUIRE(none.set_mapping(fb({MidiSourceKind::cc7, 0, 74}, kMix)));
    none.begin_block();
    Sent nothing;
    none.emit_feedback(s, MidiSourceKindMask{}, nothing);
    CHECK(nothing.messages.empty());
    none.emit_feedback(s, kAllKinds, nothing);
    CHECK(nothing.messages.size() == 1);

    // Multi-message kinds send their whole sequence.
    Sent all;
    MidiLearnEngine e2;
    REQUIRE(e2.set_mapping(fb({MidiSourceKind::nrpn, 0, 5}, kGain)));
    e2.begin_block();
    e2.emit_feedback(s, kAllKinds, all);
    CHECK(all.messages.size() == 4);
}

TEST_CASE("feedback: sent state survives a table edit by uid",
          "[state][midi-learn][feedback][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(fb({MidiSourceKind::cc7, 0, 74}, kMix)));
    e.begin_block();
    Sent sent;
    e.emit_feedback(s, kAllKinds, sent);
    sent.messages.clear();

    REQUIRE(e.set_mapping(map_to({MidiSourceKind::cc7, 0, 1}, kGain))); // unrelated edit
    e.begin_block();
    e.emit_feedback(s, kAllKinds, sent);
    CHECK(sent.messages.empty()); // no repeat of the initial sync
}

TEST_CASE("learn: a host-reported controller binds without a burst",
          "[state][midi-learn][engine][learn]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;

    // Not listening: ignored.
    e.host_reported_controller({MidiSourceKind::cc7, 2, 74});
    CHECK_FALSE(e.poll_learn(100).has_value());

    e.begin_learn(kMix, 0);
    CHECK(e.learning());
    e.host_reported_controller({MidiSourceKind::cc7, 2, 74});
    auto out = e.poll_learn(1);
    REQUIRE(out.has_value());
    CHECK(out->bound);
    CHECK(out->mapping.source == MidiSource{MidiSourceKind::cc7, 2, 74});
    CHECK(out->mapping.mode == RelativeMode::absolute);
    CHECK_FALSE(e.learning());

    // An out-of-range report is ignored and the listen continues.
    e.begin_learn(kGain, 0);
    e.host_reported_controller({MidiSourceKind::cc7, 20, 74}); // channel 20
    CHECK_FALSE(e.poll_learn(1).has_value());
    CHECK(e.learning());
    e.host_reported_controller({MidiSourceKind::pitch_bend, 0, 0});
    out = e.poll_learn(2);
    REQUIRE(out.has_value());
    CHECK(out->mapping.source.kind == MidiSourceKind::pitch_bend);

    // begin_learn discards a stale report.
    e.begin_learn(kPan, 0);
    e.host_reported_controller({MidiSourceKind::cc7, 0, 9});
    e.begin_learn(kPan, 10);
    CHECK_FALSE(e.poll_learn(11).has_value());
    e.cancel_learn();
}

TEST_CASE("feedback: emitting allocates nothing", "[state][midi-learn][feedback][rt-safety]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    REQUIRE(e.set_mapping(fb({MidiSourceKind::cc7, 0, 74}, kMix)));
    REQUIRE(e.set_mapping(fb({MidiSourceKind::nrpn, 0, 5}, kGain)));
    e.begin_block();
    s.set_normalized_rt(kMix, 0.3f);
    std::size_t count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 100; ++i) {
            s.set_normalized_rt(kMix, (i % 10) / 10.0f);
            e.begin_block();
            e.emit_feedback(s, kAllKinds, [&](const RawMidi&) { ++count; });
        }
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(count > 0);
}
