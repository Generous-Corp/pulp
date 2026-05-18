#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/headless.hpp>
#include <cmath>

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

    void release() override { ++release_calls; }

    void process(
        pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input,
        pulp::midi::MidiBuffer& midi_in, pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext& context) override
    {
        last_context = context;
        ++process_calls;
        midi_in_events = 0;
        for (const auto& event : midi_in) {
            if (event.is_note_on()) ++midi_note_ons;
            ++midi_in_events;
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

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
    pulp::format::PrepareContext last_prepare_context{};
    int prepare_calls = 0;
    int release_calls = 0;
    int process_calls = 0;
    int midi_in_events = 0;
    int midi_note_ons = 0;
};

std::unique_ptr<pulp::format::Processor> create_test_gain() {
    return std::make_unique<TestGainProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_null_processor() {
    return {};
}

} // anonymous namespace

using Catch::Matchers::WithinAbs;

TEST_CASE("HeadlessHost creates processor", "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(host.descriptor().name == "TestGain");
    REQUIRE(host.state().param_count() == 1);
}

TEST_CASE("build_editor_ui falls back to AutoUi when no script path is configured",
          "[format][editor_ui][codecov]") {
    REQUIRE_FALSE(pulp::format::configured_ui_script_path().has_value());

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
}

TEST_CASE("HeadlessHost forwards prepare context and release",
          "[headless][coverage][issue-647]") {
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

TEST_CASE("HeadlessHost fills default process context",
          "[headless][coverage][issue-647]") {
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
}

TEST_CASE("HeadlessHost defaults non-positive context fields",
          "[headless][coverage][phase3]") {
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

    pulp::format::ProcessContext explicit_ctx;
    explicit_ctx.sample_rate = -1.0;
    explicit_ctx.num_samples = -32;
    host.process(out_view, in_view, explicit_ctx);
    REQUIRE_THAT(last_context.sample_rate, WithinAbs(88200.0, 0.001));
    REQUIRE(last_context.num_samples == 8);

    explicit_ctx.sample_rate = 44100.0;
    explicit_ctx.num_samples = 4;
    host.process(out_view, in_view, explicit_ctx);
    REQUIRE_THAT(last_context.sample_rate, WithinAbs(44100.0, 0.001));
    REQUIRE(last_context.num_samples == 4);
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

TEST_CASE("HeadlessHost accepts explicit transport context for offline stepping", "[headless][phase12]") {
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

TEST_CASE("HeadlessHost forwards explicit MIDI buffers",
          "[headless][coverage][issue-647]") {
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

TEST_CASE("HeadlessHost null processor process and release are no-ops",
          "[headless][coverage][issue-647]") {
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
