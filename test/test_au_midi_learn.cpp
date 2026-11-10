// MIDI Learn feedback through the AU v2 output path: the messages the learn
// engine writes to the plug-in's MIDI output are delivered as CoreMIDI packets,
// with the right bytes for each message kind, and nothing is built when the
// parameter has not moved.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/midi_learn_support.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/midi_learn_engine.hpp>
#include <pulp/state/store.hpp>

#include <cstdint>
#include <vector>

using namespace pulp;

namespace {

constexpr state::ParamID kCut = 1;
constexpr state::ParamID kBend = 2;
constexpr state::ParamID kPress = 3;

state::MidiMapping feedback_mapping(state::MidiSourceKind kind, uint8_t ch, uint16_t number,
                                    state::ParamID id) {
    state::MidiMapping m;
    m.source = {kind, ch, number};
    m.id = id;
    m.feedback = true;
    return m;
}

struct Rig {
    state::StateStore store;
    state::MidiLearnEngine engine;
    midi::MidiBuffer in, out;
    format::au::MidiOutputPacketBuilder builder;

    Rig() {
        for (auto id : {kCut, kBend, kPress})
            store.add_parameter(
                {.id = id, .name = "P", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    }

    // One block through the shared helper, then the AU delivery buffer.
    const MIDIPacketList* block(bool produces_midi = true) {
        out.clear();
        format::midi_learn_process_block(
            engine, store, in, out,
            format::midi_feedback_capabilities(format::MidiLearnFormat::au_v2, produces_midi));
        return builder.build(out, 256);
    }
};

// The delivered MIDI messages, in order. Packetization is the packet builder's
// business (messages sharing a timestamp may share a packet), so the bytes of
// every packet are flattened and split at status bytes.
std::vector<std::vector<uint8_t>> packets(const MIDIPacketList* list) {
    std::vector<uint8_t> bytes;
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        bytes.insert(bytes.end(), p->data, p->data + p->length);
        p = MIDIPacketNext(p);
    }
    std::vector<std::vector<uint8_t>> out;
    for (uint8_t b : bytes) {
        if (b & 0x80)
            out.push_back({b});
        else if (!out.empty())
            out.back().push_back(b);
    }
    return out;
}

} // namespace

TEST_CASE("au midi learn: feedback is delivered as CoreMIDI packets", "[au][au-v2][midi-learn]") {
    Rig rig;
    REQUIRE(rig.engine.set_mapping(feedback_mapping(state::MidiSourceKind::cc7, 3, 74, kCut)));
    REQUIRE(
        rig.engine.set_mapping(feedback_mapping(state::MidiSourceKind::pitch_bend, 1, 0, kBend)));
    REQUIRE(rig.engine.set_mapping(
        feedback_mapping(state::MidiSourceKind::channel_pressure, 2, 0, kPress)));

    // First block: each controller is synced to its parameter's value (0.5).
    const MIDIPacketList* list = rig.block();
    REQUIRE(list != nullptr);
    auto got = packets(list);
    REQUIRE(got.size() == 3);
    CHECK(got[0] == std::vector<uint8_t>{0xB3, 74, 64}); // control change, channel 4
    CHECK(got[1] == std::vector<uint8_t>{0xE1, 0, 64});  // pitch bend centre, channel 2
    CHECK(got[2] == std::vector<uint8_t>{0xD2, 64});     // channel pressure, two bytes

    // Nothing moved: nothing to deliver.
    CHECK(rig.block() == nullptr);

    // One parameter moves: one packet follows.
    rig.store.set_normalized_rt(kCut, 1.0f);
    list = rig.block();
    REQUIRE(list != nullptr);
    got = packets(list);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == std::vector<uint8_t>{0xB3, 74, 127});
    CHECK(rig.builder.dropped == 0);
}

TEST_CASE("au midi learn: a plug-in without MIDI output builds nothing",
          "[au][au-v2][midi-learn]") {
    Rig rig;
    REQUIRE(rig.engine.set_mapping(feedback_mapping(state::MidiSourceKind::cc7, 0, 74, kCut)));
    rig.store.set_normalized_rt(kCut, 1.0f);
    CHECK(rig.block(/*produces_midi=*/false) == nullptr);
}
