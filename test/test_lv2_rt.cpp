// LV2 run() render-path RT-safety guard (MF-2).
//
// The LV2 generic entry point (core/format/include/pulp/format/lv2_entry.hpp)
// used to construct unreserved stack-local midi::MidiBuffer objects every
// run() and parse MIDI into them outside its ScopedNoAlloc guard, so the first
// add() heap-allocated on the audio thread; it also lacked the flush-to-zero
// (denormal) guard every other adapter has. The fix pre-reserves instance-owned
// MIDI buffers (set_realtime_capacity_limit(true)), wraps the whole render in
// ScopedFlushDenormals, and spans MIDI parse + process() + MIDI serialize with
// ScopedNoAlloc.
//
// This test drives instantiate() -> connect_port() -> run() headlessly with a
// MIDI-in/MIDI-out processor (exercising the previously-allocating parse) and
// asserts the run() body neither allocates nor takes a blocking lock. Built
// with PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1 and linked against the trap TU
// (test/native_components/rt_intercept_test_support.cpp), ScopedRtProcessProbe
// ABORTS on an allocation / blocking lock in scope. See
// test/harness/scoped_rt_process_probe.hpp. It also verifies the new latency
// output control port receives the processor's reported latency each block.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/lv2_adapter.hpp>
#include <pulp/format/lv2_entry.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include "harness/scoped_rt_process_probe.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format;
using namespace pulp::format::lv2_adapter;

namespace {

// Reported plugin latency in samples; run() must write this to the latency port.
constexpr int kLv2RtLatencySamples = 64;
constexpr state::ParamID kLv2RtGainParam = 1;

// Minimal RT-safe MIDI effect: gains its input, echoes one MIDI note out, and
// reports a non-zero latency. Touches no heap and takes no lock in process().
class Lv2RtProcessor final : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor desc;
        desc.name = "Lv2RtProbe";
        desc.manufacturer = "PulpTest";
        desc.bundle_id = "com.pulp.test.lv2-rt";
        desc.version = "1.0.0";
        desc.category = PluginCategory::MidiEffect;
        desc.input_buses = {{"Audio In", 2}};
        desc.output_buses = {{"Audio Out", 2}};
        desc.accepts_midi = true;
        desc.produces_midi = true;
        return desc;
    }

    int latency_samples() const override { return kLv2RtLatencySamples; }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kLv2RtGainParam,
            .name = "Gain",
            .unit = "",
            .range = {0.0f, 2.0f, 1.0f, 0.01f},
        });
    }

    void prepare(const PrepareContext&) override {}
    void release() override {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext& context) override {
        const float gain = state().get_value(kLv2RtGainParam);
        const std::size_t channels = output.num_channels();
        const std::size_t samples = output.num_samples();
        for (std::size_t c = 0; c < channels; ++c) {
            float* dst = output.channel_ptr(c);
            const bool has_in = c < input.num_channels();
            const float* src = has_in ? input.channel_ptr(c) : nullptr;
            for (std::size_t n = 0; n < samples; ++n) {
                dst[n] = has_in ? src[n] * gain : 0.0f;
            }
        }
        // Echo every inbound note as an outbound note (into the pre-reserved,
        // capacity-limited adapter buffer — no allocation).
        for (const auto& ev : midi_in) {
            auto out = midi::MidiEvent::note_on(0, ev.size() > 1 ? ev.data()[1]
                                                                 : uint8_t{60},
                                                100);
            out.sample_offset = ev.sample_offset;
            midi_out.add(out);
        }
        (void)context;
    }

    void process(ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext& context) override {
        auto* out = audio.main_output();
        auto* in = audio.main_input();
        audio::BufferView<const float> empty_input;
        if (out) {
            process(*out, in ? *in : empty_input, midi_in, midi_out, context);
        }
    }
};

std::unique_ptr<Processor> make_lv2_rt_processor() {
    return std::make_unique<Lv2RtProcessor>();
}

// Stable-ID URID map, matching the pattern in test_lv2_adapter.cpp.
LV2_URID fake_map(LV2_URID_Map_Handle handle, const char* uri) {
    auto* table = static_cast<std::vector<std::string>*>(handle);
    for (std::size_t i = 0; i < table->size(); ++i) {
        if ((*table)[i] == uri) return static_cast<LV2_URID>(100 + i);
    }
    table->push_back(uri);
    return static_cast<LV2_URID>(100 + table->size() - 1);
}

struct Lv2FeatureBundle {
    std::vector<std::string> table;
    LV2_URID_Map map{&table, &fake_map};
    LV2_Feature map_feature{LV2_URID__map, &map};
    const LV2_Feature* features[2] = {&map_feature, nullptr};
};

// Host-style output Atom_Sequence buffer (atom.size carries body capacity).
struct LV2SequenceBuffer {
    std::array<uint8_t, 512> storage{};
    LV2_Atom_Sequence* as_seq() {
        return reinterpret_cast<LV2_Atom_Sequence*>(storage.data());
    }
    void prepare_as_output_port() {
        std::memset(storage.data(), 0, storage.size());
        as_seq()->atom.size =
            static_cast<uint32_t>(storage.size() - sizeof(LV2_Atom));
        as_seq()->atom.type = 0;
    }
};

uint32_t prepare_input_sequence(LV2SequenceBuffer& buf, LV2_URID atom_seq_urid) {
    buf.prepare_as_output_port();
    auto* seq = buf.as_seq();
    const uint32_t capacity = seq->atom.size;
    seq->atom.type = atom_seq_urid;
    seq->body.unit = 0;
    seq->body.pad = 0;
    lv2_atom_sequence_clear(seq);
    return capacity;
}

void append_midi_event(LV2_Atom_Sequence* seq, uint32_t capacity,
                       LV2_URID midi_event_urid, int64_t frame,
                       uint8_t status, uint8_t data1, uint8_t data2) {
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

struct Lv2FactoryGuard {
    ProcessorFactory previous_factory = lv2_generic::g_factory;
    const char* previous_uri = lv2_generic::g_uri;
    const char* previous_descriptor_uri = lv2_generic::g_lv2_descriptor.URI;

    explicit Lv2FactoryGuard(ProcessorFactory factory) {
        lv2_generic::g_factory = factory;
        lv2_generic::g_uri = "http://pulp.audio/test/lv2-rt";
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

}  // namespace

TEST_CASE("LV2 run() is allocation/lock-free and reports latency",
          "[format][lv2][rt-safety][issue-mf2]") {
    Lv2FactoryGuard factory(&make_lv2_rt_processor);
    Lv2FeatureBundle features;

    Lv2HandleGuard handle{lv2_generic::instantiate(
        &lv2_generic::g_lv2_descriptor, 48000.0, "", features.features)};
    REQUIRE(handle.handle != nullptr);

    auto* inst = static_cast<PulpLv2Instance*>(handle.handle);
    REQUIRE(inst->num_audio_inputs == 2);
    REQUIRE(inst->num_audio_outputs == 2);
    REQUIRE(inst->num_params == 1);
    REQUIRE(inst->accepts_midi);
    REQUIRE(inst->produces_midi);

    constexpr int kFrames = 256;
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    for (int n = 0; n < kFrames; ++n) {
        in_l[static_cast<std::size_t>(n)] = 0.1f;
        in_r[static_cast<std::size_t>(n)] = -0.2f;
    }
    float gain = 1.0f;
    float latency_readback = -1.0f;

    // MIDI input sequence with a couple of notes so the parse path runs.
    LV2SequenceBuffer midi_in;
    const uint32_t midi_in_cap =
        prepare_input_sequence(midi_in, inst->urid_atom_sequence);
    append_midi_event(midi_in.as_seq(), midi_in_cap, inst->urid_midi_event,
                      0, 0x90, 60, 100);
    append_midi_event(midi_in.as_seq(), midi_in_cap, inst->urid_midi_event,
                      64, 0x90, 64, 100);

    LV2SequenceBuffer midi_out;
    midi_out.prepare_as_output_port();

    // Port layout: 2 audio in (0,1), 2 audio out (2,3), 1 control (4),
    // MIDI in atom (5), MIDI out atom (6), latency (7).
    lv2_generic::connect_port(handle.handle, 0, in_l.data());
    lv2_generic::connect_port(handle.handle, 1, in_r.data());
    lv2_generic::connect_port(handle.handle, 2, out_l.data());
    lv2_generic::connect_port(handle.handle, 3, out_r.data());
    lv2_generic::connect_port(handle.handle, 4, &gain);
    lv2_generic::connect_port(handle.handle, 5, midi_in.as_seq());
    lv2_generic::connect_port(handle.handle, 6, midi_out.as_seq());
    lv2_generic::connect_port(handle.handle, 7, &latency_readback);
    lv2_generic::activate(handle.handle);

    // Warm-up: absorb any one-time steady-state priming (store SPSC init, etc.)
    // outside the probe, as a host's first callbacks would.
    lv2_generic::run(handle.handle, kFrames);
    lv2_generic::run(handle.handle, kFrames);

    std::size_t allocation_count = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        lv2_generic::run(handle.handle, kFrames);
        allocation_count = probe.allocation_count();
    }
    REQUIRE(allocation_count == 0);

    // The processor ran (gain default 1.0 copies input through).
    REQUIRE(out_l[0] == 0.1f);
    REQUIRE(out_r[0] == -0.2f);

    // The latency port received the processor's reported latency.
    REQUIRE(latency_readback == static_cast<float>(kLv2RtLatencySamples));

    // Outbound MIDI was serialized (echoed notes), proving the guarded
    // serialize path ran without allocating.
    int out_events = 0;
    LV2_ATOM_SEQUENCE_FOREACH(midi_out.as_seq(), ev) {
        (void)ev;
        ++out_events;
    }
    REQUIRE(out_events == 2);
}
