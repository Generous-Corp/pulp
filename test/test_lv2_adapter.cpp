#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/format/lv2_adapter.hpp>
#include <pulp/format/lv2_entry.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <cstring>

using namespace pulp;
using namespace pulp::format;
using namespace pulp::format::lv2_adapter;
using Catch::Matchers::ContainsSubstring;

// Helper: build a test descriptor and store without needing a Processor subclass
static PluginDescriptor make_effect_desc() {
    PluginDescriptor desc;
    desc.name = "TestLv2";
    desc.manufacturer = "Pulp";
    desc.bundle_id = "com.pulp.test-lv2";
    desc.version = "1.0.0";
    desc.category = PluginCategory::Effect;
    desc.input_buses = {{"Audio In", 2}};
    desc.output_buses = {{"Audio Out", 2}};
    desc.accepts_midi = false;
    desc.produces_midi = false;
    return desc;
}

static void add_test_params(state::StateStore& store) {
    store.add_parameter({
        .id = 1,
        .name = "Gain",
        .unit = "dB",
        .range = {-60.0f, 24.0f, 0.0f, 0.1f},
    });
    store.add_parameter({
        .id = 2,
        .name = "Mix",
        .unit = "%",
        .range = {0.0f, 100.0f, 100.0f},
    });
}

TEST_CASE("LV2 TTL generation produces valid plugin.ttl", "[format][lv2]") {
    auto desc = make_effect_desc();
    state::StateStore store;
    add_test_params(store);

    auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/test-lv2");

    // Check prefixes
    REQUIRE_THAT(ttl, ContainsSubstring("@prefix lv2:"));
    REQUIRE_THAT(ttl, ContainsSubstring("@prefix doap:"));

    // Check plugin URI
    REQUIRE_THAT(ttl, ContainsSubstring("<http://pulp.audio/plugins/test-lv2>"));

    // Check plugin metadata
    REQUIRE_THAT(ttl, ContainsSubstring("doap:name \"TestLv2\""));
    REQUIRE_THAT(ttl, ContainsSubstring("doap:name \"Pulp\""));

    // Check audio ports (2 in + 2 out = 4 audio ports)
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:AudioPort"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:InputPort"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:OutputPort"));

    // Check control ports for parameters
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:ControlPort"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:name \"Gain\""));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:name \"Mix\""));

    // Check parameter ranges
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:minimum -60"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:maximum 24"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:default 0"));
}

TEST_CASE("LV2 TTL includes MIDI ports for instruments", "[format][lv2]") {
    PluginDescriptor desc;
    desc.name = "TestSynth";
    desc.manufacturer = "Pulp";
    desc.category = PluginCategory::Instrument;
    desc.input_buses = {};
    desc.output_buses = {{"Audio Out", 2}};
    desc.accepts_midi = true;
    desc.produces_midi = false;

    state::StateStore store;
    auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/test-synth");

    // Should have InstrumentPlugin class
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:InstrumentPlugin"));

    // Should have MIDI input atom port
    REQUIRE_THAT(ttl, ContainsSubstring("atom:AtomPort"));
    REQUIRE_THAT(ttl, ContainsSubstring("midi:MidiEvent"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:symbol \"midi_in\""));
}

TEST_CASE("LV2 manifest.ttl generation", "[format][lv2]") {
    auto ttl = generate_manifest_ttl(
        "http://pulp.audio/plugins/test-lv2", "TestLv2.so");

    REQUIRE_THAT(ttl, ContainsSubstring("<http://pulp.audio/plugins/test-lv2>"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:binary <TestLv2.so>"));
    REQUIRE_THAT(ttl, ContainsSubstring("rdfs:seeAlso <TestLv2.ttl>"));
}

TEST_CASE("LV2 TTL port indices are sequential", "[format][lv2]") {
    auto desc = make_effect_desc();
    state::StateStore store;
    add_test_params(store);

    auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/test");

    // 2 audio in + 2 audio out + 2 control = indices 0-5
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 0"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 1"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 2"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 3"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 4"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 5"));
}

// ── URID feature resolution (workstream 01 slice 1.5) ─────────────────────

static LV2_URID fake_map(LV2_URID_Map_Handle handle, const char* uri) {
    // Simple table: return stable IDs per URI, starting at 100.
    auto* table = static_cast<std::vector<std::string>*>(handle);
    for (size_t i = 0; i < table->size(); ++i) {
        if ((*table)[i] == uri) return static_cast<LV2_URID>(100 + i);
    }
    table->push_back(uri);
    return static_cast<LV2_URID>(100 + table->size() - 1);
}

TEST_CASE("find_urid_map locates LV2_URID__map in features", "[format][lv2]") {
    std::vector<std::string> table;
    LV2_URID_Map map{&table, &fake_map};
    LV2_Feature feat_map{LV2_URID__map, &map};
    LV2_Feature feat_other{"http://example.com/irrelevant", nullptr};

    // Feature array must be NULL-terminated.
    const LV2_Feature* features[] = {&feat_other, &feat_map, nullptr};
    REQUIRE(find_urid_map(features) == &map);
}

TEST_CASE("find_urid_map returns nullptr when feature absent", "[format][lv2]") {
    LV2_Feature feat_other{"http://example.com/irrelevant", nullptr};
    const LV2_Feature* features[] = {&feat_other, nullptr};
    REQUIRE(find_urid_map(features) == nullptr);
    REQUIRE(find_urid_map(nullptr) == nullptr);
    // Empty array (only sentinel)
    const LV2_Feature* empty[] = {nullptr};
    REQUIRE(find_urid_map(empty) == nullptr);
}

// ── #491: LV2 MIDI output port serialization ─────────────────────────────
//
// Regression guard for the silent-drop bug: the LV2 adapter declared the
// atom:AtomPort output in TTL for plugins with produces_midi, but run()
// never serialized midi_out back into the host's sequence buffer — so
// every outgoing MIDI event vanished. These tests exercise the extracted
// helper write_midi_out_to_sequence() directly; full run()-level
// integration is tested via format-validator lanes in CI.

namespace {
constexpr LV2_URID kUridAtomSeq = 1;
constexpr LV2_URID kUridMidiEvt = 2;

// Allocate an output Atom_Sequence buffer with host-style capacity encoding:
// atom.size initially holds the usable body capacity (per LV2 spec the
// plugin overwrites it in run()).
struct LV2SequenceBuffer {
    std::array<uint8_t, 512> storage{};
    LV2_Atom_Sequence* as_seq() {
        return reinterpret_cast<LV2_Atom_Sequence*>(storage.data());
    }
    void prepare_as_output_port() {
        std::memset(storage.data(), 0, storage.size());
        // Host sets atom.size to the body capacity on entry to run().
        as_seq()->atom.size = static_cast<uint32_t>(
            storage.size() - sizeof(LV2_Atom));
        as_seq()->atom.type = 0;
    }
};
} // namespace

TEST_CASE("write_midi_out_to_sequence emits MIDI events in order",
          "[format][lv2][issue-491]") {
    LV2SequenceBuffer buf;
    buf.prepare_as_output_port();

    midi::MidiBuffer midi_out;
    auto note_on = midi::MidiEvent::note_on(0, 60, 100);
    note_on.sample_offset = 8;
    midi_out.add(note_on);
    auto note_off = midi::MidiEvent::note_off(0, 60, 0);
    note_off.sample_offset = 48;
    midi_out.add(note_off);

    lv2_generic::write_midi_out_to_sequence(
        buf.as_seq(), kUridAtomSeq, kUridMidiEvt, midi_out);

    // Header: type rewritten to sequence URID, unit = 0 (frames).
    REQUIRE(buf.as_seq()->atom.type == kUridAtomSeq);
    REQUIRE(buf.as_seq()->body.unit == 0);
    REQUIRE(buf.as_seq()->body.pad == 0);

    // Walk the emitted sequence and verify both events landed intact.
    std::vector<std::tuple<int64_t, LV2_URID, uint32_t, uint8_t, uint8_t, uint8_t>> seen;
    LV2_ATOM_SEQUENCE_FOREACH(buf.as_seq(), ev) {
        const auto* data = reinterpret_cast<const uint8_t*>(ev + 1);
        seen.emplace_back(ev->time.frames, ev->body.type, ev->body.size,
                          data[0], data[1], data[2]);
    }
    REQUIRE(seen.size() == 2);
    REQUIRE(std::get<0>(seen[0]) == 8);
    REQUIRE(std::get<1>(seen[0]) == kUridMidiEvt);
    REQUIRE(std::get<2>(seen[0]) == 3);
    REQUIRE(std::get<3>(seen[0]) == 0x90);  // note-on, ch0
    REQUIRE(std::get<4>(seen[0]) == 60);
    REQUIRE(std::get<5>(seen[0]) == 100);
    REQUIRE(std::get<0>(seen[1]) == 48);
    REQUIRE(std::get<3>(seen[1]) == 0x80);  // note-off, ch0
}

TEST_CASE("write_midi_out_to_sequence is a no-op on null/missing URIDs",
          "[format][lv2][issue-491]") {
    LV2SequenceBuffer buf;
    buf.prepare_as_output_port();
    midi::MidiBuffer midi_out;
    midi_out.add(midi::MidiEvent::note_on(0, 60, 100));

    // Null out_seq — function returns without touching anything.
    lv2_generic::write_midi_out_to_sequence(
        nullptr, kUridAtomSeq, kUridMidiEvt, midi_out);
    SUCCEED("no crash on null out_seq");

    // Missing atom-sequence URID — function bails before mutating buf.
    const auto snapshot = buf.storage;
    lv2_generic::write_midi_out_to_sequence(
        buf.as_seq(), 0, kUridMidiEvt, midi_out);
    REQUIRE(buf.storage == snapshot);

    // Missing midi-event URID — same guard.
    lv2_generic::write_midi_out_to_sequence(
        buf.as_seq(), kUridAtomSeq, 0, midi_out);
    REQUIRE(buf.storage == snapshot);
}

TEST_CASE("write_midi_out_to_sequence drops events that overflow capacity",
          "[format][lv2][issue-491]") {
    // Undersized buffer: only room for a couple of events before append fails.
    struct TinyBuf {
        std::array<uint8_t, 64> storage{};
        LV2_Atom_Sequence* as_seq() {
            return reinterpret_cast<LV2_Atom_Sequence*>(storage.data());
        }
    } buf;
    std::memset(buf.storage.data(), 0, buf.storage.size());
    buf.as_seq()->atom.size = static_cast<uint32_t>(
        buf.storage.size() - sizeof(LV2_Atom));

    midi::MidiBuffer midi_out;
    for (int i = 0; i < 20; ++i) {
        auto ev = midi::MidiEvent::note_on(0, static_cast<uint8_t>(60 + i), 100);
        ev.sample_offset = i * 4;
        midi_out.add(ev);
    }

    lv2_generic::write_midi_out_to_sequence(
        buf.as_seq(), kUridAtomSeq, kUridMidiEvt, midi_out);

    // At least one event must have landed; overflow drops the remainder.
    int count = 0;
    LV2_ATOM_SEQUENCE_FOREACH(buf.as_seq(), ev) { (void)ev; ++count; }
    REQUIRE(count >= 1);
    REQUIRE(count < 20);  // drop happened, no crash/corruption
}
