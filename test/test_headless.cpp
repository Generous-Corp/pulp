#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/format/background_task_lane.hpp>
#include <pulp/format/registry.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

// Simple test processor for headless testing
namespace {

static pulp::format::ProcessContext last_context{};
static class TestGainProcessor* last_processor = nullptr;

class TestGainProcessor : public pulp::format::Processor {
public:
    TestGainProcessor() { last_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "TestGain",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.gain",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .supported_bus_layouts = {
                {.inputs = {2}, .outputs = {2}, .name = "Stereo"},
                {.inputs = {1}, .outputs = {1}, .name = "Mono"},
            },
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
    }

    void prepare(const pulp::format::PrepareContext& context) override {
        ++prepare_calls;
        last_prepare_context = context;
    }

    pulp::format::PrepareResourceUsage estimate_prepare_resources(
        const pulp::format::PrepareContext& context) const override {
        return {
            .persistent_bytes = estimated_persistent_bytes,
            .block_scratch_bytes = estimated_block_scratch_bytes,
            .block_size = context.max_buffer_size,
            .input_channels = context.input_channels,
            .output_channels = context.output_channels,
            .parameter_events = estimated_parameter_events,
            .midi_events = estimated_midi_events,
            .voices = estimated_voices,
        };
    }

    void release() override { ++release_calls; }

    void process(
        pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input,
        pulp::midi::MidiBuffer& midi_in, pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext& context) override
    {
        last_context = context;
        contexts.push_back(context);
        ++process_calls;
        midi_in_events = 0;
        for (const auto& event : midi_in) {
            if (event.is_note_on()) ++midi_note_ons;
            ++midi_in_events;
        }
        had_param_events = (param_events() != nullptr);
        last_param_events.clear();
        if (auto* events = param_events()) {
            for (const auto& event : *events) last_param_events.push_back(event);
        }

        float db = state().get_value(1);
        float gain = std::pow(10.0f, db / 20.0f);
        for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                out[i] = in[i] * gain;
        }
    }

    void process(
        pulp::format::ProcessBuffers& audio,
        pulp::midi::MidiBuffer& midi_in,
        pulp::midi::MidiBuffer& midi_out,
        const pulp::format::ProcessContext& context) override
    {
        ++process_buffer_calls;
        process_buffer_input_buses = audio.inputs.size();
        process_buffer_output_buses = audio.outputs.size();
        process_buffer_active_inputs = audio.inputs.active_count();
        process_buffer_active_outputs = audio.outputs.active_count();
        process_buffer_layouts_match = audio.layouts_match_descriptors();
        process_buffer_storage_valid = audio.active_buses_have_storage();

        pulp::format::Processor::process(audio, midi_in, midi_out, context);
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
    std::size_t estimated_persistent_bytes = 0;
    std::size_t estimated_block_scratch_bytes = 0;
    int estimated_parameter_events = 0;
    int estimated_midi_events = 0;
    int estimated_voices = 0;
    pulp::format::PrepareContext last_prepare_context{};
    int prepare_calls = 0;
    int release_calls = 0;
    int process_calls = 0;
    int process_buffer_calls = 0;
    std::size_t process_buffer_input_buses = 0;
    std::size_t process_buffer_output_buses = 0;
    std::size_t process_buffer_active_inputs = 0;
    std::size_t process_buffer_active_outputs = 0;
    bool process_buffer_layouts_match = false;
    bool process_buffer_storage_valid = false;
    int midi_in_events = 0;
    int midi_note_ons = 0;
    bool had_param_events = false;
    std::vector<pulp::state::ParameterEvent> last_param_events;
    std::vector<pulp::format::ProcessContext> contexts;
};

class NativeF64ProbeProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "NativeF64Probe",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.native-f64",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 1}},
            .output_buses = {{"Audio Out", 1}},
            .supports_f64_audio = true,
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {
        ++f32_process_calls;
        output.clear();
    }

    void process_f64(
        pulp::format::ProcessBuffers64& audio,
        pulp::midi::MidiBuffer& midi_in,
        pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext& context) override
    {
        ++f64_process_calls;
        last_context = context;
        midi_in_events = 0;
        for (const auto& event : midi_in) {
            static_cast<void>(event);
            ++midi_in_events;
        }
        had_param_events = (param_events() != nullptr);
        last_param_events.clear();
        if (auto* events = param_events()) {
            for (const auto& event : *events) last_param_events.push_back(event);
        }

        auto* output = audio.main_output();
        auto* input = audio.main_input();
        if (!output || !input) return;

        const auto channels =
            (std::min)(output->num_channels(), input->num_channels());
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const auto src = input->channel(ch);
            auto dst = output->channel(ch);
            for (std::size_t i = 0; i < dst.size(); ++i) {
                dst[i] = src[i] + 1.0e-12;
            }
        }
    }

    int f32_process_calls = 0;
    int f64_process_calls = 0;
    int midi_in_events = 0;
    bool had_param_events = false;
    pulp::format::ProcessContext last_context{};
    std::vector<pulp::state::ParameterEvent> last_param_events;
};

class SidechainProbeProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "SidechainProbe",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.sidechain-probe",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 1, false}, {"Sidechain", 1, true}},
            .output_buses = {{"Main Out", 1, false}},
        };
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const auto* sc = sidechain_input();
        saw_sidechain = sc != nullptr;
        if (sc) buffers_distinct = sc->channel(0).data() != input.channel(0).data();
        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            const float side = sc ? sc->channel(0)[i] : 0.0f;
            output.channel(0)[i] = input.channel(0)[i] + 10.0f * side;
        }
    }
    bool saw_sidechain = false;
    bool buffers_distinct = false;
};

std::unique_ptr<pulp::format::Processor> create_test_gain() {
    return std::make_unique<TestGainProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_native_f64_probe() {
    return std::make_unique<NativeF64ProbeProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_sidechain_probe() {
    return std::make_unique<SidechainProbeProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_null_processor() {
    return {};
}

class ScopedFactoryRegistration {
public:
    explicit ScopedFactoryRegistration(pulp::format::ProcessorFactory factory)
        : previous_(pulp::format::registered_factory()) {
        pulp::format::register_plugin(factory);
    }

    ~ScopedFactoryRegistration() {
        pulp::format::register_plugin(previous_);
    }

private:
    pulp::format::ProcessorFactory previous_ = nullptr;
};

} // anonymous namespace

using Catch::Matchers::WithinAbs;

TEST_CASE("HeadlessHost injects sidechain as an independent bus", "[headless][sidechain]") {
    pulp::format::HeadlessHost host(create_sidechain_probe);
    host.prepare(48000.0, 8, 1, 1);
    pulp::audio::Buffer<float> main(1, 4), sidechain(1, 4), output(1, 4);
    for (std::size_t i = 0; i < 4; ++i) {
        main.channel(0)[i] = static_cast<float>(i + 1);
        sidechain.channel(0)[i] = 0.1f * static_cast<float>(i + 1);
    }
    const float* main_ptrs[] = {main.channel(0).data()};
    const float* side_ptrs[] = {sidechain.channel(0).data()};
    pulp::audio::BufferView<const float> main_view(main_ptrs, 1, 4);
    pulp::audio::BufferView<const float> side_view(side_ptrs, 1, 4);
    auto output_view = output.view();

    host.process_with_sidechain(output_view, main_view, side_view);
    auto* probe = host.processor_as<SidechainProbeProcessor>();
    REQUIRE(probe != nullptr);
    REQUIRE(probe->saw_sidechain);
    REQUIRE(probe->buffers_distinct);
    for (std::size_t i = 0; i < 4; ++i)
        REQUIRE_THAT(output.channel(0)[i], WithinAbs(2.0 * (i + 1), 0.0001));

    probe->saw_sidechain = true;
    host.process(output_view, main_view);
    REQUIRE_FALSE(probe->saw_sidechain);
}

TEST_CASE("HeadlessHost offline render drives an independent sidechain file",
          "[headless][sidechain][offline]") {
    pulp::format::HeadlessHost host(create_sidechain_probe);
    host.prepare(48000.0, 3, 1, 1);
    pulp::audio::AudioFileData main{{{1.0f, 2.0f, 3.0f, 4.0f}}, 48000};
    pulp::audio::AudioFileData side{{{0.1f, 0.2f, 0.3f, 0.4f}}, 48000};
    pulp::audio::OfflineRenderOptions options;
    options.block_size_schedule = {3, 1};

    const auto rendered = host.render_offline_with_sidechain(main, side, options);
    REQUIRE(rendered.has_value());
    REQUIRE(rendered->num_frames() == 4);
    for (std::size_t i = 0; i < 4; ++i)
        REQUIRE_THAT(rendered->channels[0][i], WithinAbs(2.0 * (i + 1), 0.0001));

    side.sample_rate = 44100;
    REQUIRE_FALSE(host.render_offline_with_sidechain(main, side, options).has_value());
}

TEST_CASE("HeadlessHost creates processor", "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(host.descriptor().name == "TestGain");
    REQUIRE(host.state().param_count() == 1);
}

TEST_CASE("HeadlessHost exposes the wrapped processor for typed introspection",
          "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(host.valid());
    // processor() returns the live instance (the one the factory produced).
    REQUIRE(host.processor() != nullptr);
    REQUIRE(host.processor() == last_processor);
    // processor_as<T>() yields the typed pointer when the dynamic type matches,
    // and nullptr otherwise — letting tests reach custom state (e.g. a meter).
    REQUIRE(host.processor_as<TestGainProcessor>() != nullptr);
    REQUIRE(host.processor_as<TestGainProcessor>() == last_processor);
    struct Unrelated { virtual ~Unrelated() = default; };  // complete, polymorphic
    REQUIRE(host.processor_as<Unrelated>() == nullptr);     // cross-cast misses

    const auto& const_host = host;
    REQUIRE(const_host.processor() == host.processor());
}

TEST_CASE("format registry stores and replaces the processor factory",
          "[format][registry]") {
    const auto previous = pulp::format::registered_factory();
    {
        ScopedFactoryRegistration scoped(create_test_gain);
        REQUIRE(pulp::format::registered_factory() == create_test_gain);

        pulp::format::register_plugin(create_null_processor);
        REQUIRE(pulp::format::registered_factory() == create_null_processor);
    }
    REQUIRE(pulp::format::registered_factory() == previous);
}

TEST_CASE("build_editor_ui falls back to AutoUi when no script path is configured",
          "[format][editor_ui]") {
    if (pulp::format::configured_ui_script_path().has_value()) {
        SKIP("AutoUi fallback requires a build without PULP_UI_SCRIPT_PATH");
    }

    pulp::format::HeadlessHost host(create_test_gain);
    std::string error = "preexisting";
    auto ui = pulp::format::build_editor_ui(host.state(), false, &error);

    REQUIRE(ui.root != nullptr);
    REQUIRE(ui.scripted_ui == nullptr);
    REQUIRE_FALSE(ui.uses_script_ui);
    REQUIRE(error == "preexisting");
}

TEST_CASE("HeadlessHost processes audio at unity gain", "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;
    host.prepare(48000.0, 256);

    pulp::audio::Buffer<float> in(2, 128), out(2, 128);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 128; ++i)
            in.channel(ch)[i] = 0.5f;

    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 128);
    auto out_view = out.view();

    host.process(out_view, in_view);

    // 0 dB = unity gain
    REQUIRE_THAT(out.channel(0)[0], WithinAbs(0.5, 0.001));
    REQUIRE(processor->process_calls == 1);
    REQUIRE(processor->process_buffer_calls == 1);
    REQUIRE(processor->process_buffer_input_buses == 1);
    REQUIRE(processor->process_buffer_output_buses == 1);
    REQUIRE(processor->process_buffer_active_inputs == 1);
    REQUIRE(processor->process_buffer_active_outputs == 1);
    REQUIRE(processor->process_buffer_layouts_match);
    REQUIRE(processor->process_buffer_storage_valid);
}

TEST_CASE("HeadlessHost forwards prepare context and release",
          "[headless][issue-647]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;

    host.prepare(96000.0, 1024, 1, 4);
    REQUIRE(processor->prepare_calls == 1);
    REQUIRE_THAT(processor->last_prepare_context.sample_rate,
                 WithinAbs(96000.0, 0.001));
    REQUIRE(processor->last_prepare_context.max_buffer_size == 1024);
    REQUIRE(processor->last_prepare_context.input_channels == 1);
    REQUIRE(processor->last_prepare_context.output_channels == 4);

    host.release();
    REQUIRE(processor->release_calls == 1);
}

TEST_CASE("HeadlessHost prepares a declared bus layout by index",
          "[headless][bus-layout]") {
    pulp::format::HeadlessHost host(create_test_gain);
    auto* processor = last_processor;

    REQUIRE(host.try_prepare_bus_layout(1, 48000.0, 256));
    REQUIRE(processor->last_prepare_context.input_channels == 1);
    REQUIRE(processor->last_prepare_context.output_channels == 1);
    REQUIRE_FALSE(host.try_prepare_bus_layout(2, 48000.0, 256));
}

TEST_CASE("HeadlessHost try_prepare enforces processor resource budgets",
          "[headless][prepare-budget]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;
    processor->estimated_persistent_bytes = 4096;
    processor->estimated_block_scratch_bytes = 1024;
    processor->estimated_parameter_events = 32;
    processor->estimated_midi_events = 64;
    processor->estimated_voices = 8;

    pulp::format::PrepareResourceLimits limits;
    limits.max_total_bytes = 8192;
    limits.max_block_size = 512;
    limits.max_input_channels = 2;
    limits.max_output_channels = 2;
    limits.max_parameter_events = 64;
    limits.max_midi_events = 128;
    limits.max_voices = 16;

    REQUIRE(host.try_prepare(48000.0, 256, 2, 2, limits));
    REQUIRE(host.last_prepare_limit_failure() ==
            pulp::format::PrepareResourceLimit::None);
    REQUIRE(processor->prepare_calls == 1);
    REQUIRE(processor->last_prepare_context.resource_limits.max_total_bytes == 8192);

    limits.max_total_bytes = 4096;
    REQUIRE_FALSE(host.try_prepare(96000.0, 256, 2, 2, limits));
    REQUIRE(host.last_prepare_limit_failure() ==
            pulp::format::PrepareResourceLimit::TotalBytes);
    REQUIRE(processor->prepare_calls == 1);
    REQUIRE_THAT(processor->last_prepare_context.sample_rate,
                 WithinAbs(48000.0, 0.001));
}

TEST_CASE("HeadlessHost failed budget prepare preserves active render context",
          "[headless][prepare-budget]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;
    processor->estimated_persistent_bytes = 4096;
    processor->estimated_block_scratch_bytes = 1024;

    pulp::format::PrepareResourceLimits limits;
    limits.max_total_bytes = 8192;
    REQUIRE(host.try_prepare(48000.0, 256, 2, 2, limits));

    limits.max_total_bytes = 4096;
    REQUIRE_FALSE(host.try_prepare(96000.0, 512, 2, 2, limits));
    REQUIRE(host.last_prepare_limit_failure() ==
            pulp::format::PrepareResourceLimit::TotalBytes);
    REQUIRE(processor->prepare_calls == 1);

    pulp::audio::Buffer<float> in(2, 32), out(2, 32);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 32; ++i)
            in.channel(ch)[i] = 0.25f;

    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 32);
    auto out_view = out.view();

    host.process(out_view, in_view);

    REQUIRE(processor->process_calls == 1);
    REQUIRE_THAT(last_context.sample_rate, WithinAbs(48000.0, 0.001));
    REQUIRE(last_context.num_samples == 32);
    REQUIRE_THAT(out.channel(0)[0], WithinAbs(0.25, 0.001));
}

TEST_CASE("HeadlessHost try_prepare reports channel and voice budget failures",
          "[headless][prepare-budget]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;
    processor->estimated_voices = 12;

    pulp::format::PrepareResourceLimits limits;
    limits.max_input_channels = 1;
    limits.max_voices = 16;
    REQUIRE_FALSE(host.try_prepare(48000.0, 128, 2, 2, limits));
    REQUIRE(host.last_prepare_limit_failure() ==
            pulp::format::PrepareResourceLimit::InputChannels);
    REQUIRE(processor->prepare_calls == 0);

    limits.max_input_channels = 2;
    limits.max_voices = 8;
    REQUIRE_FALSE(host.try_prepare(48000.0, 128, 2, 2, limits));
    REQUIRE(host.last_prepare_limit_failure() ==
            pulp::format::PrepareResourceLimit::Voices);
    REQUIRE(processor->prepare_calls == 0);
}

TEST_CASE("HeadlessHost fills default process context",
          "[headless][issue-647]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(44100.0, 512);

    pulp::audio::Buffer<float> in(2, 32), out(2, 32);
    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 32);
    auto out_view = out.view();

    last_context = {};
    host.process(out_view, in_view);

    REQUIRE_THAT(last_context.sample_rate, WithinAbs(44100.0, 0.001));
    REQUIRE(last_context.num_samples == 32);
    REQUIRE(last_context.process_mode == pulp::format::ProcessMode::Offline);
    REQUIRE(last_context.render_speed_hint ==
            pulp::format::RenderSpeedHint::FasterThanRealtime);
    REQUIRE(last_context.is_offline());
    REQUIRE(last_context.allows_offline_quality_work());
}

TEST_CASE("HeadlessHost defaults non-positive context fields",
          "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(88200.0, 512);

    pulp::audio::Buffer<float> in(1, 8), out(1, 8);
    const float* in_ptrs[1] = {in.channel(0).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 8);
    auto out_view = out.view();

    pulp::format::ProcessContext zero_ctx;
    host.process(out_view, in_view, zero_ctx);
    REQUIRE_THAT(last_context.sample_rate, WithinAbs(88200.0, 0.001));
    REQUIRE(last_context.num_samples == 8);
    REQUIRE(last_context.process_mode == pulp::format::ProcessMode::Realtime);
    REQUIRE_FALSE(last_context.is_offline());

    pulp::format::ProcessContext explicit_ctx;
    explicit_ctx.sample_rate = -1.0;
    explicit_ctx.num_samples = -32;
    explicit_ctx.process_mode = pulp::format::ProcessMode::Offline;
    explicit_ctx.render_speed_hint = pulp::format::RenderSpeedHint::SlowerThanRealtime;
    host.process(out_view, in_view, explicit_ctx);
    REQUIRE_THAT(last_context.sample_rate, WithinAbs(88200.0, 0.001));
    REQUIRE(last_context.num_samples == 8);
    REQUIRE(last_context.process_mode == pulp::format::ProcessMode::Offline);
    REQUIRE(last_context.render_speed_hint ==
            pulp::format::RenderSpeedHint::SlowerThanRealtime);
    REQUIRE(last_context.allows_offline_quality_work());

    explicit_ctx.sample_rate = 44100.0;
    explicit_ctx.num_samples = 4;
    explicit_ctx.process_mode = pulp::format::ProcessMode::Realtime;
    explicit_ctx.render_speed_hint = pulp::format::RenderSpeedHint::Realtime;
    host.process(out_view, in_view, explicit_ctx);
    REQUIRE_THAT(last_context.sample_rate, WithinAbs(44100.0, 0.001));
    REQUIRE(last_context.num_samples == 4);
    REQUIRE(last_context.process_mode == pulp::format::ProcessMode::Realtime);
    REQUIRE(last_context.render_speed_hint == pulp::format::RenderSpeedHint::Realtime);
    REQUIRE_FALSE(last_context.allows_offline_quality_work());
}

TEST_CASE("HeadlessHost prepares and drives the f64 fallback path",
          "[headless][f64]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;
    host.prepare(96000.0, 16, 1, 1);

    pulp::audio::Buffer<double> in(1, 5), out(1, 5);
    in.channel(0)[0] = 0.123456789012345;
    in.channel(0)[1] = -0.25;
    in.channel(0)[2] = 0.5;
    in.channel(0)[3] = -0.75;
    in.channel(0)[4] = 0.875;

    auto in_view = std::as_const(in).view();
    auto out_view = out.view();

    last_context = {};
    host.process_f64(out_view, in_view);

    REQUIRE(processor->process_calls == 1);
    REQUIRE(processor->process_buffer_calls == 0);
    REQUIRE_THAT(last_context.sample_rate, WithinAbs(96000.0, 0.001));
    REQUIRE(last_context.num_samples == 5);
    REQUIRE(last_context.process_mode == pulp::format::ProcessMode::Offline);
    REQUIRE(last_context.render_speed_hint ==
            pulp::format::RenderSpeedHint::FasterThanRealtime);

    const double expected =
        static_cast<double>(static_cast<float>(in.channel(0)[0]));
    REQUIRE_THAT(out.channel(0)[0], WithinAbs(expected, 0.0));
    REQUIRE(out.channel(0)[0] != 0.0);
}

TEST_CASE("HeadlessHost routes opted-in plugins to native f64 processing",
          "[headless][f64]") {
    pulp::format::HeadlessHost host(create_native_f64_probe);
    auto* processor = host.processor_as<NativeF64ProbeProcessor>();
    REQUIRE(processor != nullptr);
    REQUIRE(host.descriptor().effective_capabilities().supports_f64_audio);
    host.prepare(48000.0, 32, 1, 1);

    pulp::audio::Buffer<double> in(1, 4), out(1, 4);
    in.channel(0)[0] = 1.0 / 3.0;
    in.channel(0)[1] = -0.5;
    in.channel(0)[2] = 0.75;
    in.channel(0)[3] = -1.0;

    auto in_view = std::as_const(in).view();
    auto out_view = out.view();
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    midi_in.add(pulp::midi::MidiEvent::note_on(0, 60, 100));

    pulp::state::ParameterEventQueue events;
    events.push({7, 2, 0.25f});

    pulp::format::ProcessContext context;
    context.sample_rate = 44100.0;
    context.num_samples = 4;
    context.position_samples = 128;

    host.process_f64(out_view, in_view, midi_in, midi_out, events, context);

    REQUIRE(processor->f64_process_calls == 1);
    REQUIRE(processor->f32_process_calls == 0);
    REQUIRE(processor->midi_in_events == 1);
    REQUIRE(processor->had_param_events);
    REQUIRE(processor->last_param_events.size() == 1);
    REQUIRE(processor->last_param_events[0].sample_offset == 2);
    REQUIRE_THAT(processor->last_context.sample_rate,
                 WithinAbs(44100.0, 0.001));
    REQUIRE(processor->last_context.num_samples == 4);
    REQUIRE(processor->last_context.position_samples == 128);

    REQUIRE_THAT(out.channel(0)[0],
                 WithinAbs((1.0 / 3.0) + 1.0e-12, 1.0e-18));
    REQUIRE(std::abs(out.channel(0)[0] -
                     static_cast<double>(static_cast<float>(1.0 / 3.0)))
            > 1.0e-10);
}

TEST_CASE("HeadlessHost applies parameter changes", "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);

    host.state().set_value(1, 6.0f); // +6 dB ≈ 2x

    pulp::audio::Buffer<float> in(2, 128), out(2, 128);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 128; ++i)
            in.channel(ch)[i] = 0.5f;

    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 128);
    auto out_view = out.view();

    host.process(out_view, in_view);

    float expected = 0.5f * std::pow(10.0f, 6.0f / 20.0f);
    REQUIRE_THAT(static_cast<double>(out.channel(0)[0]),
                 Catch::Matchers::WithinRel(static_cast<double>(expected), 0.01));
}

TEST_CASE("HeadlessHost accepts explicit transport context for offline stepping", "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);

    pulp::audio::Buffer<float> in(2, 128), out(2, 128);
    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 128);
    auto out_view = out.view();

    last_context = {};
    pulp::format::ProcessContext ctx;
    ctx.is_playing = true;
    ctx.tempo_bpm = 98.0;
    ctx.position_beats = 12.5;
    ctx.position_samples = 2048;
    ctx.time_sig_numerator = 7;
    ctx.time_sig_denominator = 8;

    host.process(out_view, in_view, ctx);

    REQUIRE_THAT(last_context.sample_rate, WithinAbs(48000.0, 0.001));
    REQUIRE(last_context.num_samples == 128);
    REQUIRE(last_context.is_playing);
    REQUIRE_THAT(last_context.tempo_bpm, WithinAbs(98.0, 0.001));
    REQUIRE_THAT(last_context.position_beats, WithinAbs(12.5, 0.001));
    REQUIRE(last_context.position_samples == 2048);
    REQUIRE(last_context.time_sig_numerator == 7);
    REQUIRE(last_context.time_sig_denominator == 8);
}

TEST_CASE("HeadlessHost forwards explicit runtime mode maintenance flags",
          "[headless][runtime-mode]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);

    pulp::audio::Buffer<float> in(2, 64), out(2, 64);
    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 64);
    auto out_view = out.view();

    pulp::format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 64;
    ctx.process_mode = pulp::format::ProcessMode::Offline;
    ctx.render_speed_hint = pulp::format::RenderSpeedHint::SlowerThanRealtime;
    ctx.is_bypassed = true;
    ctx.is_tail_drain = true;
    ctx.reset_requested = true;
    ctx.transport_jump = true;

    last_context = {};
    host.process(out_view, in_view, ctx);

    REQUIRE(last_context.process_mode == pulp::format::ProcessMode::Offline);
    REQUIRE(last_context.render_speed_hint ==
            pulp::format::RenderSpeedHint::SlowerThanRealtime);
    REQUIRE(last_context.is_bypassed);
    REQUIRE(last_context.is_tail_drain);
    REQUIRE(last_context.reset_requested);
    REQUIRE(last_context.transport_jump);
    REQUIRE(last_context.is_offline());
    REQUIRE(last_context.allows_offline_quality_work());
    REQUIRE(last_context.is_maintenance_render());
    REQUIRE(last_context.should_render_tail_only());
    REQUIRE(last_context.should_reset_dsp_state());
}

TEST_CASE("HeadlessHost renders deterministic offline AudioFileData blocks",
          "[headless][offline][advanced]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 4);
    host.state().set_value(1, 6.0f);

    pulp::audio::AudioFileData input;
    input.sample_rate = 48000;
    input.channels = {
        {0.25f, 0.5f, 0.75f},
        {-0.25f, -0.5f, -0.75f},
    };

    pulp::audio::OfflineRenderOptions options;
    options.block_size_schedule = {2, 1};
    options.start_sample_position = 48000;
    options.start_position_beats = 4.0;
    options.tempo_bpm = 90.0;
    options.render_speed_ratio = 2.0;

    auto output = host.render_offline(input, options);

    REQUIRE(output.has_value());
    REQUIRE(output->sample_rate == input.sample_rate);
    REQUIRE(output->num_channels() == 2);
    REQUIRE(output->num_frames() == 3);
    const float expected_gain = std::pow(10.0f, 6.0f / 20.0f);
    REQUIRE_THAT(output->channels[0][0],
                 Catch::Matchers::WithinRel(0.25f * expected_gain, 0.01f));
    REQUIRE_THAT(output->channels[1][2],
                 Catch::Matchers::WithinRel(-0.75f * expected_gain, 0.01f));
    REQUIRE(last_context.process_mode == pulp::format::ProcessMode::Offline);
    REQUIRE(last_context.render_speed_hint ==
            pulp::format::RenderSpeedHint::FasterThanRealtime);
    REQUIRE(last_context.num_samples == 1);
    REQUIRE(last_context.position_samples == 48002);
    REQUIRE_THAT(last_context.position_beats,
                 WithinAbs(4.0 + 2.0 / 48000.0 * 1.5, 1e-12));
    REQUIRE_THAT(last_context.tempo_bpm, WithinAbs(90.0, 1e-12));
}

TEST_CASE("HeadlessHost offline render matches direct process for stateless effects",
          "[headless][offline][parity][advanced]") {
    pulp::format::HeadlessHost direct_host(create_test_gain);
    direct_host.prepare(48000.0, 3);
    direct_host.state().set_value(1, -3.0f);
    auto* direct_processor = last_processor;
    REQUIRE(direct_processor != nullptr);

    const std::vector<float> in_left = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f};
    const std::vector<float> in_right = {-0.5f, -1.0f, -1.5f, -2.0f, -2.5f};
    std::vector<float> direct_left;
    std::vector<float> direct_right;

    auto run_direct_block = [&](std::size_t offset, std::size_t frames,
                                std::uint64_t sample_position,
                                double position_beats) {
        pulp::audio::Buffer<float> direct_in(2, frames), direct_out(2, frames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            direct_in.channel(0)[frame] = in_left[offset + frame];
            direct_in.channel(1)[frame] = in_right[offset + frame];
        }
        const float* direct_in_ptrs[2] = {
            direct_in.channel(0).data(),
            direct_in.channel(1).data(),
        };
        pulp::audio::BufferView<const float> direct_in_view(
            direct_in_ptrs, 2, frames);
        auto direct_out_view = direct_out.view();

        pulp::format::ProcessContext context;
        context.sample_rate = 48000.0;
        context.num_samples = static_cast<int>(frames);
        context.process_mode = pulp::format::ProcessMode::Offline;
        context.render_speed_hint =
            pulp::format::RenderSpeedHint::FasterThanRealtime;
        context.tempo_bpm = 90.0;
        context.position_samples = static_cast<int64_t>(sample_position);
        context.position_beats = position_beats;

        direct_host.process(direct_out_view, direct_in_view, context);
        direct_left.insert(direct_left.end(), direct_out.channel(0).begin(),
                           direct_out.channel(0).end());
        direct_right.insert(direct_right.end(), direct_out.channel(1).begin(),
                            direct_out.channel(1).end());
    };

    run_direct_block(0, 3, 960, 2.0);
    run_direct_block(3, 2, 963, 2.0 + 3.0 / 48000.0 * 1.5);

    REQUIRE(direct_processor->contexts.size() == 2);
    REQUIRE(direct_processor->contexts[0].num_samples == 3);
    REQUIRE(direct_processor->contexts[1].num_samples == 2);
    REQUIRE(direct_processor->contexts[0].position_samples == 960);
    REQUIRE(direct_processor->contexts[1].position_samples == 963);
    REQUIRE(direct_processor->contexts[0].process_mode ==
            pulp::format::ProcessMode::Offline);
    REQUIRE(direct_processor->contexts[1].render_speed_hint ==
            pulp::format::RenderSpeedHint::FasterThanRealtime);

    pulp::format::HeadlessHost offline_host(create_test_gain);
    offline_host.prepare(48000.0, 3);
    offline_host.state().set_value(1, -3.0f);
    auto* offline_processor = last_processor;
    REQUIRE(offline_processor != nullptr);
    pulp::audio::AudioFileData offline_input;
    offline_input.sample_rate = 48000;
    offline_input.channels = {in_left, in_right};
    pulp::audio::OfflineRenderOptions options;
    options.block_size_schedule = {3, 2};
    options.start_sample_position = 960;
    options.start_position_beats = 2.0;
    options.tempo_bpm = 90.0;
    options.render_speed_ratio = 2.0;
    auto offline_output = offline_host.render_offline(offline_input, options);

    REQUIRE(offline_output.has_value());
    REQUIRE(offline_output->num_frames() == direct_left.size());
    REQUIRE(offline_output->channels[0].size() == direct_left.size());
    REQUIRE(offline_output->channels[1].size() == direct_right.size());
    for (std::size_t frame = 0; frame < direct_left.size(); ++frame) {
        REQUIRE_THAT(offline_output->channels[0][frame],
                     WithinAbs(direct_left[frame], 1e-6f));
        REQUIRE_THAT(offline_output->channels[1][frame],
                     WithinAbs(direct_right[frame], 1e-6f));
    }

    REQUIRE(offline_processor->contexts.size() == direct_processor->contexts.size());
    for (std::size_t i = 0; i < direct_processor->contexts.size(); ++i) {
        REQUIRE(offline_processor->contexts[i].num_samples ==
                direct_processor->contexts[i].num_samples);
        REQUIRE(offline_processor->contexts[i].position_samples ==
                direct_processor->contexts[i].position_samples);
        REQUIRE_THAT(offline_processor->contexts[i].position_beats,
                     WithinAbs(direct_processor->contexts[i].position_beats, 1e-12));
        REQUIRE_THAT(offline_processor->contexts[i].tempo_bpm,
                     WithinAbs(direct_processor->contexts[i].tempo_bpm, 1e-12));
        REQUIRE(offline_processor->contexts[i].process_mode ==
                direct_processor->contexts[i].process_mode);
        REQUIRE(offline_processor->contexts[i].render_speed_hint ==
                direct_processor->contexts[i].render_speed_hint);
    }
}

TEST_CASE("HeadlessHost forwards explicit MIDI buffers",
          "[headless][issue-647]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;

    pulp::audio::Buffer<float> in(2, 16), out(2, 16);
    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 16);
    auto out_view = out.view();
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    midi_in.add(pulp::midi::MidiEvent::note_on(0, 64, 100));

    host.process(out_view, in_view, midi_in, midi_out);

    REQUIRE(processor->process_calls == 1);
    REQUIRE(processor->midi_in_events == 1);
    REQUIRE(processor->midi_note_ons == 1);
}

TEST_CASE("HeadlessHost forwards explicit parameter-event queues",
          "[headless][params]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;

    pulp::audio::Buffer<float> in(2, 16), out(2, 16);
    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 16);
    auto out_view = out.view();

    pulp::state::ParameterEventQueue events;
    events.push({1, 12, -3.0f});
    events.push({1, 4, -12.0f});
    events.sort();

    host.process(out_view, in_view, events);

    REQUIRE(processor->process_buffer_calls == 1);
    REQUIRE(processor->process_buffer_input_buses == 1);
    REQUIRE(processor->process_buffer_output_buses == 1);
    REQUIRE(processor->process_buffer_layouts_match);
    REQUIRE(processor->process_buffer_storage_valid);
    REQUIRE(processor->had_param_events);
    REQUIRE(processor->last_param_events.size() == 2);
    REQUIRE(processor->last_param_events[0].sample_offset == 4);
    REQUIRE(processor->last_param_events[0].value == -12.0f);
    REQUIRE(processor->last_param_events[1].sample_offset == 12);
    REQUIRE(processor->last_param_events[1].value == -3.0f);

    host.process(out_view, in_view);
    REQUIRE_FALSE(processor->had_param_events);
}

TEST_CASE("HeadlessHost forwards MIDI, parameter events, and explicit context together",
          "[headless][params]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);
    REQUIRE(last_processor != nullptr);
    auto* processor = last_processor;

    pulp::audio::Buffer<float> in(1, 8), out(1, 8);
    const float* in_ptrs[1] = {in.channel(0).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 8);
    auto out_view = out.view();

    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    midi_in.add(pulp::midi::MidiEvent::note_on(0, 60, 100));

    pulp::state::ParameterEventQueue events;
    events.push({1, 6, -9.0f});

    pulp::format::ProcessContext ctx;
    ctx.sample_rate = 32000.0;
    ctx.num_samples = 6;
    ctx.is_playing = true;
    ctx.position_samples = 128;

    last_context = {};
    host.process(out_view, in_view, midi_in, midi_out, events, ctx);

    REQUIRE(processor->process_calls == 1);
    REQUIRE(processor->midi_in_events == 1);
    REQUIRE(processor->midi_note_ons == 1);
    REQUIRE(processor->had_param_events);
    REQUIRE(processor->last_param_events.size() == 1);
    REQUIRE(processor->last_param_events[0].sample_offset == 6);
    REQUIRE(processor->last_param_events[0].value == -9.0f);
    REQUIRE_THAT(last_context.sample_rate, WithinAbs(32000.0, 0.001));
    REQUIRE(last_context.num_samples == 6);
    REQUIRE(last_context.is_playing);
    REQUIRE(last_context.position_samples == 128);
}

TEST_CASE("HeadlessHost null processor process and release are no-ops",
          "[headless][issue-647]") {
    pulp::format::HeadlessHost host(create_null_processor);
    host.prepare(48000.0, 64);

    pulp::audio::Buffer<float> in(1, 4), out(1, 4);
    for (std::size_t i = 0; i < 4; ++i) out.channel(0)[i] = 7.0f;
    const float* in_ptrs[1] = {in.channel(0).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    auto out_view = out.view();

    host.process(out_view, in_view);
    host.release();

    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE_THAT(out.channel(0)[i], WithinAbs(7.0, 0.001));
    }
}

TEST_CASE("HeadlessHost state round-trip", "[headless]") {
    pulp::format::HeadlessHost host1(create_test_gain);
    host1.state().set_value(1, -12.5f);

    auto saved = host1.save_state();

    pulp::format::HeadlessHost host2(create_test_gain);
    REQUIRE(host2.load_state(saved));
    REQUIRE_THAT(host2.state().get_value(1), WithinAbs(-12.5, 0.01));
}

TEST_CASE("HeadlessHost state round-trip includes plugin-owned payload", "[headless]") {
    pulp::format::HeadlessHost host1(create_test_gain);
    REQUIRE(last_processor != nullptr);
    last_processor->plugin_state = "bands=56;view=40-18000";
    host1.state().set_value(1, -18.0f);

    auto saved = host1.save_state();

    pulp::format::HeadlessHost host2(create_test_gain);
    auto* restored_processor = last_processor;
    REQUIRE(restored_processor != nullptr);
    restored_processor->plugin_state = "stale";

    REQUIRE(host2.load_state(saved));
    REQUIRE_THAT(host2.state().get_value(1), WithinAbs(-18.0, 0.01));
    REQUIRE(restored_processor->plugin_state == "bands=56;view=40-18000");
}

TEST_CASE("HeadlessHost save/load fail cleanly without a processor", "[headless]") {
    pulp::format::HeadlessHost host(create_null_processor);
    REQUIRE(host.save_state().empty());

    const std::vector<uint8_t> bad_state = {'N', 'O', 'P', 'E'};
    REQUIRE_FALSE(host.load_state(bad_state));
}

TEST_CASE("BackgroundTaskLane is bounded, prewarmed, and serialized",
          "[format][background-task]") {
    struct State { std::atomic<int> count{0}; std::atomic<int> last{0}; } state;
    const auto handler = [](void* context, const int& value) noexcept {
        auto& s = *static_cast<State*>(context);
        s.last.store(value, std::memory_order_release);
        s.count.fetch_add(1, std::memory_order_release);
    };
    pulp::format::BackgroundTaskLane<int, 4> lane;
    REQUIRE(lane.start(handler, &state));
    REQUIRE(lane.try_spawn(1));
    REQUIRE(lane.try_spawn(2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (state.count.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    lane.stop();
    REQUIRE(state.count.load() == 2);
    REQUIRE(state.last.load() == 2);
    REQUIRE_FALSE(lane.try_spawn(3));
}

TEST_CASE("BackgroundTaskLane latest policy coalesces safely",
          "[format][background-task]") {
    std::atomic<int> last{0};
    const auto handler = [](void* context, const int& value) noexcept {
        static_cast<std::atomic<int>*>(context)->store(value, std::memory_order_release);
    };
    pulp::format::BackgroundTaskLane<int> lane;
    REQUIRE(lane.start(handler, &last, pulp::format::BackgroundTaskPolicy::Latest));
    for (int value = 1; value <= 100; ++value) REQUIRE(lane.try_spawn(value));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (last.load(std::memory_order_acquire) != 100 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    lane.stop();
    REQUIRE(last.load() == 100);
}
