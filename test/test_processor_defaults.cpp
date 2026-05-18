// Processor and descriptor default-contract coverage.
//
// These are pure format-layer unit tests: no plugin SDK, audio device, UI
// host, or external service required. The cases pin the small defaults that
// format adapters rely on when a plugin author does not override a hook.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/ara.hpp>
#include <pulp/format/processor.hpp>

using namespace pulp::format;

namespace {

class PlainProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override { return descriptor_; }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext& context) override {
        last_prepare = context;
        ++prepare_calls;
    }
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext& context) override {
        last_process = context;
        ++process_calls;
    }

    PluginDescriptor descriptor_;
    PrepareContext last_prepare{};
    ProcessContext last_process{};
    int prepare_calls = 0;
    int process_calls = 0;
};

class CustomEditorSizeProcessor : public PlainProcessor {
public:
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {1024, 640};
    }
};

class CustomViewSizeProcessor : public PlainProcessor {
public:
    ViewSize view_size() const override {
        return {
            .preferred_width = 960,
            .preferred_height = 540,
            .min_width = 480,
            .min_height = 270,
            .max_width = 1920,
            .max_height = 1080,
            .aspect_ratio = 16.0 / 9.0,
        };
    }
};

class EditorlessProcessor : public PlainProcessor {
public:
    bool has_editor() const override { return false; }
};

} // namespace

TEST_CASE("PluginDescriptor defaults to conservative stereo effect metadata",
          "[format][processor-defaults]") {
    PluginDescriptor d;

    REQUIRE(d.category == PluginCategory::Effect);
    REQUIRE(d.input_buses.size() == 1);
    REQUIRE(d.output_buses.size() == 1);
    REQUIRE(d.input_buses[0].name == "Main In");
    REQUIRE(d.output_buses[0].name == "Main Out");
    REQUIRE(d.default_input_channels() == 2);
    REQUIRE(d.default_output_channels() == 2);
    REQUIRE_FALSE(d.input_buses[0].optional);
    REQUIRE_FALSE(d.output_buses[0].optional);
    REQUIRE_FALSE(d.accepts_midi);
    REQUIRE_FALSE(d.produces_midi);
    REQUIRE_FALSE(d.supports_mpe);
    REQUIRE_FALSE(d.supports_ump);
    REQUIRE_FALSE(d.ios_requires_background_audio);
    REQUIRE(d.tail_samples == 0);
}

TEST_CASE("PluginDescriptor bus helpers return zero when main buses are absent",
          "[format][processor-defaults][buses]") {
    PluginDescriptor d;
    d.input_buses.clear();
    d.output_buses.clear();

    REQUIRE(d.default_input_channels() == 0);
    REQUIRE(d.default_output_channels() == 0);
}

TEST_CASE("PluginDescriptor bus helpers read only the first bus",
          "[format][processor-defaults][buses]") {
    PluginDescriptor d;
    d.input_buses = {
        {"Main In", 1, false},
        {"Sidechain", 2, true},
        {"Aux In", 8, true},
    };
    d.output_buses = {
        {"Main Out", 6, false},
        {"Monitor", 2, true},
    };

    REQUIRE(d.default_input_channels() == 1);
    REQUIRE(d.default_output_channels() == 6);
    REQUIRE(d.input_buses[1].optional);
    REQUIRE(d.output_buses[1].optional);
}

TEST_CASE("PluginDescriptor carries MIDI, MPE, UMP, and mobile flags independently",
          "[format][processor-defaults][flags]") {
    PluginDescriptor d;
    d.category = PluginCategory::Instrument;
    d.accepts_midi = true;
    d.produces_midi = true;
    d.supports_mpe = true;
    d.supports_ump = true;
    d.ios_requires_background_audio = true;
    d.tail_samples = -1;

    REQUIRE(d.category == PluginCategory::Instrument);
    REQUIRE(d.accepts_midi);
    REQUIRE(d.produces_midi);
    REQUIRE(d.supports_mpe);
    REQUIRE(d.supports_ump);
    REQUIRE(d.ios_requires_background_audio);
    REQUIRE(d.tail_samples == -1);
}

TEST_CASE("PrepareContext defaults match the headless stereo render path",
          "[format][processor-defaults][context]") {
    PrepareContext c;
    REQUIRE(c.sample_rate == 48000.0);
    REQUIRE(c.max_buffer_size == 512);
    REQUIRE(c.input_channels == 2);
    REQUIRE(c.output_channels == 2);
}

TEST_CASE("ProcessContext defaults represent stopped 4/4 playback at 120 BPM",
          "[format][processor-defaults][context]") {
    ProcessContext c;
    REQUIRE(c.sample_rate == 0.0);
    REQUIRE(c.num_samples == 0);
    REQUIRE_FALSE(c.is_playing);
    REQUIRE_FALSE(c.is_recording);
    REQUIRE(c.tempo_bpm == 120.0);
    REQUIRE(c.position_beats == 0.0);
    REQUIRE(c.position_samples == 0);
    REQUIRE(c.time_sig_numerator == 4);
    REQUIRE(c.time_sig_denominator == 4);
}

TEST_CASE("Processor default editor contract is auto UI with free-resize hints",
          "[format][processor-defaults][editor]") {
    PlainProcessor p;

    REQUIRE(p.has_editor());
    REQUIRE(p.editor_size() == std::pair<uint32_t, uint32_t>{400, 300});

    const auto hints = p.view_size();
    REQUIRE(hints.preferred_width == 400);
    REQUIRE(hints.preferred_height == 300);
    REQUIRE(hints.min_width == 0);
    REQUIRE(hints.min_height == 0);
    REQUIRE(hints.max_width == 0);
    REQUIRE(hints.max_height == 0);
    REQUIRE(hints.aspect_ratio == 0.0);
    REQUIRE(p.create_view() == nullptr);
}

TEST_CASE("Processor default view_size reflects an overridden editor_size",
          "[format][processor-defaults][editor]") {
    CustomEditorSizeProcessor p;
    const auto hints = p.view_size();

    REQUIRE(hints.preferred_width == 1024);
    REQUIRE(hints.preferred_height == 640);
    REQUIRE(hints.min_width == 0);
    REQUIRE(hints.max_width == 0);
    REQUIRE(hints.aspect_ratio == 0.0);
}

TEST_CASE("Processor custom view_size can provide resize bounds and aspect ratio",
          "[format][processor-defaults][editor][coverage][phase3]") {
    CustomViewSizeProcessor p;
    const auto hints = p.view_size();

    REQUIRE(hints.preferred_width == 960);
    REQUIRE(hints.preferred_height == 540);
    REQUIRE(hints.min_width == 480);
    REQUIRE(hints.min_height == 270);
    REQUIRE(hints.max_width == 1920);
    REQUIRE(hints.max_height == 1080);
    REQUIRE(hints.aspect_ratio == 16.0 / 9.0);
}

TEST_CASE("Processor has_editor override can disable default editor contract",
          "[format][processor-defaults][editor][coverage][phase3]") {
    EditorlessProcessor p;

    REQUIRE_FALSE(p.has_editor());
    REQUIRE(p.editor_size() == std::pair<uint32_t, uint32_t>{400, 300});
    REQUIRE(p.view_size().preferred_width == 400);
    REQUIRE(p.create_view() == nullptr);
}

TEST_CASE("Processor default latency and plugin-owned state hooks are inert",
          "[format][processor-defaults][state]") {
    PlainProcessor p;

    REQUIRE(p.latency_samples() == 0);
    REQUIRE(p.serialize_plugin_state().empty());

    const std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    REQUIRE(p.deserialize_plugin_state({}));
    REQUIRE(p.deserialize_plugin_state(payload));
}

TEST_CASE("Processor default lifecycle hooks are callable no-ops",
          "[format][processor-defaults][lifecycle][coverage][phase3]") {
    PlainProcessor p;
    pulp::view::View view;

    p.release();
    p.on_memory_pressure(Processor::MemoryPressure::Advisory);
    p.on_memory_pressure(Processor::MemoryPressure::Critical);
    p.on_view_opened(view);
    p.on_view_resized(view, 320, 200);
    p.on_view_closed(view);
    p.on_host_transport_changed(true, 12.5);
    p.on_host_transport_changed(false, 0.0);
    p.on_host_tempo_changed(97.5);

    REQUIRE(p.create_ara_document_controller() == nullptr);
}

TEST_CASE("Processor prepare receives the exact host context",
          "[format][processor-defaults][context][coverage][phase3]") {
    PlainProcessor p;
    PrepareContext context;
    context.sample_rate = 96000.0;
    context.max_buffer_size = 2048;
    context.input_channels = 1;
    context.output_channels = 6;

    p.prepare(context);

    REQUIRE(p.prepare_calls == 1);
    REQUIRE(p.last_prepare.sample_rate == 96000.0);
    REQUIRE(p.last_prepare.max_buffer_size == 2048);
    REQUIRE(p.last_prepare.input_channels == 1);
    REQUIRE(p.last_prepare.output_channels == 6);
}

TEST_CASE("Processor process receives the exact transport context",
          "[format][processor-defaults][context][coverage][phase3]") {
    PlainProcessor p;
    float out_samples[4] = {};
    float* out_channels[1] = {out_samples};
    const float in_samples[4] = {1.0f, 0.5f, -0.5f, -1.0f};
    const float* in_channels[1] = {in_samples};
    pulp::audio::BufferView<float> output(out_channels, 1, 4);
    pulp::audio::BufferView<const float> input(in_channels, 1, 4);
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;

    ProcessContext context;
    context.sample_rate = 44100.0;
    context.num_samples = 4;
    context.is_playing = true;
    context.is_recording = true;
    context.tempo_bpm = 137.25;
    context.position_beats = 33.5;
    context.position_samples = 123456;
    context.time_sig_numerator = 7;
    context.time_sig_denominator = 8;

    p.process(output, input, midi_in, midi_out, context);

    REQUIRE(p.process_calls == 1);
    REQUIRE(p.last_process.sample_rate == 44100.0);
    REQUIRE(p.last_process.num_samples == 4);
    REQUIRE(p.last_process.is_playing);
    REQUIRE(p.last_process.is_recording);
    REQUIRE(p.last_process.tempo_bpm == 137.25);
    REQUIRE(p.last_process.position_beats == 33.5);
    REQUIRE(p.last_process.position_samples == 123456);
    REQUIRE(p.last_process.time_sig_numerator == 7);
    REQUIRE(p.last_process.time_sig_denominator == 8);
}

TEST_CASE("Processor state-store pointer wiring exposes the exact store",
          "[format][processor-defaults][state]") {
    PlainProcessor p;
    pulp::state::StateStore store;

    p.set_state_store(&store);

    REQUIRE(&p.state() == &store);
    const PlainProcessor& cp = p;
    REQUIRE(&cp.state() == &store);
}

TEST_CASE("Processor sidechain, MPE, and UMP sidecar pointers can be set and cleared",
          "[format][processor-defaults][sidecars]") {
    PlainProcessor p;
    REQUIRE(p.sidechain_input() == nullptr);
    REQUIRE(p.mpe_input() == nullptr);
    REQUIRE(p.ump_input() == nullptr);

    const float sidechain_samples[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    const float* sidechain_channels[1] = {sidechain_samples};
    pulp::audio::BufferView<const float> sidechain(sidechain_channels, 1, 4);
    pulp::midi::MpeBuffer mpe;
    pulp::midi::UmpBuffer ump;

    p.set_sidechain(&sidechain);
    p.set_mpe_input(&mpe);
    p.set_ump_input(&ump);

    REQUIRE(p.sidechain_input() == &sidechain);
    REQUIRE(p.mpe_input() == &mpe);
    REQUIRE(p.ump_input() == &ump);

    p.set_sidechain(nullptr);
    p.set_mpe_input(nullptr);
    p.set_ump_input(nullptr);

    REQUIRE(p.sidechain_input() == nullptr);
    REQUIRE(p.mpe_input() == nullptr);
    REQUIRE(p.ump_input() == nullptr);
}

TEST_CASE("Processor sidecar pointers replace independently between blocks",
          "[format][processor-defaults][sidecars][coverage]") {
    PlainProcessor p;

    const float first_samples[2] = {0.1f, 0.2f};
    const float second_samples[2] = {0.3f, 0.4f};
    const float* first_channels[1] = {first_samples};
    const float* second_channels[1] = {second_samples};
    pulp::audio::BufferView<const float> first_sidechain(first_channels, 1, 2);
    pulp::audio::BufferView<const float> second_sidechain(second_channels, 1, 2);
    pulp::midi::MpeBuffer first_mpe;
    pulp::midi::MpeBuffer second_mpe;
    pulp::midi::UmpBuffer first_ump;
    pulp::midi::UmpBuffer second_ump;

    p.set_sidechain(&first_sidechain);
    p.set_mpe_input(&first_mpe);
    p.set_ump_input(&first_ump);
    REQUIRE(p.sidechain_input() == &first_sidechain);
    REQUIRE(p.mpe_input() == &first_mpe);
    REQUIRE(p.ump_input() == &first_ump);

    p.set_sidechain(&second_sidechain);
    p.set_mpe_input(&second_mpe);
    p.set_ump_input(&second_ump);
    REQUIRE(p.sidechain_input() == &second_sidechain);
    REQUIRE(p.mpe_input() == &second_mpe);
    REQUIRE(p.ump_input() == &second_ump);

    p.set_mpe_input(nullptr);
    REQUIRE(p.sidechain_input() == &second_sidechain);
    REQUIRE(p.mpe_input() == nullptr);
    REQUIRE(p.ump_input() == &second_ump);
}
