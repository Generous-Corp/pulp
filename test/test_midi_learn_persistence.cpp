// MIDI mapping persistence and undo: the binary state chunk (round trip, pinned
// wire layout, older/newer/corrupt data), the JSON preset form, engine load
// and import modes, and undo/redo of mapping edits.

#include <pulp/state/midi_learn_engine.hpp>
#include <pulp/state/midi_learn_persistence.hpp>
#include <pulp/state/midi_learn_undo.hpp>
#include <pulp/state/store.hpp>
#include <pulp/state/undo_manager.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstring>

using namespace pulp::state;
using Catch::Matchers::WithinAbs;
using Status = MidiMapParseResult::Status;

namespace {

MidiMapping mk(MidiSourceKind kind, uint8_t ch, uint16_t number, ParamID id) {
    MidiMapping m;
    m.source = {kind, ch, number};
    m.id = id;
    return m;
}

std::vector<MidiMapping> sample() {
    std::vector<MidiMapping> v;
    auto a = mk(MidiSourceKind::cc7, 3, 74, 1);
    a.scale = {0.25f, 0.75f};
    a.takeover = TakeoverMode::pickup;
    v.push_back(a);
    auto b = mk(MidiSourceKind::cc7, MidiSource::kOmni, 16, 2);
    b.mode = RelativeMode::signed_bit;
    b.relative = {0.02f, 0.5f};
    v.push_back(b);
    v.push_back(mk(MidiSourceKind::cc14, 0, 7, 3));
    auto d = mk(MidiSourceKind::nrpn, 1, 1155, 4);
    d.feedback = true;
    v.push_back(d);
    v.push_back(mk(MidiSourceKind::rpn, 2, 2, 5));
    v.push_back(mk(MidiSourceKind::pitch_bend, 0, 0, 6));
    v.push_back(mk(MidiSourceKind::channel_pressure, 4, 0, 7));
    v.push_back(mk(MidiSourceKind::poly_pressure, 0, 61, 8));
    auto e = mk(MidiSourceKind::program_change, 0, 12, 9);
    e.scale = {1.0f, 0.0f};
    v.push_back(e);
    return v;
}

void check_same(const std::vector<MidiMapping>& got, const std::vector<MidiMapping>& want) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        auto w = want[i];
        auto g = got[i];
        w.uid = g.uid = 0;
        CHECK(g == w);
    }
}

uint32_t fnv(const uint8_t* p, std::size_t n) {
    uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void populate(StateStore& s) {
    for (ParamID id = 1; id <= 9; ++id)
        s.add_parameter({.id = id, .name = "P", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
}

} // namespace

TEST_CASE("chunk: every source kind round-trips", "[state][midi-learn][persistence]") {
    const auto in = sample();
    const auto bytes = serialize_midi_mappings(in);
    const auto out = parse_midi_mappings(bytes);
    CHECK(out.status == Status::ok);
    CHECK(out.version == kMidiMapChunkVersion);
    CHECK(out.dropped == 0);
    check_same(out.mappings, in);
}

TEST_CASE("chunk: the wire layout is pinned", "[state][midi-learn][persistence]") {
    MidiMapping m = mk(MidiSourceKind::cc7, 0, 74, 1);
    m.relative = {1.0f / 128.0f, 0.0f};
    const auto bytes = serialize_midi_mappings(std::vector{m});
    REQUIRE(bytes.size() == 16 + 28);
    CHECK(std::memcmp(bytes.data(), "PMLN", 4) == 0);
    CHECK(bytes[4] == 1); // version, little-endian
    CHECK(bytes[5] == 0);
    CHECK(bytes[6] == 28); // record size
    CHECK(bytes[8] == 1);  // count
    const uint8_t record[28] = {0x00, 0x00, 0x4A, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00};
    CHECK(std::memcmp(bytes.data() + 16, record, 28) == 0);
    const uint32_t sum = fnv(record, 28);
    CHECK(bytes[12] == (sum & 0xFF));
    CHECK(bytes[15] == (sum >> 24));
}

TEST_CASE("chunk: state saved before mappings existed is an empty ok",
          "[state][midi-learn][persistence]") {
    const auto out = parse_midi_mappings({});
    CHECK(out.status == Status::ok);
    CHECK(out.mappings.empty());
    const auto zero = parse_midi_mappings(serialize_midi_mappings({}));
    CHECK(zero.status == Status::ok);
    CHECK(zero.mappings.empty());
}

TEST_CASE("chunk: a newer version loads its known fields", "[state][midi-learn][persistence]") {
    const auto in = sample();
    auto bytes = serialize_midi_mappings(in);
    // Rewrite as version 2 with 4 extra bytes per record, which an older
    // reader must skip.
    const std::size_t n = in.size();
    std::vector<uint8_t> v2(bytes.begin(), bytes.begin() + 16);
    v2[4] = 2;
    v2[6] = 32;
    for (std::size_t i = 0; i < n; ++i) {
        v2.insert(v2.end(), bytes.begin() + 16 + i * 28, bytes.begin() + 16 + (i + 1) * 28);
        for (int k = 0; k < 4; ++k)
            v2.push_back(0xEE);
    }
    const auto out = parse_midi_mappings(v2);
    CHECK(out.status == Status::newer_version);
    CHECK(out.version == 2);
    check_same(out.mappings, in);

    // A newer record smaller than the fields we need is unusable.
    auto small = v2;
    small[6] = 20;
    CHECK(parse_midi_mappings(small).status == Status::corrupt);
}

TEST_CASE("chunk: damaged data is rejected without partial results",
          "[state][midi-learn][persistence]") {
    const auto good = serialize_midi_mappings(sample());

    auto bad_magic = good;
    bad_magic[0] = 'X';
    CHECK(parse_midi_mappings(bad_magic).status == Status::corrupt);

    CHECK(parse_midi_mappings(std::span<const uint8_t>(good.data(), 10)).status == Status::corrupt);
    CHECK(parse_midi_mappings(std::span<const uint8_t>(good.data(), good.size() - 1)).status ==
          Status::corrupt);

    auto flipped = good;
    flipped[16 + 30] ^= 0x01;
    const auto r = parse_midi_mappings(flipped);
    CHECK(r.status == Status::corrupt);
    CHECK(r.mappings.empty());

    // A count that claims more records than the buffer holds must not allocate or read past it.
    auto huge = good;
    huge[8] = huge[9] = huge[10] = huge[11] = 0xFF;
    CHECK(parse_midi_mappings(huge).status == Status::corrupt);

    auto zero_version = good;
    zero_version[4] = zero_version[5] = 0;
    CHECK(parse_midi_mappings(zero_version).status == Status::corrupt);
}

TEST_CASE("chunk: invalid records are dropped and counted", "[state][midi-learn][persistence]") {
    auto in = sample();
    auto bytes = serialize_midi_mappings(in);
    // Record 0: source kind out of range. Record 1: channel 20. Record 2: cc14 number 40.
    bytes[16 + 0 * 28 + 0] = 99;
    bytes[16 + 1 * 28 + 1] = 20;
    bytes[16 + 2 * 28 + 2] = 40;
    // Fix the checksum so only the field validation is exercised.
    const uint32_t sum = fnv(bytes.data() + 16, bytes.size() - 16);
    for (int i = 0; i < 4; ++i)
        bytes[12 + i] = static_cast<uint8_t>(sum >> (8 * i));
    const auto out = parse_midi_mappings(bytes);
    CHECK(out.status == Status::ok);
    CHECK(out.dropped == 3);
    CHECK(out.mappings.size() == in.size() - 3);
}

TEST_CASE("chunk: capacity is enforced on both sides", "[state][midi-learn][persistence]") {
    std::vector<MidiMapping> many;
    for (uint32_t i = 0; i < kMaxMidiMappings + 10; ++i)
        many.push_back(mk(MidiSourceKind::poly_pressure, static_cast<uint8_t>(i % 16),
                          static_cast<uint16_t>(i % 128), 1));
    const auto out = parse_midi_mappings(serialize_midi_mappings(many));
    CHECK(out.mappings.size() == kMaxMidiMappings);
    CHECK(out.status == Status::ok);
}

TEST_CASE("json: round trip and tolerant parsing", "[state][midi-learn][persistence][json]") {
    const auto in = sample();
    const auto text = midi_mappings_to_json(in);
    const auto out = midi_mappings_from_json(text);
    CHECK(out.status == Status::ok);
    CHECK(out.dropped == 0);
    check_same(out.mappings, in);

    // Unknown kind and out-of-range fields drop only that entry.
    const auto mixed = midi_mappings_from_json(R"({"version":1,"mappings":[
        {"source":{"kind":"cc7","channel":"omni","number":10},"param":1},
        {"source":{"kind":"theremin","channel":0,"number":1},"param":2},
        {"source":{"kind":"cc7","channel":16,"number":1},"param":3},
        {"source":{"kind":"cc7","channel":0,"number":200},"param":4},
        {"source":{"kind":"cc7","channel":0,"number":11},"param":5,"mode":"sideways"},
        7,
        {"source":{"kind":"pitch_bend","channel":2,"number":0},"param":6,"min":1.0,"max":0.0}]})");
    CHECK(mixed.status == Status::ok);
    CHECK(mixed.dropped == 5);
    REQUIRE(mixed.mappings.size() == 2);
    CHECK(mixed.mappings[0].source.channel == MidiSource::kOmni);
    CHECK_THAT(mixed.mappings[1].scale.min, WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(mixed.mappings[1].scale.max, WithinAbs(0.0f, 1e-6f));

    CHECK(midi_mappings_from_json("not json").status == Status::corrupt);
    CHECK(midi_mappings_from_json("[]").status == Status::corrupt);
    CHECK(midi_mappings_from_json(R"({"mappings":[]})").status == Status::corrupt);
    CHECK(midi_mappings_from_json(R"({"version":1})").status == Status::corrupt);
    CHECK(midi_mappings_from_json(R"({"version":7,"mappings":[]})").status ==
          Status::newer_version);
}

TEST_CASE("engine: save and load a state chunk", "[state][midi-learn][persistence][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine a;
    for (const auto& m : sample())
        REQUIRE(a.set_mapping(m));
    const auto bytes = a.save_state();

    MidiLearnEngine b;
    const auto report = b.load_state(bytes, &s);
    CHECK(report.status == Status::ok);
    CHECK(report.loaded == sample().size());
    CHECK(report.changed);
    check_same(b.mappings(), sample());

    // The loaded engine routes like the original.
    b.begin_block();
    // The first mapping uses pickup: a value near the parameter engages it.
    b.route(s, RawMidi{0xB3, 74, 64});
    b.route(s, RawMidi{0xB3, 74, 127});
    CHECK_THAT(s.get_normalized(1), WithinAbs(0.75f, 1e-5f));
}

TEST_CASE("engine: load drops unknown parameters and keeps the table on corrupt data",
          "[state][midi-learn][persistence][engine]") {
    StateStore s;
    populate(s);
    s.add_parameter({.id = 77, .name = "X", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});

    MidiLearnEngine a;
    a.set_mapping(mk(MidiSourceKind::cc7, 0, 1, 1));
    a.set_mapping(mk(MidiSourceKind::cc7, 0, 2, 4242)); // not in the store used for loading
    const auto bytes = a.save_state();

    MidiLearnEngine b;
    b.set_mapping(mk(MidiSourceKind::cc7, 0, 50, 2));
    const auto report = b.load_state(bytes, &s);
    CHECK(report.loaded == 1);
    CHECK(report.dropped == 1);
    REQUIRE(b.mappings().size() == 1);
    CHECK(b.mappings()[0].source.number == 1);

    auto damaged = bytes;
    damaged[20] ^= 0xFF;
    const auto bad = b.load_state(damaged, &s);
    CHECK(bad.status == Status::corrupt);
    CHECK_FALSE(bad.changed);
    CHECK(b.mappings().size() == 1);
}

TEST_CASE("engine: JSON import in replace and merge modes",
          "[state][midi-learn][persistence][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine a;
    a.set_mapping(mk(MidiSourceKind::cc7, 0, 1, 1));
    a.set_mapping(mk(MidiSourceKind::cc7, 0, 2, 2));
    const auto json = a.save_json();

    MidiLearnEngine b;
    b.set_mapping(mk(MidiSourceKind::cc7, 0, 1, 3));  // same source as an imported one
    b.set_mapping(mk(MidiSourceKind::cc7, 0, 9, 2));  // same parameter as an imported one
    b.set_mapping(mk(MidiSourceKind::cc7, 0, 30, 4)); // unrelated, survives a merge

    const auto merged = b.load_json(json, &s, MidiImportMode::merge);
    CHECK(merged.loaded == 3);
    auto mp = b.mappings();
    CHECK(mp.size() == 3);
    CHECK(b.mapping_for(4).has_value());
    CHECK_FALSE(b.mapping_for(3).has_value());
    CHECK(b.mapping_for(2)->source.number == 2);

    const auto replaced = b.load_json(json, &s, MidiImportMode::replace);
    CHECK(replaced.loaded == 2);
    CHECK(b.mappings().size() == 2);
    CHECK_FALSE(b.mapping_for(4).has_value());
}

TEST_CASE("engine: replace_all keeps uids so takeover state survives",
          "[state][midi-learn][persistence][engine]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    auto m = mk(MidiSourceKind::cc7, 0, 74, 1);
    m.takeover = TakeoverMode::pickup;
    e.set_mapping(m);
    e.begin_block();
    e.route(s, RawMidi{0xB0, 74, 10});
    e.route(s, RawMidi{0xB0, 74, 90}); // crosses 0.5: engaged
    REQUIRE_THAT(s.get_normalized(1), WithinAbs(90 / 127.0f, 1e-5f));

    REQUIRE(e.replace_all(e.mappings()));
    e.begin_block();
    e.route(s, RawMidi{0xB0, 74, 20});
    CHECK_THAT(s.get_normalized(1), WithinAbs(20 / 127.0f, 1e-5f)); // still engaged

    std::vector<MidiMapping> too_many(kMaxMidiMappings + 1);
    for (std::size_t i = 0; i < too_many.size(); ++i)
        too_many[i].source = {MidiSourceKind::poly_pressure, static_cast<uint8_t>(i % 16),
                              static_cast<uint16_t>(i / 16 % 128)};
    CHECK_FALSE(e.replace_all(too_many));
    CHECK(e.mappings().size() == 1);
}

TEST_CASE("undo: set, clear and clear-all are one step each", "[state][midi-learn][undo]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    UndoManager undo;
    MidiLearnEditor ed(e, undo);

    REQUIRE(ed.set_mapping(mk(MidiSourceKind::cc7, 0, 1, 1)));
    REQUIRE(ed.set_mapping(mk(MidiSourceKind::cc7, 0, 2, 2)));
    CHECK(e.mappings().size() == 2);

    ed.clear(1);
    CHECK(e.mappings().size() == 1);
    undo.undo();
    CHECK(e.mappings().size() == 2);
    undo.redo();
    CHECK(e.mappings().size() == 1);

    ed.clear_all();
    CHECK(e.mappings().empty());
    undo.undo();
    CHECK(e.mappings().size() == 1);
    undo.undo(); // undo the clear of param 1
    CHECK(e.mappings().size() == 2);
    undo.undo(); // undo the second set
    CHECK(e.mappings().size() == 1);

    // An edit that changes nothing records nothing.
    const bool had_undo = undo.can_undo();
    ed.clear(999);
    CHECK(undo.can_undo() == had_undo);

    // The restored table is live on the audio side.
    e.begin_block();
    CHECK(e.route(s, RawMidi{0xB0, 1, 127}));
    CHECK_FALSE(e.route(s, RawMidi{0xB0, 2, 127}));
}

TEST_CASE("undo: a learn is undone with what it replaced", "[state][midi-learn][undo]") {
    StateStore s;
    populate(s);
    MidiLearnEngine e;
    UndoManager undo;
    MidiLearnEditor ed(e, undo);
    REQUIRE(ed.set_mapping(mk(MidiSourceKind::cc7, 0, 1, 1)));  // same parameter as the learn
    REQUIRE(ed.set_mapping(mk(MidiSourceKind::cc7, 0, 20, 2))); // same source as the learn

    e.begin_learn(1, 0);
    e.begin_block();
    e.route(s, RawMidi{0xB0, 20, 30});
    e.route(s, RawMidi{0xB0, 20, 60});
    e.poll_learn(10);
    auto out = e.poll_learn(1000);
    REQUIRE(out.has_value());
    REQUIRE(out->bound);
    CHECK(out->replaced.size() == 2);
    ed.record_learn(*out);
    REQUIRE(e.mappings().size() == 1);
    CHECK(e.mapping_for(1)->source.number == 20);

    undo.undo();
    auto restored = e.mappings();
    REQUIRE(restored.size() == 2);
    CHECK(e.mapping_for(1)->source.number == 1);
    CHECK(e.mapping_for(2)->source.number == 20);

    undo.redo();
    CHECK(e.mappings().size() == 1);
    CHECK(e.mapping_for(1)->source.number == 20);

    // A failed learn records nothing.
    MidiLearnOutcome none;
    const bool before = undo.can_undo();
    ed.record_learn(none);
    CHECK(undo.can_undo() == before);
}
