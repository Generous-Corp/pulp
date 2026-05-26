// Processor and descriptor default-contract coverage.
//
// These are pure format-layer unit tests: no plugin SDK, audio device, UI
// host, or external service required. The cases pin the small defaults that
// format adapters rely on when a plugin author does not override a hook.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/ara.hpp>
#include <pulp/format/param_processing.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>

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
    REQUIRE_FALSE(d.node_capabilities.supports_mpe);
    REQUIRE_FALSE(d.node_capabilities.supports_ump);
    REQUIRE_FALSE(d.effective_capabilities().supports_mpe);
    REQUIRE_FALSE(d.effective_capabilities().supports_ump);
    REQUIRE_FALSE(d.ios_requires_background_audio);
    REQUIRE(d.tail_samples == 0);
}

TEST_CASE("Node ABI version is visible through Processor surface",
          "[format][processor-defaults][node-abi]") {
    REQUIRE(pulp::PULP_NODE_ABI_VERSION == 1);
    REQUIRE(pulp::pulp_node_abi_version() == pulp::PULP_NODE_ABI_VERSION);
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

TEST_CASE("PluginDescriptor effective_capabilities ORs legacy and node fields",
          "[format][processor-defaults][node-abi]") {
    PluginDescriptor d;

    SECTION("legacy MPE plus node UMP") {
        d.supports_mpe = true;
        d.node_capabilities.supports_ump = true;

        const auto caps = d.effective_capabilities();
        REQUIRE(caps.supports_mpe);
        REQUIRE(caps.supports_ump);
    }

    SECTION("node MPE plus legacy UMP") {
        d.node_capabilities.supports_mpe = true;
        d.supports_ump = true;

        const auto caps = d.effective_capabilities();
        REQUIRE(caps.supports_mpe);
        REQUIRE(caps.supports_ump);
    }
}

TEST_CASE("PluginDescriptor preserves optional vendor contact metadata",
          "[format][processor-defaults][metadata][coverage][phase3]") {
    PluginDescriptor d;
    d.vendor_url = "https://example.test/pulp";
    d.vendor_email = "support@example.test";

    REQUIRE(d.vendor_url == "https://example.test/pulp");
    REQUIRE(d.vendor_email == "support@example.test");
    REQUIRE(d.default_input_channels() == 2);
    REQUIRE(d.default_output_channels() == 2);
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

    // Item 1.3 extensions — every new field defaults to the documented
    // "host did not provide" sentinel so existing adapters that don't
    // populate them keep their pre-extension behaviour.
    REQUIRE(c.bar == 0);
    REQUIRE_FALSE(c.is_looping);
    REQUIRE(c.loop_start_beats == 0.0);
    REQUIRE(c.loop_end_beats == 0.0);
    REQUIRE(c.host_time_ns == 0);
    REQUIRE(c.frame_rate == FrameRate::unknown);
    REQUIRE_FALSE(c.tempo_changed);
    REQUIRE_FALSE(c.time_sig_changed);
    REQUIRE_FALSE(c.transport_changed);
}

TEST_CASE("ProcessContext carries item 1.3 playhead extensions independently",
          "[format][processor-defaults][context][issue-1003][playhead]") {
    ProcessContext c;

    c.is_looping = true;
    c.loop_start_beats = 16.0;
    c.loop_end_beats = 32.5;
    c.bar = 7;
    c.host_time_ns = 1'234'567'890LL;
    c.frame_rate = FrameRate::fps_29_97_drop;
    c.tempo_changed = true;
    c.time_sig_changed = true;
    c.transport_changed = true;

    REQUIRE(c.is_looping);
    REQUIRE(c.loop_start_beats == 16.0);
    REQUIRE(c.loop_end_beats == 32.5);
    REQUIRE(c.bar == 7);
    REQUIRE(c.host_time_ns == 1'234'567'890LL);
    REQUIRE(c.frame_rate == FrameRate::fps_29_97_drop);
    REQUIRE(c.tempo_changed);
    REQUIRE(c.time_sig_changed);
    REQUIRE(c.transport_changed);

    // Defaulted legacy fields should not be disturbed when only the
    // 1.3 extensions are touched.
    REQUIRE(c.tempo_bpm == 120.0);
    REQUIRE(c.time_sig_numerator == 4);
    REQUIRE(c.time_sig_denominator == 4);
    REQUIRE_FALSE(c.is_playing);
    REQUIRE_FALSE(c.is_recording);
}

TEST_CASE("FrameRate enum exposes the SMPTE rates adapters must surface",
          "[format][processor-defaults][context][playhead][frame-rate]") {
    // Adapters map host frame-rate values onto this enum. The exact
    // enumerators below are the contract; reordering them would silently
    // break per-block transport push from VST3/AU/AAX.
    REQUIRE(static_cast<int>(FrameRate::unknown) == 0);
    REQUIRE(FrameRate::fps_24 != FrameRate::fps_25);
    REQUIRE(FrameRate::fps_29_97 != FrameRate::fps_29_97_drop);
    REQUIRE(FrameRate::fps_30 != FrameRate::fps_30_drop);
    REQUIRE(FrameRate::fps_60 != FrameRate::fps_30);
    REQUIRE(FrameRate::unknown != FrameRate::fps_24);
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

TEST_CASE("view_size_from_design derives sensible bounds + aspect from preferred",
          "[format][processor-defaults][editor][design-import][issue-2784]") {
    // This is the helper that Processor::view_size()'s default calls when
    // pulp_add_plugin(... DESIGN_WIDTH N DESIGN_HEIGHT N) is set.
    SECTION("auto-derived bounds (no explicit min/max)") {
        const auto hints = pulp::format::view_size_from_design(900, 520);
        REQUIRE(hints.preferred_width == 900);
        REQUIRE(hints.preferred_height == 520);
        // CLAP's gui_can_resize requires min > 0 — the derived 2/3 floor is
        // exactly what saves plugin authors from per-format min overrides.
        REQUIRE(hints.min_width == 600);
        REQUIRE(hints.min_height == 346);
        REQUIRE(hints.max_width == 1800);
        REQUIRE(hints.max_height == 1040);
        REQUIRE(hints.aspect_ratio == 900.0 / 520.0);
    }

    SECTION("explicit non-zero min/max overrides the derivation") {
        const auto hints =
            pulp::format::view_size_from_design(900, 520, 700, 400, 1920, 1080);
        REQUIRE(hints.min_width == 700);
        REQUIRE(hints.min_height == 400);
        REQUIRE(hints.max_width == 1920);
        REQUIRE(hints.max_height == 1080);
        // aspect_ratio always tracks preferred — the explicit max doesn't
        // change what ratio the host snaps the corner-drag to.
        REQUIRE(hints.aspect_ratio == 900.0 / 520.0);
    }

    SECTION("zero preferred_height degrades safely (aspect = 0, no /0)") {
        const auto hints = pulp::format::view_size_from_design(900, 0);
        REQUIRE(hints.preferred_height == 0);
        REQUIRE(hints.aspect_ratio == 0.0);
    }
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
    // Item 1.3 extensions also round-trip through process() so adapters
    // that populate the new fields land them at the plugin verbatim.
    context.bar = 4;
    context.is_looping = true;
    context.loop_start_beats = 8.0;
    context.loop_end_beats = 24.0;
    context.host_time_ns = 9'876'543'210LL;
    context.frame_rate = FrameRate::fps_24;
    context.tempo_changed = true;
    context.transport_changed = true;

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
    REQUIRE(p.last_process.bar == 4);
    REQUIRE(p.last_process.is_looping);
    REQUIRE(p.last_process.loop_start_beats == 8.0);
    REQUIRE(p.last_process.loop_end_beats == 24.0);
    REQUIRE(p.last_process.host_time_ns == 9'876'543'210LL);
    REQUIRE(p.last_process.frame_rate == FrameRate::fps_24);
    REQUIRE(p.last_process.tempo_changed);
    REQUIRE_FALSE(p.last_process.time_sig_changed);
    REQUIRE(p.last_process.transport_changed);
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
    REQUIRE(p.param_events() == nullptr);

    const float sidechain_samples[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    const float* sidechain_channels[1] = {sidechain_samples};
    pulp::audio::BufferView<const float> sidechain(sidechain_channels, 1, 4);
    pulp::midi::MpeBuffer mpe;
    pulp::midi::UmpBuffer ump;
    pulp::state::ParameterEventQueue param_events;

    p.set_sidechain(&sidechain);
    p.set_mpe_input(&mpe);
    p.set_ump_input(&ump);
    p.set_param_events(&param_events);

    REQUIRE(p.sidechain_input() == &sidechain);
    REQUIRE(p.mpe_input() == &mpe);
    REQUIRE(p.ump_input() == &ump);
    REQUIRE(p.param_events() == &param_events);

    p.set_sidechain(nullptr);
    p.set_mpe_input(nullptr);
    p.set_ump_input(nullptr);
    p.set_param_events(nullptr);

    REQUIRE(p.sidechain_input() == nullptr);
    REQUIRE(p.mpe_input() == nullptr);
    REQUIRE(p.ump_input() == nullptr);
    REQUIRE(p.param_events() == nullptr);
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
    pulp::state::ParameterEventQueue first_param_events;
    pulp::state::ParameterEventQueue second_param_events;

    p.set_sidechain(&first_sidechain);
    p.set_mpe_input(&first_mpe);
    p.set_ump_input(&first_ump);
    p.set_param_events(&first_param_events);
    REQUIRE(p.sidechain_input() == &first_sidechain);
    REQUIRE(p.mpe_input() == &first_mpe);
    REQUIRE(p.ump_input() == &first_ump);
    REQUIRE(p.param_events() == &first_param_events);

    p.set_sidechain(&second_sidechain);
    p.set_mpe_input(&second_mpe);
    p.set_ump_input(&second_ump);
    p.set_param_events(&second_param_events);
    REQUIRE(p.sidechain_input() == &second_sidechain);
    REQUIRE(p.mpe_input() == &second_mpe);
    REQUIRE(p.ump_input() == &second_ump);
    REQUIRE(p.param_events() == &second_param_events);

    p.set_mpe_input(nullptr);
    p.set_param_events(nullptr);
    REQUIRE(p.sidechain_input() == &second_sidechain);
    REQUIRE(p.mpe_input() == nullptr);
    REQUIRE(p.ump_input() == &second_ump);
    REQUIRE(p.param_events() == nullptr);
}

TEST_CASE("ParamCursor uses explicit snapshots without mutating StateStore",
          "[format][params][subblock][rt]") {
    pulp::state::StateStore store;
    store.add_parameter({
        .id = 1,
        .name = "Gain",
        .range = {0.0f, 1.0f, 0.25f, 0.0f},
    });
    store.set_value(1, 0.95f);

    pulp::state::ParameterEventQueue events;
    REQUIRE(events.push({1, 0, 0.10f}));
    REQUIRE(events.push({1, 256, 0.50f}));
    REQUIRE(events.push({1, 256, 0.75f}));
    REQUIRE(events.push({1, 512, 0.90f}));
    events.sort();

    std::array<pulp::state::ParamSnapshotEntry, 1> initial{{
        {1, 0.25f},
    }};

    float in_samples[512] = {};
    float out_samples[512] = {};
    const float* in_channels[1] = {in_samples};
    float* out_channels[1] = {out_samples};
    pulp::audio::BufferView<const float> input(in_channels, 1, 512);
    pulp::audio::BufferView<float> output(out_channels, 1, 512);

    std::array<std::size_t, 4> starts{};
    std::array<std::size_t, 4> lengths{};
    std::array<float, 4> values{};
    int calls = 0;

    {
        pulp::runtime::ScopedNoAlloc guard;
        pulp::state::ParamCursor cursor(store, &events, initial);
        pulp::format::for_each_subblock(
            output, input, &events, cursor,
            [&](pulp::audio::BufferView<float>& out,
                const pulp::audio::BufferView<const float>&,
                const pulp::state::ParamCursor& params) {
                starts[static_cast<std::size_t>(calls)] =
                    static_cast<std::size_t>(out.channel_ptr(0) - out_samples);
                lengths[static_cast<std::size_t>(calls)] = out.num_samples();
                values[static_cast<std::size_t>(calls)] = params.value(1);
                ++calls;
            });
    }

    REQUIRE(calls == 2);
    REQUIRE(starts[0] == 0);
    REQUIRE(lengths[0] == 256);
    REQUIRE(values[0] == 0.10f);
    REQUIRE(starts[1] == 256);
    REQUIRE(lengths[1] == 256);
    REQUIRE(values[1] == 0.75f);
    REQUIRE(store.get_value(1) == 0.95f);
}

TEST_CASE("for_each_subblock invokes one full block for null or empty queues",
          "[format][params][subblock]") {
    pulp::state::StateStore store;
    store.add_parameter({
        .id = 7,
        .name = "Mix",
        .range = {0.0f, 1.0f, 0.5f, 0.0f},
    });

    float out_samples[8] = {};
    float* out_channels[1] = {out_samples};
    pulp::audio::BufferView<float> output(out_channels, 1, 8);
    pulp::audio::BufferView<const float> input;

    int calls = 0;
    pulp::format::for_each_subblock(
        output, input, store, nullptr,
        [&](pulp::audio::BufferView<float>& out,
            const pulp::audio::BufferView<const float>& in,
            const pulp::state::ParamCursor& params) {
            ++calls;
            REQUIRE(out.num_samples() == 8);
            REQUIRE(in.num_samples() == 0);
            REQUIRE(params.value(7) == 0.5f);
        });

    REQUIRE(calls == 1);

    pulp::state::ParameterEventQueue empty;
    calls = 0;
    pulp::format::for_each_subblock(
        output, input, store, &empty,
        [&](pulp::audio::BufferView<float>& out,
            const pulp::audio::BufferView<const float>&,
            const pulp::state::ParamCursor&) {
            ++calls;
            REQUIRE(out.num_samples() == 8);
        });
    REQUIRE(calls == 1);
}

TEST_CASE("for_each_subblock skips zero-sample buffers",
          "[format][params][subblock][coverage][phase3]") {
    pulp::state::StateStore store;
    store.add_parameter({
        .id = 3,
        .name = "Level",
        .range = {0.0f, 1.0f, 0.25f, 0.0f},
    });

    float out_sample = 0.0f;
    float in_sample = 0.0f;
    float* out_channels[1] = {&out_sample};
    const float* in_channels[1] = {&in_sample};
    pulp::audio::BufferView<float> output(out_channels, 1, 0);
    pulp::audio::BufferView<const float> input(in_channels, 1, 0);

    pulp::state::ParameterEventQueue events;
    REQUIRE(events.push({3, 0, 0.75f}));

    int calls = 0;
    pulp::format::for_each_subblock(
        output, input, store, &events,
        [&](pulp::audio::BufferView<float>&,
            const pulp::audio::BufferView<const float>&,
            const pulp::state::ParamCursor&) {
            ++calls;
        });

    REQUIRE(calls == 0);
}

TEST_CASE("for_each_subblock ignores boundary events and slices input with output",
          "[format][params][subblock][coverage][phase3]") {
    pulp::state::StateStore store;
    store.add_parameter({
        .id = 9,
        .name = "Tone",
        .range = {0.0f, 1.0f, 0.1f, 0.0f},
    });

    pulp::state::ParameterEventQueue events;
    REQUIRE(events.push({9, 8, 0.8f}));
    REQUIRE(events.push({9, -2, 0.2f}));
    REQUIRE(events.push({9, 3, 0.6f}));
    REQUIRE(events.push({9, 0, 0.4f}));
    events.sort();

    float in_samples[8] = {};
    float out_samples[8] = {};
    const float* in_channels[1] = {in_samples};
    float* out_channels[1] = {out_samples};
    pulp::audio::BufferView<const float> input(in_channels, 1, 8);
    pulp::audio::BufferView<float> output(out_channels, 1, 8);

    std::array<std::size_t, 2> starts{};
    std::array<std::size_t, 2> lengths{};
    std::array<std::size_t, 2> input_starts{};
    std::array<float, 2> values{};
    int calls = 0;

    pulp::format::for_each_subblock(
        output, input, store, &events,
        [&](pulp::audio::BufferView<float>& out,
            const pulp::audio::BufferView<const float>& in,
            const pulp::state::ParamCursor& params) {
            REQUIRE(calls < 2);
            starts[static_cast<std::size_t>(calls)] =
                static_cast<std::size_t>(out.channel_ptr(0) - out_samples);
            input_starts[static_cast<std::size_t>(calls)] =
                static_cast<std::size_t>(in.channel_ptr(0) - in_samples);
            lengths[static_cast<std::size_t>(calls)] = out.num_samples();
            values[static_cast<std::size_t>(calls)] = params.value(9);
            ++calls;
        });

    REQUIRE(calls == 2);
    REQUIRE(starts[0] == 0);
    REQUIRE(input_starts[0] == 0);
    REQUIRE(lengths[0] == 3);
    REQUIRE(values[0] == 0.4f);
    REQUIRE(starts[1] == 3);
    REQUIRE(input_starts[1] == 3);
    REQUIRE(lengths[1] == 5);
    REQUIRE(values[1] == 0.6f);
}

TEST_CASE("ControlRateParamSmoother is bit-exact off and ramps when opted in",
          "[format][params][smoothing]") {
    pulp::state::ParamInfo info;
    pulp::format::ControlRateParamSmoother smoother;

    smoother.prepare(info, 1000.0, 0.0f);
    REQUIRE_FALSE(smoother.smoothing_enabled());
    smoother.set_target(1.0f);
    for (int i = 0; i < 8; ++i) {
        REQUIRE(smoother.next() == 1.0f);
    }

    info.smoothing_ramp_seconds = 0.004f;
    smoother.prepare(info, 1000.0, 0.0f);
    REQUIRE(smoother.smoothing_enabled());
    smoother.set_target(1.0f);

    REQUIRE(smoother.next() == 0.25f);
    REQUIRE(smoother.next() == 0.50f);
    REQUIRE(smoother.next() == 0.75f);
    REQUIRE(smoother.next() == 1.00f);
    REQUIRE_FALSE(smoother.is_smoothing());
    REQUIRE(smoother.next() == 1.00f);
}

TEST_CASE("ControlRateParamSmoother handles invalid rates reset and skip",
          "[format][params][smoothing][coverage][phase3]") {
    pulp::state::ParamInfo info;
    info.smoothing_ramp_seconds = 0.004f;

    pulp::format::ControlRateParamSmoother smoother;
    smoother.prepare(info, 0.0, 0.25f);
    REQUIRE_FALSE(smoother.smoothing_enabled());
    REQUIRE(smoother.ramp_seconds() == 0.004f);
    smoother.set_target(0.75f);
    smoother.skip(8);
    REQUIRE(smoother.current() == 0.75f);
    REQUIRE(smoother.next() == 0.75f);

    smoother.prepare(info, 1000.0, 0.0f);
    REQUIRE(smoother.smoothing_enabled());
    smoother.set_target(1.0f);
    smoother.skip(2);
    REQUIRE(smoother.current() == 0.5f);
    REQUIRE(smoother.target() == 1.0f);
    REQUIRE(smoother.is_smoothing());

    smoother.reset(0.25f);
    REQUIRE_FALSE(smoother.is_smoothing());
    REQUIRE(smoother.current() == 0.25f);
    REQUIRE(smoother.target() == 0.25f);

    smoother.set_target(1.0f);
    smoother.skip(99);
    REQUIRE(smoother.current() == 1.0f);
    REQUIRE_FALSE(smoother.is_smoothing());
}
