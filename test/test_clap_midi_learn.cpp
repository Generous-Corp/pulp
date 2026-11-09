// MIDI Learn feedback through the CLAP adapter: a mapped parameter's value is
// written to the host's output events as CLAP_EVENT_MIDI, a value that has not
// moved is not repeated, and a plug-in that does not declare MIDI output sends
// nothing.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/midi_learn_support.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/midi_learn_engine.hpp>

#include <clap/events.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace pulp;
namespace clap_adapter = pulp::format::clap_adapter;

namespace {

constexpr state::ParamID kParam = 1;

class LearnClapProcessor : public format::Processor {
  public:
    explicit LearnClapProcessor(bool produces_midi)
        : produces_midi_(produces_midi), handle_(*this, engine) {}

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "ClapLearnTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.clap.learn",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
            .accepts_midi = true,
            .produces_midi = produces_midi_,
        };
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kParam, .name = "P", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const format::ProcessContext&) override {
        ++process_calls;
        format::midi_learn_process_block(
            engine, state(), midi_in, midi_out,
            format::midi_feedback_capabilities(format::MidiLearnFormat::clap, descriptor()));
    }
    int process_calls = 0;
    state::MidiLearnEngine engine;

  private:
    bool produces_midi_;
    format::MidiLearnHandle handle_;
};

LearnClapProcessor* g_proc = nullptr;
std::unique_ptr<format::Processor> make_with_output() {
    auto p = std::make_unique<LearnClapProcessor>(true);
    g_proc = p.get();
    return p;
}
std::unique_ptr<format::Processor> make_without_output() {
    auto p = std::make_unique<LearnClapProcessor>(false);
    g_proc = p.get();
    return p;
}

struct Captured {
    std::vector<clap_event_midi_t> midi;
};

struct Rig {
    static constexpr uint32_t kFrames = 64;
    clap_adapter::PulpClapPlugin plugin;
    std::vector<float> in_l, in_r, out_l, out_r;
    const float* in_ptrs[2]{};
    float* out_ptrs[2]{};
    clap_audio_buffer_t audio_in{}, audio_out{};

    explicit Rig(format::ProcessorFactory factory) {
        plugin.factory = factory;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE(clap_adapter::clap_activate(&plugin.plugin, 48000.0, 32, kFrames));
        in_l.assign(kFrames, 0.0f);
        in_r.assign(kFrames, 0.0f);
        out_l.assign(kFrames, 0.0f);
        out_r.assign(kFrames, 0.0f);
        in_ptrs[0] = in_l.data();
        in_ptrs[1] = in_r.data();
        out_ptrs[0] = out_l.data();
        out_ptrs[1] = out_r.data();
        audio_in.data32 = const_cast<float**>(in_ptrs);
        audio_in.channel_count = 2;
        audio_out.data32 = out_ptrs;
        audio_out.channel_count = 2;
    }
    ~Rig() {
        clap_adapter::clap_deactivate(&plugin.plugin);
    }

    // The adapter may warm up for a block or two before running the Processor.
    Captured run_block() {
        Captured captured;
        clap_input_events_t in{};
        in.ctx = nullptr;
        in.size = [](const clap_input_events_t*) -> uint32_t { return 0; };
        in.get = [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
            return nullptr;
        };
        clap_output_events_t out{};
        out.ctx = &captured;
        out.try_push = [](const clap_output_events_t* list, const clap_event_header_t* h) -> bool {
            auto* c = static_cast<Captured*>(list->ctx);
            if (h->type == CLAP_EVENT_MIDI)
                c->midi.push_back(*reinterpret_cast<const clap_event_midi_t*>(h));
            return true;
        };
        clap_process_t proc{};
        proc.frames_count = kFrames;
        proc.audio_inputs = &audio_in;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = &audio_out;
        proc.audio_outputs_count = 1;
        proc.in_events = &in;
        proc.out_events = &out;
        clap_adapter::clap_process(&plugin.plugin, &proc);
        return captured;
    }
    void settle() {
        for (int i = 0; i < 16 && g_proc->process_calls == 0; ++i)
            run_block();
        REQUIRE(g_proc->process_calls > 0);
    }
};

state::MidiMapping feedback_mapping() {
    state::MidiMapping m;
    m.source = {state::MidiSourceKind::cc7, 4, 74};
    m.id = kParam;
    m.feedback = true;
    return m;
}

} // namespace

TEST_CASE("clap midi learn: feedback is written as CLAP_EVENT_MIDI", "[clap][midi-learn]") {
    Rig rig(&make_with_output);
    rig.settle();
    REQUIRE(g_proc->engine.set_mapping(feedback_mapping()));

    // The first block after the mapping appears syncs the controller (0.5 -> 64).
    auto c = rig.run_block();
    REQUIRE(c.midi.size() == 1);
    CHECK(c.midi[0].data[0] == 0xB4); // control change, channel 5
    CHECK(c.midi[0].data[1] == 74);
    CHECK(c.midi[0].data[2] == 64);

    // Nothing moved: nothing sent.
    CHECK(rig.run_block().midi.empty());

    // The parameter moves: the controller follows.
    g_proc->state().set_normalized(kParam, 1.0f);
    c = rig.run_block();
    REQUIRE(c.midi.size() == 1);
    CHECK(c.midi[0].data[2] == 127);
}

TEST_CASE("clap midi learn: a plug-in without MIDI output sends nothing", "[clap][midi-learn]") {
    Rig rig(&make_without_output);
    rig.settle();
    REQUIRE(g_proc->engine.set_mapping(feedback_mapping()));
    g_proc->state().set_normalized(kParam, 1.0f);
    CHECK(rig.run_block().midi.empty());
    CHECK(rig.run_block().midi.empty());
}
