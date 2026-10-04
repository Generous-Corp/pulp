// test_spectral_frame_engine_stream_start.cpp — the first samples after
// prepare()/reset() must reach the output at full strength.
//
// The equal-hop SpectralFrameEngine path reports latency fft_size +
// analysis_hop. Every input sample therefore has to come back exactly that
// late and at unity gain, including the samples at the very start of a stream,
// which (with frames starting at the first input sample) are covered by fewer
// than fft_size / analysis_hop overlapping windows. A start-of-stream
// attenuation there costs every playback start and every seek its attack.
//
// The evidence is rendered through RenderScenario (fresh prepare per render)
// and compared with the input delayed by the reported latency using the
// harness null assertion. The stimuli are deterministic impulses at fixed
// positions and a synthetic kick; the oracle is the input itself.

#include <catch2/catch_test_macros.hpp>

#include "support/audio_contracts.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>
#include <pulp/signal/spectral_mask_processor.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::test::audio;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kNullToleranceDbfs = -120.0;

struct EngineGeometry {
    int fft_size = 0;
    int hop = 0;
};

struct StreamStartConfig {
    EngineGeometry geometry;
    /// Run noise through the engine and reset() before the render, so the
    /// render starts from a reset stream rather than a freshly prepared one.
    bool reset_after_noise = false;
};

// RenderScenario takes a plain factory, so the per-render configuration has to
// reach the processor out of band.
StreamStartConfig g_stream_start;

class StreamStartIdentityProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "SpectralStreamStart",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.spectral-stream-start",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 1}},
            .output_buses = {{"Audio Out", 1}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const pulp::format::PrepareContext& context) override {
        pulp::signal::SpectralFrameEngineConfig config;
        config.fft_size = g_stream_start.geometry.fft_size;
        config.analysis_hop = g_stream_start.geometry.hop;
        config.channels = 1;
        config.max_block = std::max(context.max_buffer_size, 4096);
        engine_.prepare(config);
        if (g_stream_start.reset_after_noise) {
            // Leave real content in every ring, then reset: the next stream
            // must behave exactly like a fresh prepare().
            std::vector<float> noise(4096), sink(4096);
            unsigned state = 0x2545F491u;
            for (auto& v : noise) {
                state = state * 1664525u + 1013904223u;
                v = static_cast<float>(state >> 8) / 16777216.0f - 0.5f;
            }
            const float* in = noise.data();
            float* out = sink.data();
            for (int i = 0; i < 6; ++i)
                engine_.process(&in, &out, 4096, [](std::complex<float>* const*, int) {});
            engine_.reset();
        }
    }

    int latency_samples() const override { return engine_.latency_samples(); }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const float* ip = in.channel_ptr(0);
        float* op = out.channel_ptr(0);
        engine_.process(&ip, &op, static_cast<int>(out.num_samples()),
                        [](std::complex<float>* const*, int) {});
    }

private:
    pulp::signal::SpectralFrameEngine engine_;
};

std::unique_ptr<pulp::format::Processor> create_stream_start_identity() {
    return std::make_unique<StreamStartIdentityProcessor>();
}

// A synthetic kick: a 150 -> 45 Hz exponential pitch drop under a fast
// exponential decay, starting at full amplitude on its first sample.
pulp::audio::Buffer<float> make_kick(int frames, int onset) {
    pulp::audio::Buffer<float> buffer(1, static_cast<std::size_t>(frames));
    float* data = buffer.view().channel_ptr(0);
    double phase = 0.0;
    for (int i = onset; i < frames; ++i) {
        const double t = static_cast<double>(i - onset) / kSampleRate;
        const double hz = 45.0 + 105.0 * std::exp(-t / 0.03);
        const double env = std::exp(-t / 0.12);
        data[i] = static_cast<float>(0.9 * env * std::cos(phase));
        phase += 2.0 * 3.14159265358979323846 * hz / kSampleRate;
    }
    return buffer;
}

pulp::audio::Buffer<float> delayed(const pulp::audio::Buffer<float>& input, int delay) {
    const auto frames = input.view().num_samples();
    pulp::audio::Buffer<float> out(1, frames);
    const float* src = input.view().channel_ptr(0);
    float* dst = out.view().channel_ptr(0);
    for (std::size_t i = static_cast<std::size_t>(delay); i < frames; ++i)
        dst[i] = src[i - static_cast<std::size_t>(delay)];
    return out;
}

double peak_dbfs(const pulp::audio::Buffer<float>& buffer, std::size_t from, std::size_t to) {
    const float* data = buffer.view().channel_ptr(0);
    float peak = 0.0f;
    for (std::size_t i = from; i < to; ++i) peak = std::max(peak, std::abs(data[i]));
    return 20.0 * std::log10(std::max(static_cast<double>(peak), 1e-15));
}

struct StreamStartCase {
    std::string label;
    pulp::audio::Buffer<float> input;
    int onset = 0;
};

std::vector<StreamStartCase> stream_start_cases(int frames) {
    std::vector<StreamStartCase> cases;
    for (int position : {0, 13, 512, 1024, 2048, 6000})
        cases.push_back({"impulse@" + std::to_string(position),
                         make_impulse(1, frames, 1.0f, position), position});
    cases.push_back({"kick@0", make_kick(frames, 0), 0});
    return cases;
}

void require_stream_start_nulls(EngineGeometry geometry, bool reset_after_noise,
                                int block_size) {
    g_stream_start = {geometry, reset_after_noise};
    const int latency = geometry.fft_size + geometry.hop;
    // Room for the latest stimulus, the delay, and one more window of output.
    const int frames = 6000 + latency + 2 * geometry.fft_size;

    for (auto& c : stream_start_cases(frames)) {
        const auto result = RenderScenario(create_stream_start_identity)
                                .name("spectral.stream-start." + c.label)
                                .sample_rate(kSampleRate)
                                .block_size(block_size)
                                .channels(1, 1)
                                .duration_frames(frames)
                                .input(c.input)
                                .render();
        REQUIRE(result.latency.final_reported_samples == latency);

        const auto expected = delayed(result.input, latency);
        const auto check = assert_null_near(result.output, expected, kNullToleranceDbfs);
        // The delayed onset itself, for the failure message: what an attack
        // that was swallowed reads as.
        const auto at = static_cast<std::size_t>(c.onset + latency);
        INFO("fft=" << geometry.fft_size << " hop=" << geometry.hop
                    << " block=" << block_size << " reset=" << reset_after_noise
                    << " case=" << c.label << " onset peak="
                    << peak_dbfs(result.output, at, at + 1) << " dBFS; "
                    << check.message);
        REQUIRE(check.passed);
    }
}

} // namespace

TEST_CASE("SpectralFrameEngine passes the first samples after prepare at full overlap",
          "[signal][spectral-frame-engine][stream-start]") {
    const EngineGeometry geometries[] = {
        {8192, 2048}, {2048, 512}, {1024, 256}, {4096, 2048}, {1024, 300}};
    for (const auto g : geometries) {
        require_stream_start_nulls(g, /*reset_after_noise=*/false, 512);
        require_stream_start_nulls(g, /*reset_after_noise=*/false, 480);
    }
}

TEST_CASE("SpectralFrameEngine passes the first samples after reset at full overlap",
          "[signal][spectral-frame-engine][stream-start]") {
    const EngineGeometry geometries[] = {{8192, 2048}, {2048, 512}, {1024, 300}};
    for (const auto g : geometries)
        require_stream_start_nulls(g, /*reset_after_noise=*/true, 512);
}

TEST_CASE("SpectralFrameEngine stream-start coverage equals steady-state coverage",
          "[signal][spectral-frame-engine][stream-start]") {
    // The pure coverage function is what the engine normalizes by: with the
    // frame grid starting at first_frame_start(), the first real sample sees
    // exactly the window energy of a sample deep in the stream.
    for (const auto g : {EngineGeometry{8192, 2048}, EngineGeometry{1024, 300}}) {
        pulp::signal::SpectralFrameEngineConfig config;
        config.fft_size = g.fft_size;
        config.analysis_hop = g.hop;
        pulp::signal::SpectralFrameEngine engine;
        engine.prepare(config);
        const auto window = pulp::signal::WindowFunction::generate<float>(
            g.fft_size, pulp::signal::WindowFunction::Type::hann);
        const auto first = engine.first_frame_start();
        INFO("fft=" << g.fft_size << " hop=" << g.hop << " first=" << first);
        REQUIRE(first <= 0);
        REQUIRE(first > -g.fft_size);
        REQUIRE(first % g.hop == 0);
        for (std::int64_t p = 0; p < 3 * g.fft_size; ++p) {
            // Same residue modulo the hop, far past the start.
            const std::int64_t deep = p + 64 * static_cast<std::int64_t>(g.hop);
            const double at_start = pulp::signal::spectral_ola_window_energy(
                window.data(), g.fft_size, g.hop, first, p);
            const double steady = pulp::signal::spectral_ola_window_energy(
                window.data(), g.fft_size, g.hop, first, deep);
            REQUIRE(std::abs(at_start - steady) <= 1e-9 * std::max(1.0, steady));
        }
        // Negative control: a grid that starts at the first sample, as a frame
        // engine without the pre-stream frames would, leaves sample 0 covered
        // only by the zero-valued edge of one window.
        REQUIRE(pulp::signal::spectral_ola_window_energy(
                    window.data(), g.fft_size, g.hop, 0, 0) < 1e-12);
    }
}

TEST_CASE("SpectralMaskProcessor at unity mask passes the stream start untouched",
          "[signal][spectral-frame-engine][stream-start]") {
    // The mask renderer is the shipped consumer of the equal-hop path. At a
    // unity mask its output must be the input delayed by its latency from the
    // first sample, after prepare and again after reset.
    pulp::signal::SpectralMaskProcessorConfig cfg;
    cfg.frame.fft_size = 8192;
    cfg.frame.analysis_hop = 2048;
    cfg.frame.channels = 1;
    cfg.frame.max_block = 512;
    cfg.sample_rate = static_cast<float>(kSampleRate);
    cfg.initial_mix = 1.0f;
    cfg.mix_ramp_samples = 0;
    pulp::signal::SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(cfg));
    const int latency = processor.latency_samples();
    REQUIRE(latency == 8192 + 2048);

    const int frames = 2048 + latency + 8192;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) processor.reset();
        auto input = make_impulse(1, frames, 1.0f, 1024);
        pulp::audio::Buffer<float> output(1, static_cast<std::size_t>(frames));
        const float* in = input.view().channel_ptr(0);
        float* out = output.view().channel_ptr(0);
        for (int pos = 0; pos < frames; pos += 512) {
            const int n = std::min(512, frames - pos);
            const float* ip = in + pos;
            float* op = out + pos;
            REQUIRE(processor.process(&ip, &op, n));
        }
        const auto check = assert_null_near(output, delayed(input, latency), kNullToleranceDbfs);
        INFO("pass=" << pass << " " << check.message);
        REQUIRE(check.passed);
    }
}
