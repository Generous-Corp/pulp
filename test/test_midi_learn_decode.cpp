// MIDI controller decoding and learn classification: 7-bit CC, 14-bit CC pairs,
// NRPN/RPN data entry, pitch bend and pressure, the three relative-encoder
// conventions, learn-burst classification, and pickup/scale takeover. All pure
// functions over raw bytes, so no audio or StateStore is involved.

#include <pulp/state/midi_decode.hpp>
#include <pulp/state/midi_learn.hpp>
#include <pulp/state/midi_source.hpp>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

using namespace pulp::state;
using Catch::Matchers::WithinAbs;

namespace {

RawMidi cc(uint8_t ch, uint8_t n, uint8_t v) { return {static_cast<uint8_t>(0xB0 | ch), n, v}; }

std::vector<RawMidi> ccs(uint8_t ch, uint8_t n, std::initializer_list<int> values) {
    std::vector<RawMidi> out;
    for (int v : values) out.push_back(cc(ch, n, static_cast<uint8_t>(v)));
    return out;
}

} // namespace

TEST_CASE("decode_simple covers the stateless message kinds", "[state][midi-learn][decode]") {
    DecodedMidi d;

    REQUIRE(decode_simple(cc(2, 74, 127), d));
    CHECK(d.source == MidiSource{MidiSourceKind::cc7, 2, 74});
    CHECK_THAT(d.normalized(), WithinAbs(1.0f, 1e-6f));

    // Pitch bend: data1 = LSB, data2 = MSB. 8192 is centre.
    REQUIRE(decode_simple(RawMidi{0xE1, 0x00, 0x40}, d));
    CHECK(d.source.kind == MidiSourceKind::pitch_bend);
    CHECK(d.source.channel == 1);
    CHECK(d.raw == 8192);
    CHECK(d.max_raw == 16383);
    REQUIRE(decode_simple(RawMidi{0xE0, 0x7F, 0x7F}, d));
    CHECK_THAT(d.normalized(), WithinAbs(1.0f, 1e-6f));

    REQUIRE(decode_simple(RawMidi{0xD3, 100, 0}, d));
    CHECK(d.source == MidiSource{MidiSourceKind::channel_pressure, 3, 0});
    CHECK(d.raw == 100);

    REQUIRE(decode_simple(RawMidi{0xA0, 60, 90}, d));
    CHECK(d.source == MidiSource{MidiSourceKind::poly_pressure, 0, 60});
    CHECK(d.raw == 90);

    REQUIRE(decode_simple(RawMidi{0xC5, 12, 0}, d));
    CHECK(d.source == MidiSource{MidiSourceKind::program_change, 5, 12});

    CHECK_FALSE(decode_simple(RawMidi{0x90, 60, 100}, d)); // note on
    CHECK_FALSE(decode_simple(RawMidi{0xF8, 0, 0}, d));    // clock
}

TEST_CASE("decode_simple can leave data-entry CCs to the NRPN decoder", "[state][midi-learn][decode]") {
    DecodedMidi d;
    for (uint8_t n : {6, 38, 96, 97, 98, 99, 100, 101}) {
        CHECK(decode_simple(cc(0, n, 1), d, true));
        CHECK_FALSE(decode_simple(cc(0, n, 1), d, false));
    }
    CHECK(decode_simple(cc(0, 74, 1), d, false));
}

TEST_CASE("Cc14Decoder combines MSB and LSB", "[state][midi-learn][decode][cc14]") {
    Cc14Decoder dec;
    DecodedMidi d;

    // MSB alone decodes as 7 bits so MSB-only controllers reach full range.
    REQUIRE(dec.feed(cc(0, 1, 127), d));
    CHECK(d.source == MidiSource{MidiSourceKind::cc14, 0, 1});
    CHECK(d.max_raw == 127);
    CHECK_THAT(d.normalized(), WithinAbs(1.0f, 1e-6f));

    // Once an LSB is seen the pair is 14-bit.
    REQUIRE(dec.feed(cc(0, 33, 0x55), d));
    CHECK(d.max_raw == 16383);
    CHECK(d.raw == ((127 << 7) | 0x55));
    REQUIRE(dec.feed(cc(0, 1, 64), d)); // later MSB reuses the LSB
    CHECK(d.raw == ((64 << 7) | 0x55));

    CHECK_FALSE(dec.feed(cc(0, 74, 10), d)); // not a pair controller
    CHECK_FALSE(dec.feed(cc(0, 64, 10), d)); // sustain, outside 0..63
}

TEST_CASE("Cc14Decoder tolerates LSB-first and keeps channels apart", "[state][midi-learn][decode][cc14]") {
    Cc14Decoder dec;
    DecodedMidi d;

    CHECK_FALSE(dec.feed(cc(4, 32 + 7, 0x11), d)); // LSB first: held, nothing yet
    REQUIRE(dec.feed(cc(4, 7, 0x20), d));
    CHECK(d.source == MidiSource{MidiSourceKind::cc14, 4, 7});
    CHECK(d.raw == ((0x20 << 7) | 0x11));

    REQUIRE(dec.feed(cc(5, 7, 0x20), d)); // channel 5 never saw an LSB
    CHECK(d.max_raw == 127);

    dec.reset();
    REQUIRE(dec.feed(cc(4, 7, 0x20), d));
    CHECK(d.max_raw == 127);
}

TEST_CASE("NrpnDecoder reassembles selection and data entry", "[state][midi-learn][decode][nrpn]") {
    NrpnDecoder dec;
    DecodedMidi d;

    CHECK_FALSE(dec.feed(cc(0, 6, 10), d)); // nothing selected
    CHECK_FALSE(dec.feed(cc(0, 99, 9), d));
    CHECK_FALSE(dec.feed(cc(0, 6, 10), d)); // only the MSB half selected
    CHECK_FALSE(dec.feed(cc(0, 98, 3), d));

    // NRPN number = (9 << 7) | 3 = 1155. MSB alone is 7-bit.
    REQUIRE(dec.feed(cc(0, 6, 127), d));
    CHECK(d.source == MidiSource{MidiSourceKind::nrpn, 0, 1155});
    CHECK(d.max_raw == 127);
    CHECK_THAT(d.normalized(), WithinAbs(1.0f, 1e-6f));

    REQUIRE(dec.feed(cc(0, 38, 1), d)); // LSB makes it 14-bit
    CHECK(d.max_raw == 16383);
    CHECK(d.raw == ((127 << 7) | 1));

    // Selection persists across data entries until changed.
    REQUIRE(dec.feed(cc(0, 6, 5), d));
    CHECK(d.source.number == 1155);
}

TEST_CASE("NrpnDecoder forgets the data LSB when the selection changes", "[state][midi-learn][decode][nrpn]") {
    NrpnDecoder dec;
    DecodedMidi d;
    dec.feed(cc(1, 99, 0), d);
    dec.feed(cc(1, 98, 5), d);
    dec.feed(cc(1, 6, 10), d);
    REQUIRE(dec.feed(cc(1, 38, 3), d));
    CHECK(d.max_raw == 16383);

    dec.feed(cc(1, 98, 6), d); // new parameter
    REQUIRE(dec.feed(cc(1, 6, 10), d));
    CHECK(d.source.number == 6);
    CHECK(d.max_raw == 127);
}

TEST_CASE("NrpnDecoder: RPN, null RPN, increment and decrement, per channel", "[state][midi-learn][decode][nrpn]") {
    NrpnDecoder dec;
    DecodedMidi d;

    dec.feed(cc(2, 101, 0), d);
    dec.feed(cc(2, 100, 0), d); // RPN 0 (pitch bend sensitivity)
    REQUIRE(dec.feed(cc(2, 6, 12), d));
    CHECK(d.source == MidiSource{MidiSourceKind::rpn, 2, 0});

    REQUIRE(dec.feed(cc(2, 96, 0), d)); // increment, amount 0 counts as one
    CHECK(d.kind == DecodedMidi::Kind::step);
    CHECK(d.step == 1);
    REQUIRE(dec.feed(cc(2, 97, 3), d));
    CHECK(d.step == -3);
    CHECK_THAT(d.normalized(), WithinAbs(0.0f, 1e-9f));

    // Another channel has nothing selected.
    CHECK_FALSE(dec.feed(cc(3, 6, 12), d));

    // Null RPN deselects.
    dec.feed(cc(2, 101, 127), d);
    dec.feed(cc(2, 100, 127), d);
    CHECK_FALSE(dec.feed(cc(2, 6, 12), d));

    // Selecting NRPN after RPN replaces it (both halves required again).
    dec.feed(cc(2, 101, 0), d);
    dec.feed(cc(2, 100, 1), d);
    dec.feed(cc(2, 99, 1), d);
    CHECK_FALSE(dec.feed(cc(2, 6, 12), d));
    dec.feed(cc(2, 98, 2), d);
    REQUIRE(dec.feed(cc(2, 6, 12), d));
    CHECK(d.source == MidiSource{MidiSourceKind::nrpn, 2, (1 << 7) | 2});
}

TEST_CASE("decode_relative handles the three conventions", "[state][midi-learn][relative]") {
    // Two's complement: 1..63 up, 127..65 down.
    CHECK(decode_relative(RelativeMode::twos_complement, 0) == 0);
    CHECK(decode_relative(RelativeMode::twos_complement, 1) == 1);
    CHECK(decode_relative(RelativeMode::twos_complement, 63) == 63);
    CHECK(decode_relative(RelativeMode::twos_complement, 127) == -1);
    CHECK(decode_relative(RelativeMode::twos_complement, 65) == -63);
    CHECK(decode_relative(RelativeMode::twos_complement, 64) == -64);

    // Signed bit: bit 6 is the sign.
    CHECK(decode_relative(RelativeMode::signed_bit, 1) == 1);
    CHECK(decode_relative(RelativeMode::signed_bit, 65) == -1);
    CHECK(decode_relative(RelativeMode::signed_bit, 127) == -63);
    CHECK(decode_relative(RelativeMode::signed_bit, 64) == 0);

    // Binary offset: 64 is zero.
    CHECK(decode_relative(RelativeMode::binary_offset, 64) == 0);
    CHECK(decode_relative(RelativeMode::binary_offset, 65) == 1);
    CHECK(decode_relative(RelativeMode::binary_offset, 63) == -1);
    CHECK(decode_relative(RelativeMode::binary_offset, 0) == -64);
    CHECK(decode_relative(RelativeMode::binary_offset, 127) == 63);

    CHECK(decode_relative(RelativeMode::absolute, 99) == 0);
    CHECK(decode_relative(RelativeMode::twos_complement, 0xFF) == -1); // high bit masked
}

TEST_CASE("apply_relative moves, clamps and accelerates", "[state][midi-learn][relative]") {
    const RelativeConfig cfg{0.01f, 0.0f};
    CHECK_THAT(apply_relative(0.5f, 3, cfg), WithinAbs(0.53f, 1e-6f));
    CHECK_THAT(apply_relative(0.5f, -3, cfg), WithinAbs(0.47f, 1e-6f));
    CHECK_THAT(apply_relative(0.99f, 50, cfg), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(apply_relative(0.01f, -50, cfg), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(apply_relative(0.5f, 0, cfg), WithinAbs(0.5f, 1e-6f));

    // Acceleration amplifies a burst of n ticks by 1 + a * (n - 1).
    const RelativeConfig accel{0.01f, 0.5f};
    CHECK_THAT(apply_relative(0.0f, 1, accel), WithinAbs(0.01f, 1e-6f));
    CHECK_THAT(apply_relative(0.0f, 5, accel), WithinAbs(5 * 0.01f * 3.0f, 1e-6f));
    CHECK_THAT(apply_relative(1.0f, -5, accel), WithinAbs(1.0f - 5 * 0.01f * 3.0f, 1e-6f));
}

TEST_CASE("MidiSource matching and labels", "[state][midi-learn][source]") {
    const MidiSource any_ch{MidiSourceKind::cc7, MidiSource::kOmni, 74};
    CHECK(any_ch.matches({MidiSourceKind::cc7, 9, 74}));
    CHECK_FALSE(any_ch.matches({MidiSourceKind::cc7, 9, 75}));
    CHECK_FALSE(any_ch.matches({MidiSourceKind::cc14, 9, 74}));

    const MidiSource ch3{MidiSourceKind::cc7, 3, 74};
    CHECK(ch3.matches({MidiSourceKind::cc7, 3, 74}));
    CHECK_FALSE(ch3.matches({MidiSourceKind::cc7, 4, 74}));

    CHECK(label(ch3) == "CC 74 ch 4");
    CHECK(label(any_ch) == "CC 74 (any ch)");
    CHECK(label({MidiSourceKind::nrpn, 0, 1155}) == "NRPN 1155 ch 1");
    CHECK(label({MidiSourceKind::cc14, 1, 7}) == "CC 7/39 (14-bit) ch 2");
    CHECK(MidiSourceHash{}(ch3) == MidiSourceHash{}(MidiSource{MidiSourceKind::cc7, 3, 74}));
}

TEST_CASE("learn: absolute controllers", "[state][midi-learn][classify]") {
    // A pot sweep passes through the middle of the range.
    auto r = classify_learn_sequence(ccs(1, 74, {10, 30, 55, 80, 100}));
    REQUIRE(r.valid);
    CHECK(r.source == MidiSource{MidiSourceKind::cc7, 1, 74});
    CHECK(r.mode == RelativeMode::absolute);

    // One event is learnable but unclassifiable.
    r = classify_learn_sequence(ccs(0, 7, {100}));
    REQUIRE(r.valid);
    CHECK(r.mode == RelativeMode::absolute);
    CHECK(r.confidence < 0.6f);

    // A slow pot start ramps through DISTINCT small values: absolute, not relative.
    r = classify_learn_sequence(ccs(0, 7, {1, 2, 3, 4, 5, 6}));
    CHECK(r.mode == RelativeMode::absolute);
}

TEST_CASE("learn: relative encoders are detected per convention", "[state][midi-learn][classify]") {
    auto r = classify_learn_sequence(ccs(0, 16, {1, 1, 127, 1, 127, 127}));
    REQUIRE(r.valid);
    CHECK(r.mode == RelativeMode::twos_complement);
    CHECK(r.confidence >= 0.9f);

    r = classify_learn_sequence(ccs(0, 16, {1, 2, 65, 65, 1}));
    CHECK(r.mode == RelativeMode::signed_bit);
    CHECK(r.confidence >= 0.9f);

    r = classify_learn_sequence(ccs(0, 16, {65, 65, 63, 66, 62}));
    CHECK(r.mode == RelativeMode::binary_offset);
    CHECK(r.confidence >= 0.9f);

    // One direction, repeated ticks: detected, with honest lower confidence.
    r = classify_learn_sequence(ccs(0, 16, {1, 1, 1, 1}));
    CHECK(r.mode == RelativeMode::twos_complement);
    CHECK(r.confidence < 0.9f);
    r = classify_learn_sequence(ccs(0, 16, {127, 127, 127}));
    CHECK(r.mode == RelativeMode::twos_complement);
    r = classify_learn_sequence(ccs(0, 16, {63, 63, 63, 63}));
    CHECK(r.mode == RelativeMode::binary_offset);

    // Two events are too few to call relative.
    r = classify_learn_sequence(ccs(0, 16, {1, 127}));
    CHECK(r.mode == RelativeMode::absolute);
}

TEST_CASE("learn: only the first controller in the burst is considered", "[state][midi-learn][classify]") {
    std::vector<RawMidi> burst{cc(0, 21, 1), cc(0, 21, 1), cc(0, 99, 5), cc(0, 21, 127),
                               cc(0, 22, 64), cc(0, 21, 1)};
    // The 99 (NRPN select) alone completes nothing; notes and clock are skipped.
    burst.insert(burst.begin(), {RawMidi{0x90, 60, 100}, RawMidi{0xF8, 0, 0}});
    auto r = classify_learn_sequence(burst);
    REQUIRE(r.valid);
    CHECK(r.source == MidiSource{MidiSourceKind::cc7, 0, 21});
    CHECK(r.mode == RelativeMode::twos_complement);
}

TEST_CASE("learn: 14-bit pairs, NRPN, RPN and the other kinds", "[state][midi-learn][classify]") {
    auto r = classify_learn_sequence(std::vector<RawMidi>{cc(0, 1, 30), cc(0, 33, 12), cc(0, 1, 31)});
    REQUIRE(r.valid);
    CHECK(r.source == MidiSource{MidiSourceKind::cc14, 0, 1});

    // LSB first still resolves.
    r = classify_learn_sequence(std::vector<RawMidi>{cc(2, 34, 12), cc(2, 2, 30)});
    CHECK(r.source == MidiSource{MidiSourceKind::cc14, 2, 2});

    // MSB alone is a 7-bit CC.
    r = classify_learn_sequence(std::vector<RawMidi>{cc(0, 1, 30), cc(0, 1, 31), cc(0, 1, 90)});
    CHECK(r.source == MidiSource{MidiSourceKind::cc7, 0, 1});

    r = classify_learn_sequence(std::vector<RawMidi>{cc(1, 99, 1), cc(1, 98, 2), cc(1, 6, 50), cc(1, 38, 3)});
    REQUIRE(r.valid);
    CHECK(r.source == MidiSource{MidiSourceKind::nrpn, 1, (1 << 7) | 2});

    r = classify_learn_sequence(std::vector<RawMidi>{cc(1, 101, 0), cc(1, 100, 2), cc(1, 96, 0)});
    CHECK(r.source == MidiSource{MidiSourceKind::rpn, 1, 2});

    // A selection with no data is not learnable.
    r = classify_learn_sequence(std::vector<RawMidi>{cc(1, 99, 1), cc(1, 98, 2)});
    CHECK_FALSE(r.valid);

    r = classify_learn_sequence(std::vector<RawMidi>{RawMidi{0xE2, 0, 0x40}});
    CHECK(r.source == MidiSource{MidiSourceKind::pitch_bend, 2, 0});
    r = classify_learn_sequence(std::vector<RawMidi>{RawMidi{0xD0, 90, 0}});
    CHECK(r.source.kind == MidiSourceKind::channel_pressure);
    r = classify_learn_sequence(std::vector<RawMidi>{RawMidi{0xA4, 61, 90}});
    CHECK(r.source == MidiSource{MidiSourceKind::poly_pressure, 4, 61});

    CHECK_FALSE(classify_learn_sequence({}).valid);
    CHECK_FALSE(classify_learn_sequence(std::vector<RawMidi>{RawMidi{0x90, 60, 100}}).valid);
}

TEST_CASE("takeover jump applies immediately", "[state][midi-learn][takeover]") {
    TakeoverState s;
    auto r = takeover_step(TakeoverMode::jump, s, 0.9f, 0.1f);
    CHECK(r.apply);
    CHECK_THAT(r.value, WithinAbs(0.9f, 1e-6f));
}

TEST_CASE("takeover pickup waits for the controller to reach the parameter", "[state][midi-learn][takeover]") {
    TakeoverState s;
    // Parameter at 0.5, controller starts far below and sweeps up.
    CHECK_FALSE(takeover_step(TakeoverMode::pickup, s, 0.1f, 0.5f).apply); // first sight
    CHECK_FALSE(takeover_step(TakeoverMode::pickup, s, 0.3f, 0.5f).apply);
    auto r = takeover_step(TakeoverMode::pickup, s, 0.6f, 0.5f); // crossed
    CHECK(r.apply);
    CHECK_THAT(r.value, WithinAbs(0.6f, 1e-6f));
    // Stays engaged, even if the parameter value is now far away.
    CHECK(takeover_step(TakeoverMode::pickup, s, 0.9f, 0.6f).apply);

    // From above, crossing downward.
    takeover_reset(s);
    CHECK_FALSE(takeover_step(TakeoverMode::pickup, s, 0.9f, 0.5f).apply);
    CHECK_FALSE(takeover_step(TakeoverMode::pickup, s, 0.7f, 0.5f).apply);
    CHECK(takeover_step(TakeoverMode::pickup, s, 0.45f, 0.5f).apply);

    // Landing within epsilon engages without a crossing.
    takeover_reset(s);
    CHECK(takeover_step(TakeoverMode::pickup, s, 0.51f, 0.5f).apply);

    // Moving away from the parameter never engages.
    takeover_reset(s);
    CHECK_FALSE(takeover_step(TakeoverMode::pickup, s, 0.3f, 0.5f).apply);
    CHECK_FALSE(takeover_step(TakeoverMode::pickup, s, 0.1f, 0.5f).apply);
    CHECK_FALSE(takeover_step(TakeoverMode::pickup, s, 0.0f, 0.5f).apply);
}

TEST_CASE("takeover scale converges at the end of travel", "[state][midi-learn][takeover]") {
    TakeoverState s;
    float param = 0.8f;
    // Controller at 0.2, sweeping up to 1.0 in steps; the parameter is at 0.8.
    CHECK_FALSE(takeover_step(TakeoverMode::scale, s, 0.2f, param).apply);
    float in = 0.2f;
    bool engaged = false;
    for (int i = 0; i < 40 && !engaged; ++i) {
        in = std::min(1.0f, in + 0.05f);
        auto r = takeover_step(TakeoverMode::scale, s, in, param);
        REQUIRE(r.apply);
        // The parameter never runs backwards while the controller moves up.
        CHECK(r.value >= param - 1e-6f);
        param = r.value;
        engaged = s.engaged;
    }
    CHECK(engaged);
    CHECK_THAT(param, WithinAbs(in, 0.02f));

    // Once engaged, the controller writes directly.
    auto r = takeover_step(TakeoverMode::scale, s, 0.4f, param);
    CHECK(r.apply);
    CHECK_THAT(r.value, WithinAbs(0.4f, 1e-6f));
}

TEST_CASE("takeover scale handles downward travel and a parameter at the end stop", "[state][midi-learn][takeover]") {
    TakeoverState s;
    float param = 0.2f;
    takeover_step(TakeoverMode::scale, s, 0.9f, param);
    float in = 0.9f;
    for (int i = 0; i < 40 && !s.engaged; ++i) {
        in = std::max(0.0f, in - 0.05f);
        auto r = takeover_step(TakeoverMode::scale, s, in, param);
        REQUIRE(r.apply);
        CHECK(r.value <= param + 1e-6f);
        param = r.value;
    }
    CHECK(s.engaged);

    // Parameter pinned at 1.0 while the controller moves up: no change, no NaN.
    TakeoverState t;
    takeover_step(TakeoverMode::scale, t, 0.1f, 1.0f);
    auto r = takeover_step(TakeoverMode::scale, t, 0.3f, 1.0f);
    CHECK(r.apply);
    CHECK_THAT(r.value, WithinAbs(1.0f, 1e-6f));
}
