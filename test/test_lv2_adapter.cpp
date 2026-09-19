#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/format/lv2_adapter.hpp>
#include <pulp/format/lv2_entry.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>

#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>

// This test consumes the public sample-region Processor/example. Keep graph
// construction in that example so format coverage exercises the same API as
// installed consumers.
#include "../examples/sample-region-allpass/allpass_processor.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

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

TEST_CASE("LV2 TTL advertises explicit enum semantics and labels", "[format][lv2][params]") {
    auto desc = make_effect_desc();
    state::StateStore store;
    store.add_parameter({
        .id = 9,
        .name = "Mode",
        .range = {0.0f, 2.0f, 0.0f, 1.0f},
        .kind = state::ParamKind::Enum,
        .value_labels = {"Clean", "Warm", "Hot"},
    });

    const auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/enum");
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:portProperty lv2:enumeration , lv2:integer"));
    REQUIRE_THAT(ttl, ContainsSubstring("rdfs:label \"Clean\" ; rdf:value 0"));
    REQUIRE_THAT(ttl, ContainsSubstring("rdfs:label \"Warm\" ; rdf:value 1"));
    REQUIRE_THAT(ttl, ContainsSubstring("rdfs:label \"Hot\" ; rdf:value 2"));
}

TEST_CASE("LV2 TTL emits a latency-reporting output control port",
          "[format][lv2][issue-mf2]") {
    auto desc = make_effect_desc();
    state::StateStore store;
    add_test_params(store);

    auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/test-lv2");

    // The latency port is an output control port with the standard LV2
    // latency designation + reportsLatency property so hosts do PDC.
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:symbol \"latency\""));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:designation lv2:latency"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:portProperty lv2:reportsLatency"));
    // It is the last port: 2 in + 2 out + 2 control = indices 0-5, latency = 6.
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 6"));
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

// ── URID feature resolution ───────────────────────────────────────────────

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

constexpr state::ParamID kLv2ProbeGainParam = 7;

struct Lv2ProbeCapture {
    int prepare_calls = 0;
    int process_calls = 0;
    int process_buffer_calls = 0;
    int release_calls = 0;
    int last_num_samples = 0;
    double last_sample_rate = 0.0;
    std::size_t process_buffer_input_buses = 0;
    std::size_t process_buffer_output_buses = 0;
    std::size_t process_buffer_active_inputs = 0;
    std::size_t process_buffer_active_outputs = 0;
    bool process_buffer_layouts_match = false;
    bool process_buffer_storage_valid = false;
    std::size_t last_midi_count = 0;
    uint8_t first_status = 0;
    uint8_t first_note = 0;
    // True if the LV2 run path set a (non-null) param-events queue on
    // the Processor before calling process(), proving the uniform sidecar.
    bool param_events_non_null = false;
    std::size_t param_event_count = 0;
    std::size_t param_event_capacity = 0;
    bool param_event_overflowed = false;
    std::uint32_t param_event_drops = 0;
    float gain_seen_in_process = 0.0f;
    bool state_store_wired_during_define = false;

    void reset() { *this = {}; }
};

Lv2ProbeCapture g_lv2_probe;

class Lv2EntryProbeProcessor final : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor desc;
        desc.name = "Lv2EntryProbe";
        desc.manufacturer = "PulpTest";
        desc.bundle_id = "com.pulp.test.lv2-entry-probe";
        desc.version = "1.0.0";
        desc.category = PluginCategory::MidiEffect;
        desc.input_buses = {{"Input", 1}};
        desc.output_buses = {{"Output", 1}};
        desc.accepts_midi = true;
        desc.produces_midi = true;
        return desc;
    }

    void define_parameters(state::StateStore& store) override {
        g_lv2_probe.state_store_wired_during_define = (&state() == &store);
        store.add_parameter({
            .id = kLv2ProbeGainParam,
            .name = "Gain",
            .unit = "",
            .range = {0.0f, 2.0f, 1.0f},
        });
    }

    void prepare(const PrepareContext& context) override {
        ++g_lv2_probe.prepare_calls;
        g_lv2_probe.last_sample_rate = context.sample_rate;
    }

    void release() override { ++g_lv2_probe.release_calls; }

    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext& context) override {
        ++g_lv2_probe.process_calls;
        g_lv2_probe.last_num_samples = context.num_samples;
        g_lv2_probe.last_sample_rate = context.sample_rate;
        g_lv2_probe.last_midi_count = midi_in.size();
        g_lv2_probe.param_events_non_null = (param_events() != nullptr);
        if (auto* events = param_events()) {
            g_lv2_probe.param_event_count = events->size();
            g_lv2_probe.param_event_capacity = events->capacity();
            g_lv2_probe.param_event_overflowed = events->overflowed();
            g_lv2_probe.param_event_drops = events->dropped_event_count();
        }
        g_lv2_probe.gain_seen_in_process =
            state().get_value(kLv2ProbeGainParam);
        for (const auto& ev : midi_in) {
            g_lv2_probe.first_status = ev.data()[0];
            g_lv2_probe.first_note = ev.size() > 1 ? ev.data()[1] : 0;
            break;
        }

        for (size_t c = 0; c < audio_output.num_channels(); ++c) {
            auto* dst = audio_output.channel_ptr(c);
            const auto* src = c < audio_input.num_channels()
                ? audio_input.channel_ptr(c)
                : nullptr;
            for (int i = 0; i < context.num_samples; ++i) {
                dst[i] = (src ? src[i] : 0.0f) * 2.0f;
            }
        }

        auto out = midi::MidiEvent::note_on(0, 65, 110);
        out.sample_offset = 7;
        midi_out.add(out);
    }

    void process(ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext& context) override {
        ++g_lv2_probe.process_buffer_calls;
        g_lv2_probe.process_buffer_input_buses = audio.inputs.size();
        g_lv2_probe.process_buffer_output_buses = audio.outputs.size();
        g_lv2_probe.process_buffer_active_inputs = audio.inputs.active_count();
        g_lv2_probe.process_buffer_active_outputs = audio.outputs.active_count();
        g_lv2_probe.process_buffer_layouts_match = audio.layouts_match_descriptors();
        g_lv2_probe.process_buffer_storage_valid = audio.active_buses_have_storage();

        Processor::process(audio, midi_in, midi_out, context);
    }
};

std::unique_ptr<Processor> make_lv2_entry_probe() {
    return std::make_unique<Lv2EntryProbeProcessor>();
}

struct Lv2FactoryGuard {
    ProcessorFactory previous_factory = lv2_generic::g_factory;
    const char* previous_uri = lv2_generic::g_uri;
    const char* previous_descriptor_uri = lv2_generic::g_lv2_descriptor.URI;

    explicit Lv2FactoryGuard(ProcessorFactory factory) {
        g_lv2_probe.reset();
        lv2_generic::g_factory = factory;
        lv2_generic::g_uri = "http://pulp.audio/test/lv2-entry-probe";
        lv2_generic::g_lv2_descriptor.URI = lv2_generic::g_uri;
    }

    ~Lv2FactoryGuard() {
        lv2_generic::g_factory = previous_factory;
        lv2_generic::g_uri = previous_uri;
        lv2_generic::g_lv2_descriptor.URI = previous_descriptor_uri;
    }
};

struct Lv2HandleGuard {
    LV2_Handle handle = nullptr;

    ~Lv2HandleGuard() {
        if (handle) lv2_generic::cleanup(handle);
    }
};

struct Lv2FeatureBundle {
    std::vector<std::string> table;
    LV2_URID_Map map{&table, &fake_map};
    LV2_Feature map_feature{LV2_URID__map, &map};
    const LV2_Feature* features[2] = {&map_feature, nullptr};
};

uint32_t prepare_sequence(LV2SequenceBuffer& buf, LV2_URID atom_sequence_urid) {
    buf.prepare_as_output_port();
    auto* seq = buf.as_seq();
    const uint32_t capacity = seq->atom.size;
    seq->atom.type = atom_sequence_urid;
    seq->body.unit = 0;
    seq->body.pad = 0;
    lv2_atom_sequence_clear(seq);
    return capacity;
}

void append_midi_event(LV2_Atom_Sequence* seq,
                       uint32_t capacity,
                       LV2_URID midi_event_urid,
                       int64_t frame,
                       uint8_t status,
                       uint8_t data1,
                       uint8_t data2) {
    struct alignas(8) {
        LV2_Atom_Event hdr;
        uint8_t payload[3];
    } pkt{};
    pkt.hdr.time.frames = frame;
    pkt.hdr.body.type = midi_event_urid;
    pkt.hdr.body.size = 3;
    pkt.payload[0] = status;
    pkt.payload[1] = data1;
    pkt.payload[2] = data2;
    REQUIRE(lv2_atom_sequence_append_event(seq, capacity, &pkt.hdr));
}
} // namespace

TEST_CASE("LV2 sample-region allpass preserves catalog, partitions, and fresh-instance parity",
          "[format][lv2][sample-region]") {
    using pulp::examples::create_sample_region_allpass;

    Lv2FactoryGuard factory(&create_sample_region_allpass);
    Lv2FeatureBundle features;

    auto render = [&](std::vector<float>& output, bool vary_coefficient) {
        Lv2HandleGuard handle{
            lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor,
                                     48000.0, "", features.features)};
        REQUIRE(handle.handle != nullptr);
        auto* instance = static_cast<PulpLv2Instance*>(handle.handle);
        auto* allpass = dynamic_cast<pulp::examples::SampleRegionAllpassProcessor*>(
            instance->processor.get());
        REQUIRE(allpass != nullptr);
        REQUIRE(allpass->ready());
        REQUIRE(instance->num_audio_inputs == 1);
        REQUIRE(instance->num_audio_outputs == 1);
        REQUIRE(instance->num_params == 1);
        REQUIRE(instance->param_ids.size() == 1);
        REQUIRE(instance->param_ids[0] == pulp::examples::kAllpassCoefficient);
        REQUIRE(instance->store.get_value(pulp::examples::kAllpassCoefficient) ==
                Catch::Approx(0.5f));

        float coefficient = 0.5f;
        float latency = -1.0f;
        std::vector<float> input(output.size(), 0.0f);
        input[0] = 1.0f;
        lv2_generic::connect_port(handle.handle, 2, &coefficient);
        lv2_generic::connect_port(handle.handle, 3, &latency);
        lv2_generic::activate(handle.handle);

        constexpr std::array<std::size_t, 2> partitions = {3, 5};
        std::size_t offset = 0;
        for (const auto count : partitions) {
            if (vary_coefficient && offset == partitions[0])
                coefficient = 0.25f;
            lv2_generic::connect_port(handle.handle, 0, input.data() + offset);
            lv2_generic::connect_port(handle.handle, 1, output.data() + offset);
            lv2_generic::run(handle.handle, static_cast<uint32_t>(count));
            offset += count;
        }
        lv2_generic::deactivate(handle.handle);
        REQUIRE(latency == 0.0f);
        REQUIRE(instance->store.get_value(pulp::examples::kAllpassCoefficient) ==
                Catch::Approx(vary_coefficient ? 0.25f : 0.5f));
    };

    std::vector<float> first(8, 0.0f);
    render(first, true);

    auto oracle = [&](float second_block_coefficient) {
        std::vector<float> expected(first.size(), 0.0f);
        float x_previous = 0.0f;
        float y_previous = 0.0f;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const float coefficient = i < 3 ? 0.5f : second_block_coefficient;
            const float x = i == 0 ? 1.0f : 0.0f;
            const float y = coefficient * x + x_previous - coefficient * y_previous;
            expected[i] = y;
            x_previous = x;
            y_previous = y;
        }
        return expected;
    };
    const auto expected = oracle(0.25f);
    for (std::size_t i = 0; i < first.size(); ++i)
        REQUIRE(first[i] == Catch::Approx(expected[i]).margin(1e-6f));

    // A fresh LV2 instance is the format's reset/reload boundary. Its default
    // stream must not depend on the prior instance's UnitDelay state.
    std::vector<float> fresh(first.size(), 0.0f);
    render(fresh, false);
    const auto expected_fresh = oracle(0.5f);
    for (std::size_t i = 0; i < fresh.size(); ++i)
        REQUIRE(fresh[i] == Catch::Approx(expected_fresh[i]).margin(1e-6f));
}

TEST_CASE("LV2 generic entry refuses instantiation without URID map",
          "[format][lv2][issue-493]") {
    Lv2FactoryGuard factory(&make_lv2_entry_probe);
    const LV2_Feature* empty_features[] = {nullptr};

    REQUIRE(lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor,
                                     48000.0,
                                     "",
                                     nullptr) == nullptr);
    REQUIRE(lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor,
                                     48000.0,
                                     "",
                                     empty_features) == nullptr);
}

TEST_CASE("LV2 generic entry wires ports, audio, control values, and MIDI",
          "[format][lv2][issue-493]") {
    Lv2FactoryGuard factory(&make_lv2_entry_probe);
    Lv2FeatureBundle features;

    Lv2HandleGuard handle{
        lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor,
                                 44100.0,
                                 "",
                                 features.features)
    };
    REQUIRE(handle.handle != nullptr);

    auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
    REQUIRE(inst->processor != nullptr);
    REQUIRE(inst->sample_rate == 44100.0);
    REQUIRE(inst->num_audio_inputs == 1);
    REQUIRE(inst->num_audio_outputs == 1);
    REQUIRE(inst->num_params == 1);
    REQUIRE(inst->param_ids.size() == 1);
    REQUIRE(inst->param_ids[0] == kLv2ProbeGainParam);
    REQUIRE(g_lv2_probe.state_store_wired_during_define);
    REQUIRE(inst->accepts_midi);
    REQUIRE(inst->produces_midi);
    REQUIRE(inst->urid_midi_event != 0);
    REQUIRE(inst->urid_atom_sequence != 0);
    REQUIRE(inst->urid_atom_chunk != 0);
    REQUIRE(g_lv2_probe.prepare_calls == 1);

    float input[4] = {0.25f, -0.5f, 1.0f, 0.0f};
    float output[4] = {};
    float gain = 0.5f;

    LV2SequenceBuffer midi_in;
    const uint32_t midi_in_capacity =
        prepare_sequence(midi_in, inst->urid_atom_sequence);
    append_midi_event(midi_in.as_seq(), midi_in_capacity,
                      inst->urid_midi_event, 3, 0x90, 60, 100);

    LV2SequenceBuffer midi_out;
    midi_out.prepare_as_output_port();

    lv2_generic::connect_port(handle.handle, 0, input);
    lv2_generic::connect_port(handle.handle, 1, output);
    lv2_generic::connect_port(handle.handle, 2, &gain);
    lv2_generic::connect_port(handle.handle, 3, midi_in.as_seq());
    lv2_generic::connect_port(handle.handle, 4, midi_out.as_seq());
    lv2_generic::connect_port(handle.handle, 999, &gain);
    lv2_generic::activate(handle.handle);

    lv2_generic::run(handle.handle, 4);
    lv2_generic::deactivate(handle.handle);

    REQUIRE(inst->store.get_value(kLv2ProbeGainParam) == 0.5f);
    REQUIRE(g_lv2_probe.process_calls == 1);
    REQUIRE(g_lv2_probe.process_buffer_calls == 1);
    REQUIRE(g_lv2_probe.process_buffer_input_buses == 1);
    REQUIRE(g_lv2_probe.process_buffer_output_buses == 1);
    REQUIRE(g_lv2_probe.process_buffer_active_inputs == 1);
    REQUIRE(g_lv2_probe.process_buffer_active_outputs == 1);
    REQUIRE(g_lv2_probe.process_buffer_layouts_match);
    REQUIRE(g_lv2_probe.process_buffer_storage_valid);
    // The LV2 run path provides a uniform (non-null) param-events
    // queue to the Processor, matching VST3/CLAP/AUv3.
    REQUIRE(g_lv2_probe.param_events_non_null);
    REQUIRE(g_lv2_probe.param_event_count == 0);
    REQUIRE(g_lv2_probe.param_event_capacity ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE_FALSE(g_lv2_probe.param_event_overflowed);
    REQUIRE(g_lv2_probe.param_event_drops == 0);
    REQUIRE(g_lv2_probe.gain_seen_in_process == 0.5f);
    REQUIRE(g_lv2_probe.last_num_samples == 4);
    REQUIRE(g_lv2_probe.last_sample_rate == 44100.0);
    REQUIRE(g_lv2_probe.last_midi_count == 1);
    REQUIRE(g_lv2_probe.first_status == 0x90);
    REQUIRE(g_lv2_probe.first_note == 60);

    REQUIRE(output[0] == 0.5f);
    REQUIRE(output[1] == -1.0f);
    REQUIRE(output[2] == 2.0f);
    REQUIRE(output[3] == 0.0f);

    std::vector<std::tuple<int64_t, LV2_URID, uint32_t, uint8_t, uint8_t, uint8_t>> seen;
    LV2_ATOM_SEQUENCE_FOREACH(midi_out.as_seq(), ev) {
        const auto* data = reinterpret_cast<const uint8_t*>(ev + 1);
        seen.emplace_back(ev->time.frames, ev->body.type, ev->body.size,
                          data[0], data[1], data[2]);
    }
    REQUIRE(seen.size() == 1);
    REQUIRE(std::get<0>(seen[0]) == 7);
    REQUIRE(std::get<1>(seen[0]) == inst->urid_midi_event);
    REQUIRE(std::get<2>(seen[0]) == 3);
    REQUIRE(std::get<3>(seen[0]) == 0x90);
    REQUIRE(std::get<4>(seen[0]) == 65);
    REQUIRE(std::get<5>(seen[0]) == 110);
}

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

// ── state:interface, time:Position, and buf-size options ─────────────────

namespace {

// A processor whose interesting state is deliberately NOT a parameter — the
// class of state LV2 lost entirely before state:interface was wired, because a
// host only ever saved the control ports.
constexpr state::ParamID kLv2StateGainParam = 11;

struct Lv2StateCapture {
    int deserialize_calls = 0;
    int non_realtime_ticks = 0;
    bool last_deserialize_empty = false;
};

Lv2StateCapture g_lv2_state;

class Lv2StateProbeProcessor final : public Processor {
  public:
    // Opaque, non-parameter payload: a sample path plus a blob, standing in
    // for whatever a sampler would carry.
    std::string sample_path = "default.wav";
    std::vector<uint8_t> blob{1, 2, 3};

    PluginDescriptor descriptor() const override {
        PluginDescriptor desc;
        desc.name = "Lv2StateProbe";
        desc.manufacturer = "PulpTest";
        desc.bundle_id = "com.pulp.test.lv2-state-probe";
        desc.version = "1.0.0";
        desc.category = PluginCategory::Effect;
        desc.input_buses = {{"Input", 1}};
        desc.output_buses = {{"Output", 1}};
        desc.accepts_midi = true;
        desc.produces_midi = false;
        return desc;
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kLv2StateGainParam,
            .name = "Gain",
            .unit = "",
            .range = {0.0f, 2.0f, 1.0f},
        });
    }

    void prepare(const PrepareContext&) override {}

    void process(audio::BufferView<float>& audio_output, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const ProcessContext& context) override {
        for (size_t c = 0; c < audio_output.num_channels(); ++c) {
            auto* dst = audio_output.channel_ptr(c);
            for (int i = 0; i < context.num_samples; ++i)
                dst[i] = 0.0f;
        }
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        std::vector<uint8_t> out;
        out.push_back(static_cast<uint8_t>(sample_path.size()));
        out.insert(out.end(), sample_path.begin(), sample_path.end());
        out.insert(out.end(), blob.begin(), blob.end());
        return out;
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        ++g_lv2_state.deserialize_calls;
        g_lv2_state.last_deserialize_empty = data.empty();
        if (data.empty())
            return true;
        const std::size_t path_len = data[0];
        if (data.size() < 1 + path_len)
            return false;
        sample_path.assign(reinterpret_cast<const char*>(data.data() + 1), path_len);
        blob.assign(data.begin() + 1 + static_cast<std::ptrdiff_t>(path_len), data.end());
        return true;
    }

    void on_non_realtime_tick() override {
        ++g_lv2_state.non_realtime_ticks;
    }
};

std::unique_ptr<Processor> make_lv2_state_probe() {
    return std::make_unique<Lv2StateProbeProcessor>();
}

// Minimal in-memory host side of state:interface — one property map, exactly
// what a real host persists.
struct FakeStateStorage {
    std::vector<uint8_t> value;
    uint32_t key = 0;
    uint32_t type = 0;
    uint32_t flags = 0;
    bool stored = false;
};

LV2_State_Status fake_store(LV2_State_Handle handle, uint32_t key, const void* value, size_t size,
                            uint32_t type, uint32_t flags) {
    auto* storage = static_cast<FakeStateStorage*>(handle);
    // The spec forbids a zero-size property.
    if (size == 0 || value == nullptr)
        return LV2_STATE_ERR_UNKNOWN;
    storage->key = key;
    storage->type = type;
    storage->flags = flags;
    const auto* bytes = static_cast<const uint8_t*>(value);
    storage->value.assign(bytes, bytes + size);
    storage->stored = true;
    return LV2_STATE_SUCCESS;
}

const void* fake_retrieve(LV2_State_Handle handle, uint32_t key, size_t* size, uint32_t* type,
                          uint32_t* flags) {
    auto* storage = static_cast<FakeStateStorage*>(handle);
    if (!storage->stored || storage->key != key)
        return nullptr;
    if (size)
        *size = storage->value.size();
    if (type)
        *type = storage->type;
    if (flags)
        *flags = storage->flags;
    return storage->value.data();
}

// Build one time:Position object event for an atom sequence.
class TimePositionEvent {
  public:
    TimePositionEvent(LV2_URID object_type, LV2_URID otype, int64_t frames) {
        auto* ev = header();
        ev->time.frames = frames;
        ev->body.type = object_type;
        ev->body.size = sizeof(LV2_Atom_Object_Body);
        auto* obj_body =
            reinterpret_cast<LV2_Atom_Object_Body*>(storage_.data() + sizeof(LV2_Atom_Event));
        obj_body->id = 0;
        obj_body->otype = otype;
        used_ = sizeof(LV2_Atom_Event) + sizeof(LV2_Atom_Object_Body);
    }

    template <typename T> void add(LV2_URID key, LV2_URID type, T value) {
        static_assert(sizeof(T) <= 8);
        auto* prop = reinterpret_cast<LV2_Atom_Property_Body*>(storage_.data() + used_);
        prop->key = key;
        prop->context = 0;
        prop->value.size = sizeof(T);
        prop->value.type = type;
        std::memcpy(storage_.data() + used_ + sizeof(LV2_Atom_Property_Body), &value, sizeof(T));
        const auto bytes =
            static_cast<uint32_t>(sizeof(LV2_Atom_Property_Body) + lv2_atom_pad_size(sizeof(T)));
        used_ += bytes;
        header()->body.size += bytes;
    }

    LV2_Atom_Event* header() {
        return reinterpret_cast<LV2_Atom_Event*>(storage_.data());
    }

  private:
    alignas(8) std::array<uint8_t, 512> storage_{};
    uint32_t used_ = 0;
};

struct Lv2TimeCapture {
    bool is_playing = false;
    double tempo_bpm = 0.0;
    double position_beats = 0.0;
    int64_t position_samples = 0;
    int64_t bar = 0;
    int time_sig_numerator = 0;
    int time_sig_denominator = 0;
    bool tempo_valid = false;
    bool beats_valid = false;
    bool samples_valid = false;
    bool playing_valid = false;
    bool looping_valid = false;
    bool transport_jump = false;
    int blocks = 0;
    int last_num_samples = 0;
    float tail_sample = -1.0f;
};

Lv2TimeCapture g_lv2_time;

class Lv2TimeProbeProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor desc;
        desc.name = "Lv2TimeProbe";
        desc.manufacturer = "PulpTest";
        desc.bundle_id = "com.pulp.test.lv2-time-probe";
        desc.version = "1.0.0";
        desc.category = PluginCategory::Effect;
        desc.input_buses = {{"Input", 1}};
        desc.output_buses = {{"Output", 1}};
        desc.accepts_midi = true;
        desc.produces_midi = false;
        return desc;
    }

    void define_parameters(state::StateStore&) override {}

    void prepare(const PrepareContext& context) override {
        prepared_max_buffer_size = context.max_buffer_size;
    }

    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input, midi::MidiBuffer&,
                 midi::MidiBuffer&, const ProcessContext& context) override {
        ++g_lv2_time.blocks;
        g_lv2_time.last_num_samples = context.num_samples;
        g_lv2_time.is_playing = context.is_playing;
        g_lv2_time.tempo_bpm = context.tempo_bpm;
        g_lv2_time.position_beats = context.position_beats;
        g_lv2_time.position_samples = context.position_samples;
        g_lv2_time.bar = context.bar;
        g_lv2_time.time_sig_numerator = context.time_sig_numerator;
        g_lv2_time.time_sig_denominator = context.time_sig_denominator;
        g_lv2_time.tempo_valid = context.has_transport(TransportField::Tempo);
        g_lv2_time.beats_valid = context.has_transport(TransportField::BeatPosition);
        g_lv2_time.samples_valid = context.has_transport(TransportField::SamplePosition);
        g_lv2_time.playing_valid = context.has_transport(TransportField::Playing);
        g_lv2_time.looping_valid = context.has_transport(TransportField::Looping);
        g_lv2_time.transport_jump = context.transport_jump;

        for (size_t c = 0; c < audio_output.num_channels(); ++c) {
            auto* dst = audio_output.channel_ptr(c);
            const auto* src = c < audio_input.num_channels() ? audio_input.channel_ptr(c) : nullptr;
            for (int i = 0; i < context.num_samples; ++i) {
                dst[i] = src ? src[i] : 0.0f;
            }
        }
    }

    int prepared_max_buffer_size = 0;
};

std::unique_ptr<Processor> make_lv2_time_probe() {
    return std::make_unique<Lv2TimeProbeProcessor>();
}

} // namespace

TEST_CASE("LV2 TTL declares state transport buffer-size and hard-RT capability",
          "[format][lv2][state][transport]") {
    auto desc = make_effect_desc();
    desc.accepts_midi = true;
    desc.produces_midi = true;
    state::StateStore store;
    add_test_params(store);

    auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/test-lv2");

    REQUIRE_THAT(ttl, ContainsSubstring("lv2:extensionData state:interface"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:optionalFeature lv2:hardRTCapable"));
    REQUIRE_THAT(ttl, ContainsSubstring("bufsz:boundedBlockLength"));
    REQUIRE_THAT(ttl, ContainsSubstring("opts:supportedOption bufsz:maxBlockLength"));
    REQUIRE_THAT(ttl, ContainsSubstring("time:Position"));
    // The header comments have long promised a minimumSize for the atom ports;
    // it must actually be emitted.
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:minimumSize"));
    REQUIRE_THAT(ttl, ContainsSubstring("@prefix state:"));
    REQUIRE_THAT(ttl, ContainsSubstring("@prefix time:"));
    REQUIRE_THAT(ttl, ContainsSubstring("@prefix bufsz:"));
    REQUIRE_THAT(ttl, ContainsSubstring("@prefix opts:"));
}

TEST_CASE("LV2 extension_data serves state:interface only", "[format][lv2][state]") {
    const void* state_ext = lv2_generic::extension_data(LV2_STATE__interface);
    REQUIRE(state_ext != nullptr);
    const auto* iface = static_cast<const LV2_State_Interface*>(state_ext);
    REQUIRE(iface->save != nullptr);
    REQUIRE(iface->restore != nullptr);
    REQUIRE(lv2_generic::extension_data("http://example.com/unknown") == nullptr);
    REQUIRE(lv2_generic::extension_data(nullptr) == nullptr);
    // The descriptor must actually hand the host this entry point.
    REQUIRE(lv2_generic::g_lv2_descriptor.extension_data == &lv2_generic::extension_data);
}

TEST_CASE("LV2 state:interface round-trips non-parameter plugin state", "[format][lv2][state]") {
    g_lv2_state = {};
    FakeStateStorage storage;

    const auto* iface =
        static_cast<const LV2_State_Interface*>(lv2_generic::extension_data(LV2_STATE__interface));
    REQUIRE(iface != nullptr);

    {
        Lv2FactoryGuard factory(&make_lv2_state_probe);
        Lv2FeatureBundle features;
        Lv2HandleGuard handle{lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "",
                                                       features.features)};
        REQUIRE(handle.handle != nullptr);

        auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
        auto* probe = static_cast<Lv2StateProbeProcessor*>(inst->processor.get());
        // State a control port cannot carry.
        probe->sample_path = "kick-07.wav";
        probe->blob = {0xDE, 0xAD, 0xBE, 0xEF};

        REQUIRE(iface->save(handle.handle, &fake_store, &storage, 0, nullptr) == LV2_STATE_SUCCESS);
    }

    REQUIRE(storage.stored);
    REQUIRE_FALSE(storage.value.empty());
    REQUIRE((storage.flags & LV2_STATE_IS_POD) != 0);
    REQUIRE((storage.flags & LV2_STATE_IS_PORTABLE) != 0);

    // A fresh instance, as a host would create when reopening the session.
    Lv2FactoryGuard factory(&make_lv2_state_probe);
    Lv2FeatureBundle features;
    Lv2HandleGuard handle{
        lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "", features.features)};
    REQUIRE(handle.handle != nullptr);
    auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
    auto* probe = static_cast<Lv2StateProbeProcessor*>(inst->processor.get());
    REQUIRE(probe->sample_path == "default.wav");

    // The URID table is per-bundle, so the saved key must map identically in a
    // new instance for the restore to find anything — which is the whole point
    // of keying on a stable URI.
    REQUIRE(storage.key == inst->urid_state_blob);
    REQUIRE(storage.type == inst->urid_atom_chunk);

    REQUIRE(iface->restore(handle.handle, &fake_retrieve, &storage, 0, nullptr) ==
            LV2_STATE_SUCCESS);
    REQUIRE(probe->sample_path == "kick-07.wav");
    REQUIRE(probe->blob == std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
    REQUIRE(g_lv2_state.deserialize_calls == 1);
    REQUIRE_FALSE(g_lv2_state.last_deserialize_empty);
    REQUIRE(g_lv2_state.non_realtime_ticks == 1);
}

TEST_CASE("LV2 state restore tolerates an absent or mistyped property", "[format][lv2][state]") {
    g_lv2_state = {};
    const auto* iface =
        static_cast<const LV2_State_Interface*>(lv2_generic::extension_data(LV2_STATE__interface));
    REQUIRE(iface != nullptr);

    Lv2FactoryGuard factory(&make_lv2_state_probe);
    Lv2FeatureBundle features;
    Lv2HandleGuard handle{
        lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "", features.features)};
    REQUIRE(handle.handle != nullptr);
    auto* inst = static_cast<PulpLv2Instance*>(handle.handle);

    // Host resetting the plugin with an empty map: fall back to defaults.
    FakeStateStorage empty;
    REQUIRE(iface->restore(handle.handle, &fake_retrieve, &empty, 0, nullptr) == LV2_STATE_SUCCESS);
    REQUIRE(g_lv2_state.deserialize_calls == 0);

    // A property stored under our key with the wrong type is rejected rather
    // than fed to the envelope parser.
    FakeStateStorage mistyped;
    mistyped.stored = true;
    mistyped.key = inst->urid_state_blob;
    mistyped.type = inst->urid_atom_sequence;
    mistyped.value = {1, 2, 3, 4};
    REQUIRE(iface->restore(handle.handle, &fake_retrieve, &mistyped, 0, nullptr) ==
            LV2_STATE_ERR_BAD_TYPE);
    REQUIRE(g_lv2_state.deserialize_calls == 0);
}

TEST_CASE("max_block_length_from_options reads bufsz:maxBlockLength", "[format][lv2][bufsize]") {
    constexpr LV2_URID kKey = 40;
    constexpr LV2_URID kInt = 41;
    constexpr LV2_URID kLong = 42;

    REQUIRE(max_block_length_from_options(nullptr, kKey, kInt, kLong, 4096) == 4096);

    const int32_t as_int = 8192;
    const int64_t as_long = 16384;
    const int32_t negative = -1;
    const float wrong_type = 2048.0f;

    const LV2_Options_Option int_opts[] = {
        {LV2_OPTIONS_INSTANCE, 0, kKey, static_cast<uint32_t>(sizeof(as_int)), kInt, &as_int},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    REQUIRE(max_block_length_from_options(int_opts, kKey, kInt, kLong, 4096) == 8192);

    const LV2_Options_Option long_opts[] = {
        {LV2_OPTIONS_INSTANCE, 0, kKey, static_cast<uint32_t>(sizeof(as_long)), kLong, &as_long},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    REQUIRE(max_block_length_from_options(long_opts, kKey, kInt, kLong, 4096) == 16384);

    // Another key entirely, a negative value, and a type we do not model all
    // leave the fallback in place rather than producing a nonsense ceiling.
    const LV2_Options_Option other_key[] = {
        {LV2_OPTIONS_INSTANCE, 0, kKey + 9, static_cast<uint32_t>(sizeof(as_int)), kInt, &as_int},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    REQUIRE(max_block_length_from_options(other_key, kKey, kInt, kLong, 4096) == 4096);

    const LV2_Options_Option bad_value[] = {
        {LV2_OPTIONS_INSTANCE, 0, kKey, static_cast<uint32_t>(sizeof(negative)), kInt, &negative},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    REQUIRE(max_block_length_from_options(bad_value, kKey, kInt, kLong, 4096) == 4096);

    const LV2_Options_Option bad_type[] = {
        {LV2_OPTIONS_INSTANCE, 0, kKey, static_cast<uint32_t>(sizeof(wrong_type)), kInt + 77,
         &wrong_type},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    REQUIRE(max_block_length_from_options(bad_type, kKey, kInt, kLong, 4096) == 4096);
}

TEST_CASE("find_options locates options:options in features", "[format][lv2][bufsize]") {
    const LV2_Options_Option opts[] = {
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    LV2_Feature other{"http://example.com/irrelevant", nullptr};
    LV2_Feature options_feature{LV2_OPTIONS__options, const_cast<LV2_Options_Option*>(opts)};
    const LV2_Feature* features[] = {&other, &options_feature, nullptr};
    REQUIRE(find_options(features) == opts);

    const LV2_Feature* without[] = {&other, nullptr};
    REQUIRE(find_options(without) == nullptr);
    REQUIRE(find_options(nullptr) == nullptr);
}

TEST_CASE("LV2 instantiate prepares for the host's maxBlockLength", "[format][lv2][bufsize]") {
    Lv2FactoryGuard factory(&make_lv2_time_probe);
    g_lv2_time = {};

    SECTION("host supplies the option") {
        Lv2FeatureBundle urid;
        const int32_t max_block = 8192;
        std::vector<std::string>& table = urid.table;
        // Map the key through the same table the instance will use, so the
        // URIDs match.
        const LV2_URID key = urid.map.map(urid.map.handle, LV2_BUF_SIZE__maxBlockLength);
        const LV2_URID atom_int = urid.map.map(urid.map.handle, LV2_ATOM__Int);
        REQUIRE(!table.empty());
        const LV2_Options_Option opts[] = {
            {LV2_OPTIONS_INSTANCE, 0, key, static_cast<uint32_t>(sizeof(max_block)), atom_int,
             &max_block},
            {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
        };
        LV2_Feature options_feature{LV2_OPTIONS__options, const_cast<LV2_Options_Option*>(opts)};
        const LV2_Feature* features[] = {&urid.map_feature, &options_feature, nullptr};

        Lv2HandleGuard handle{
            lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "", features)};
        REQUIRE(handle.handle != nullptr);
        auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
        REQUIRE(inst->max_block_length == 8192);
        REQUIRE(
            static_cast<Lv2TimeProbeProcessor*>(inst->processor.get())->prepared_max_buffer_size ==
            8192);
    }

    SECTION("host supplies no options feature") {
        Lv2FeatureBundle features;
        Lv2HandleGuard handle{lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "",
                                                       features.features)};
        REQUIRE(handle.handle != nullptr);
        auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
        REQUIRE(inst->max_block_length == kDefaultMaxBlockLength);
        REQUIRE(
            static_cast<Lv2TimeProbeProcessor*>(inst->processor.get())->prepared_max_buffer_size ==
            kDefaultMaxBlockLength);
    }
}

TEST_CASE("LV2 run() clamps a block past the prepared maximum and silences the tail",
          "[format][lv2][bufsize]") {
    Lv2FactoryGuard factory(&make_lv2_time_probe);
    g_lv2_time = {};
    Lv2FeatureBundle features;
    Lv2HandleGuard handle{
        lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "", features.features)};
    REQUIRE(handle.handle != nullptr);
    auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
    // Shrink the prepared ceiling rather than rendering a 4097-frame buffer.
    inst->max_block_length = 4;

    std::array<float, 8> input{};
    std::array<float, 8> output{};
    input.fill(0.75f);
    output.fill(9.0f);

    lv2_generic::connect_port(handle.handle, 0, input.data());
    lv2_generic::connect_port(handle.handle, 1, output.data());
    lv2_generic::run(handle.handle, 8);

    REQUIRE(g_lv2_time.last_num_samples == 4);
    for (int i = 0; i < 4; ++i)
        REQUIRE(output[static_cast<size_t>(i)] == 0.75f);
    for (int i = 4; i < 8; ++i)
        REQUIRE(output[static_cast<size_t>(i)] == 0.0f);
}

TEST_CASE("LV2 run() decodes time:Position into the process context", "[format][lv2][transport]") {
    Lv2FactoryGuard factory(&make_lv2_time_probe);
    g_lv2_time = {};
    Lv2FeatureBundle features;
    Lv2HandleGuard handle{
        lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "", features.features)};
    REQUIRE(handle.handle != nullptr);
    auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
    const auto& urids = inst->time_urids;
    REQUIRE(urids.time_position != 0);

    std::array<float, 64> input{};
    std::array<float, 64> output{};
    LV2SequenceBuffer seq;
    const uint32_t capacity = prepare_sequence(seq, inst->urid_atom_sequence);

    TimePositionEvent pos(urids.atom_object, urids.time_position, 0);
    pos.add<float>(urids.time_speed, urids.atom_float, 1.0f);
    pos.add<int64_t>(urids.time_frame, urids.atom_long, 96000);
    pos.add<double>(urids.time_beat, urids.atom_double, 8.0);
    pos.add<int64_t>(urids.time_bar, urids.atom_long, 2);
    pos.add<float>(urids.time_beats_per_bar, urids.atom_float, 4.0f);
    pos.add<int32_t>(urids.time_beat_unit, urids.atom_int, 4);
    pos.add<float>(urids.time_beats_per_minute, urids.atom_float, 140.0f);
    REQUIRE(lv2_atom_sequence_append_event(seq.as_seq(), capacity, pos.header()));

    lv2_generic::connect_port(handle.handle, 0, input.data());
    lv2_generic::connect_port(handle.handle, 1, output.data());
    lv2_generic::connect_port(handle.handle, 2, seq.as_seq());
    lv2_generic::run(handle.handle, 32);

    REQUIRE(g_lv2_time.blocks == 1);
    REQUIRE(g_lv2_time.is_playing);
    REQUIRE(g_lv2_time.playing_valid);
    REQUIRE(g_lv2_time.tempo_valid);
    REQUIRE(g_lv2_time.tempo_bpm == 140.0);
    REQUIRE(g_lv2_time.samples_valid);
    REQUIRE(g_lv2_time.position_samples == 96000);
    REQUIRE(g_lv2_time.beats_valid);
    REQUIRE(g_lv2_time.position_beats == 8.0);
    REQUIRE(g_lv2_time.bar == 2);
    REQUIRE(g_lv2_time.time_sig_numerator == 4);
    REQUIRE(g_lv2_time.time_sig_denominator == 4);
    // LV2's time extension models no cycle range, so looping must stay
    // explicitly unavailable rather than reading as "not looping".
    REQUIRE_FALSE(g_lv2_time.looping_valid);
}

TEST_CASE("LV2 transport survives a block the host sends no position in",
          "[format][lv2][transport]") {
    Lv2FactoryGuard factory(&make_lv2_time_probe);
    g_lv2_time = {};
    Lv2FeatureBundle features;
    Lv2HandleGuard handle{
        lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "", features.features)};
    REQUIRE(handle.handle != nullptr);
    auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
    const auto& urids = inst->time_urids;

    std::array<float, 64> input{};
    std::array<float, 64> output{};
    LV2SequenceBuffer seq;
    const uint32_t capacity = prepare_sequence(seq, inst->urid_atom_sequence);

    TimePositionEvent pos(urids.atom_object, urids.time_position, 0);
    pos.add<float>(urids.time_speed, urids.atom_float, 1.0f);
    pos.add<int64_t>(urids.time_frame, urids.atom_long, 1000);
    pos.add<double>(urids.time_beat, urids.atom_double, 0.0);
    pos.add<float>(urids.time_beats_per_minute, urids.atom_float, 120.0f);
    REQUIRE(lv2_atom_sequence_append_event(seq.as_seq(), capacity, pos.header()));

    lv2_generic::connect_port(handle.handle, 0, input.data());
    lv2_generic::connect_port(handle.handle, 1, output.data());
    lv2_generic::connect_port(handle.handle, 2, seq.as_seq());
    lv2_generic::run(handle.handle, 32);
    REQUIRE(g_lv2_time.position_samples == 1000);

    // Second block: the host sends an empty sequence, as a host that only
    // publishes a position on change does. The transport must keep rolling —
    // a frozen sample position would read as a seek and reset synced DSP.
    prepare_sequence(seq, inst->urid_atom_sequence);
    lv2_generic::run(handle.handle, 32);

    REQUIRE(g_lv2_time.is_playing);
    REQUIRE(g_lv2_time.tempo_bpm == 120.0);
    REQUIRE(g_lv2_time.position_samples == 1032);
    REQUIRE_FALSE(g_lv2_time.transport_jump);
}

// ── Port-index cross-check + channel-ceiling admission ───────────────────

namespace {

// Descriptor and parameter list the layout probe reports. Set before each
// instantiate() so one probe covers every plugin shape.
PluginDescriptor g_layout_probe_desc;
std::vector<state::ParamInfo> g_layout_probe_params;

class Lv2LayoutProbeProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        return g_layout_probe_desc;
    }

    void define_parameters(state::StateStore& store) override {
        for (const auto& info : g_layout_probe_params)
            store.add_parameter(info);
    }

    void prepare(const PrepareContext&) override {}

    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const ProcessContext&) override {}
};

std::unique_ptr<Processor> make_lv2_layout_probe() {
    return std::make_unique<Lv2LayoutProbeProcessor>();
}

// One port block as the generated Turtle declares it. Types are read off the
// block's `a` line rather than matched as free substrings, so a type that
// landed on a different subject does not count.
struct TtlPort {
    int index = -1;
    bool is_input = false;
    bool is_output = false;
    bool is_audio = false;
    bool is_control = false;
    bool is_atom = false;
};

// Walk the port blocks of a generated plugin.ttl. Each is a bracketed blank
// node on its own lines; inline blank nodes (doap:developer, lv2:scalePoint)
// are single-line and carry no lv2:index, so they are skipped.
std::vector<TtlPort> parse_ttl_ports(const std::string& ttl) {
    std::vector<TtlPort> ports;
    const std::string open = "\n    [\n";
    const std::string close = "\n    ]";
    std::size_t pos = 0;
    while ((pos = ttl.find(open, pos)) != std::string::npos) {
        const std::size_t body_begin = pos + open.size();
        const std::size_t body_end = ttl.find(close, body_begin);
        REQUIRE(body_end != std::string::npos);
        const std::string body = ttl.substr(body_begin, body_end - body_begin);
        pos = body_end + close.size();

        const std::size_t index_at = body.find("lv2:index ");
        if (index_at == std::string::npos)
            continue;

        TtlPort port;
        port.index = std::stoi(body.substr(index_at + std::strlen("lv2:index ")));

        const std::size_t types_at = body.find("        a ");
        REQUIRE(types_at != std::string::npos);
        const std::string types = body.substr(types_at, body.find('\n', types_at) - types_at);
        port.is_input = types.find("lv2:InputPort") != std::string::npos;
        port.is_output = types.find("lv2:OutputPort") != std::string::npos;
        port.is_audio = types.find("lv2:AudioPort") != std::string::npos;
        port.is_control = types.find("lv2:ControlPort") != std::string::npos;
        port.is_atom = types.find("atom:AtomPort") != std::string::npos;
        ports.push_back(port);
    }
    return ports;
}

// What the manifest says a port is. The adapter's own classification is never
// consulted here — that is the other side of the comparison.
Lv2PortKind kind_declared_in_ttl(const TtlPort& port) {
    if (port.is_audio && port.is_input)
        return Lv2PortKind::AudioIn;
    if (port.is_audio && port.is_output)
        return Lv2PortKind::AudioOut;
    if (port.is_atom && port.is_input)
        return Lv2PortKind::AtomIn;
    if (port.is_atom && port.is_output)
        return Lv2PortKind::AtomOut;
    if (port.is_control && port.is_input)
        return Lv2PortKind::Control;
    if (port.is_control && port.is_output)
        return Lv2PortKind::Latency;
    return Lv2PortKind::None;
}

PluginDescriptor make_layout_desc(std::vector<BusInfo> inputs, std::vector<BusInfo> outputs,
                                  bool accepts_midi, bool produces_midi) {
    PluginDescriptor desc;
    desc.name = "Lv2LayoutProbe";
    desc.manufacturer = "PulpTest";
    desc.bundle_id = "com.pulp.test.lv2-layout-probe";
    desc.version = "1.0.0";
    desc.category = PluginCategory::Effect;
    desc.input_buses = std::move(inputs);
    desc.output_buses = std::move(outputs);
    desc.accepts_midi = accepts_midi;
    desc.produces_midi = produces_midi;
    return desc;
}

state::ParamInfo make_layout_param(state::ParamID id, std::string name) {
    state::ParamInfo info;
    info.id = id;
    info.name = std::move(name);
    info.range = {0.0f, 1.0f, 0.5f};
    return info;
}

} // namespace

// The port number is the whole of LV2's port identity: generate_plugin_ttl()
// writes it into the manifest and connect_port() reads the same integer back
// from the host. This pins those two orderings against each other — a
// disagreement hands the plugin a float* where it reads an LV2_Atom_Sequence.
//
// It pins the generator, which is the specification of the ordering; it is not
// evidence that a host reads this Turtle today, because nothing in the build
// writes a .ttl into an .lv2 bundle. Pulp's own LV2 host is the one consumer
// that connects ports by manifest index.
TEST_CASE("LV2 TTL port indices match the slots connect_port wires", "[format][lv2][ports]") {
    struct Shape {
        const char* label;
        PluginDescriptor desc;
        std::vector<state::ParamInfo> params;
    };

    std::vector<Shape> shapes;
    shapes.push_back({"stereo effect with parameters",
                      make_layout_desc({{"In", 2}}, {{"Out", 2}}, false, false),
                      {make_layout_param(1, "Gain"), make_layout_param(2, "Mix")}});
    shapes.push_back({"instrument with MIDI input",
                      make_layout_desc({}, {{"Out", 2}}, true, false),
                      {make_layout_param(1, "Level")}});
    shapes.push_back(
        {"MIDI effect with no audio and no parameters", make_layout_desc({}, {}, true, true), {}});
    shapes.push_back({"multi-bus effect emitting MIDI",
                      make_layout_desc({{"Main In", 1}, {"Side In", 2}},
                                       {{"Main Out", 2}, {"Aux Out", 2}}, false, true),
                      {make_layout_param(1, "Depth")}});

    for (const auto& shape : shapes) {
        CAPTURE(shape.label);
        g_layout_probe_desc = shape.desc;
        g_layout_probe_params = shape.params;

        Lv2FactoryGuard factory(&make_lv2_layout_probe);
        Lv2FeatureBundle features;
        Lv2HandleGuard handle{lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "",
                                                       features.features)};
        REQUIRE(handle.handle != nullptr);
        auto* inst = static_cast<PulpLv2Instance*>(handle.handle);

        // Generate the manifest from exactly the inputs a shipped bundle would:
        // the instance's own processor descriptor and parameter store.
        const auto ttl = generate_plugin_ttl(inst->processor->descriptor(), inst->store,
                                             "http://pulp.audio/test/lv2-layout-probe");
        const auto ports = parse_ttl_ports(ttl);

        const auto layout = inst->port_layout();
        REQUIRE(ports.size() == static_cast<std::size_t>(layout.port_count()));

        // The lv2:port predicate opens exactly once and every later port is a
        // comma continuation of it. Which kind of port comes first depends on
        // the plugin's shape, so this is asserted for every shape rather than
        // for the audio-first one alone.
        std::size_t predicate_count = 0;
        for (std::size_t at = ttl.find("\n    lv2:port\n"); at != std::string::npos;
             at = ttl.find("\n    lv2:port\n", at + 1)) {
            ++predicate_count;
        }
        REQUIRE(predicate_count == 1);

        // Indices are dense and declared once each, in ascending order — the
        // property a host relies on when it saves connections by number.
        for (std::size_t i = 0; i < ports.size(); ++i) {
            REQUIRE(ports[i].index == static_cast<int>(i));
        }

        // Connect every declared port to a distinct sentinel address. Nothing
        // dereferences these; run() is never called here, so a float address
        // standing in for an atom buffer is only ever compared.
        std::vector<float> sentinels(ports.size(), 0.0f);
        for (const auto& port : ports) {
            lv2_generic::connect_port(handle.handle, static_cast<uint32_t>(port.index),
                                      &sentinels[static_cast<std::size_t>(port.index)]);
        }

        // Ordinals come from the manifest's declaration order, so the slot each
        // port must land in is derived from the TTL alone.
        int audio_in_seen = 0;
        int audio_out_seen = 0;
        int control_seen = 0;
        int latency_seen = 0;
        for (const auto& port : ports) {
            CAPTURE(port.index);
            void* expected = &sentinels[static_cast<std::size_t>(port.index)];
            switch (kind_declared_in_ttl(port)) {
            case Lv2PortKind::AudioIn:
                REQUIRE(inst->audio_in_ports[audio_in_seen++] == expected);
                break;
            case Lv2PortKind::AudioOut:
                REQUIRE(inst->audio_out_ports[audio_out_seen++] == expected);
                break;
            case Lv2PortKind::Control:
                REQUIRE(inst->control_in_ports[control_seen++] == expected);
                break;
            case Lv2PortKind::AtomIn:
                REQUIRE(inst->midi_in_atom == expected);
                break;
            case Lv2PortKind::AtomOut:
                REQUIRE(inst->midi_out_atom == expected);
                break;
            case Lv2PortKind::Latency:
                REQUIRE(inst->latency_port == expected);
                ++latency_seen;
                break;
            case Lv2PortKind::None:
                FAIL("port declares no recognized LV2 port type");
                break;
            }
        }

        // Every slot the adapter reads was filled by a port the manifest
        // declared — a manifest short of a port would leave one null.
        REQUIRE(audio_in_seen == inst->num_audio_inputs);
        REQUIRE(audio_out_seen == inst->num_audio_outputs);
        REQUIRE(control_seen == inst->num_params);
        REQUIRE(latency_seen == 1);
        REQUIRE((inst->midi_in_atom != nullptr) == inst->accepts_midi);
        REQUIRE((inst->midi_out_atom != nullptr) == inst->produces_midi);
    }
}

// The instance carries fixed kMaxChannels-wide audio port arrays and the
// manifest numbers a port for every declared channel, so a descriptor wider
// than that ceiling has no valid wiring. Refuse it at admission rather than
// dropping connections later, where the symptom would be silence.
TEST_CASE("LV2 instantiate refuses a descriptor past the channel ceiling", "[format][lv2][ports]") {
    Lv2FeatureBundle features;

    auto instantiate_with = [&](std::vector<BusInfo> inputs, std::vector<BusInfo> outputs) {
        g_layout_probe_desc = make_layout_desc(std::move(inputs), std::move(outputs), false, false);
        g_layout_probe_params = {};
        return lv2_generic::instantiate(&lv2_generic::g_lv2_descriptor, 48000.0, "",
                                        features.features);
    };

    Lv2FactoryGuard factory(&make_lv2_layout_probe);

    // One bus past the ceiling, and a sum over it across several buses.
    REQUIRE(instantiate_with({{"In", kMaxChannels + 1}}, {{"Out", 2}}) == nullptr);
    REQUIRE(instantiate_with({{"Main In", kMaxChannels}, {"Side In", 2}}, {{"Out", 2}}) == nullptr);
    REQUIRE(instantiate_with({{"In", 2}}, {{"Main Out", kMaxChannels}, {"Aux Out", 2}}) == nullptr);

    // Control: a descriptor exactly at the ceiling is still admitted, so the
    // refusals above are the guard and not a broken instantiate().
    Lv2HandleGuard widest{instantiate_with({{"In", kMaxChannels}}, {{"Out", kMaxChannels}})};
    REQUIRE(widest.handle != nullptr);
    auto* inst = static_cast<PulpLv2Instance*>(widest.handle);
    REQUIRE(inst->num_audio_inputs == kMaxChannels);
    REQUIRE(inst->num_audio_outputs == kMaxChannels);
}
