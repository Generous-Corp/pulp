// Format-side MIDI Learn glue: per-format feedback capabilities, the
// per-block helper, and the handle that lets an adapter reach the engine.

#include <pulp/format/midi_learn_support.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace pulp;
using namespace pulp::format;
using Catch::Matchers::WithinAbs;
using state::MidiSourceKind;

namespace {

class NullProcessor : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        return {.name = "T", .manufacturer = "T", .bundle_id = "t", .version = "1"};
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const ProcessContext&) override {}
};

void populate(state::StateStore& s) {
    s.add_parameter({.id = 1, .name = "A", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
}

state::MidiMapping fb_cc(uint8_t cc, state::ParamID id) {
    state::MidiMapping m;
    m.source = {MidiSourceKind::cc7, 0, cc};
    m.id = id;
    m.feedback = true;
    return m;
}

} // namespace

TEST_CASE("capabilities: a format claims feedback only for a plug-in with MIDI output",
          "[format][midi-learn][feedback]") {
    for (auto f : {MidiLearnFormat::vst3, MidiLearnFormat::clap, MidiLearnFormat::au_v2,
                   MidiLearnFormat::au_v3}) {
        const auto yes = midi_feedback_capabilities(f, true);
        CHECK(yes.supported);
        CHECK(yes.can_send(MidiSourceKind::cc7));
        CHECK(yes.can_send(MidiSourceKind::cc14));
        CHECK(yes.can_send(MidiSourceKind::nrpn));
        CHECK(yes.can_send(MidiSourceKind::pitch_bend));
        CHECK(yes.can_send(MidiSourceKind::channel_pressure));
        CHECK(yes.can_send(MidiSourceKind::poly_pressure));
        CHECK_FALSE(yes.can_send(MidiSourceKind::program_change)); // never sent as feedback

        const auto no = midi_feedback_capabilities(f, false);
        CHECK_FALSE(no.supported);
        CHECK_FALSE(no.can_send(MidiSourceKind::cc7));
    }

    // Not claimed: no adapter outbound path has been verified for these.
    for (auto f : {MidiLearnFormat::lv2, MidiLearnFormat::aax, MidiLearnFormat::standalone}) {
        CHECK_FALSE(midi_feedback_capabilities(f, true).supported);
    }

    NullProcessor p;
    auto d = p.descriptor();
    CHECK_FALSE(midi_feedback_capabilities(MidiLearnFormat::vst3, d).supported);
    d.produces_midi = true;
    CHECK(midi_feedback_capabilities(MidiLearnFormat::vst3, d).supported);
}

TEST_CASE("process block: routes input and sends feedback into the output buffer",
          "[format][midi-learn][feedback]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    REQUIRE(engine.set_mapping(fb_cc(74, 1)));
    const auto caps = midi_feedback_capabilities(MidiLearnFormat::vst3, true);

    midi::MidiBuffer in, out;
    midi_learn_process_block(engine, store, in, out, caps); // adopt + initial sync
    REQUIRE(out.size() == 1);
    CHECK(out.begin()->data()[0] == 0xB0);
    CHECK(out.begin()->data()[1] == 74);
    CHECK(out.begin()->data()[2] == 64);

    // Automation moves the parameter: feedback follows.
    out.clear();
    store.set_normalized_rt(1, 1.0f);
    midi_learn_process_block(engine, store, in, out, caps);
    REQUIRE(out.size() == 1);
    CHECK(out.begin()->data()[2] == 127);

    // A controller message drives the parameter and is not echoed.
    out.clear();
    in.add(midi::MidiEvent::cc(0, 74, 32));
    midi_learn_process_block(engine, store, in, out, caps);
    CHECK_THAT(store.get_normalized(1), WithinAbs(32 / 127.0f, 1e-5f));
    CHECK(out.size() == 0);
}

TEST_CASE("process block: without feedback support nothing is written",
          "[format][midi-learn][feedback]") {
    state::StateStore store;
    populate(store);
    state::MidiLearnEngine engine;
    REQUIRE(engine.set_mapping(fb_cc(74, 1)));

    midi::MidiBuffer in, out;
    in.add(midi::MidiEvent::cc(0, 74, 127));
    midi_learn_process_block(engine, store, in, out,
                             midi_feedback_capabilities(MidiLearnFormat::lv2, true));
    CHECK(out.size() == 0);
    CHECK_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-5f)); // input still routes
}

TEST_CASE("handle: registers the engine with its processor and unregisters",
          "[format][midi-learn]") {
    NullProcessor p;
    CHECK(p.midi_learn_engine() == nullptr);
    {
        state::MidiLearnEngine engine;
        MidiLearnHandle handle(p, engine);
        CHECK(p.midi_learn_engine() == &engine);

        NullProcessor other;
        CHECK(other.midi_learn_engine() == nullptr);
    }
    CHECK(p.midi_learn_engine() == nullptr);
}
