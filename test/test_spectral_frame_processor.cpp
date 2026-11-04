#include <catch2/catch_test_macros.hpp>
#include <pulp/format/spectral_frame_processor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace {

using pulp::format::SpectralFrameError;
using pulp::format::SpectralFrameProcessor;
using pulp::format::SpectralFrameStage;
using pulp::format::SpectralFrameView;

struct Stage final : SpectralFrameStage {
    enum class Action { Identity, Negate, Mute, NonFinite, Refuse } action;
    std::vector<std::uint64_t> sequences;
    std::vector<std::int64_t> origins;

    explicit Stage(Action value) : action(value) {
        sequences.reserve(128);
        origins.reserve(128);
    }
    bool process(const SpectralFrameView& frame) noexcept override {
        sequences.push_back(frame.sequence);
        origins.push_back(frame.origin_sample);
        if (frame.channel_count != 1 || frame.bin_count != frame.fft_size / 2 + 1 ||
            frame.hop != 64 || frame.sample_rate != 48000.0)
            return false;
        if (action == Action::Refuse)
            return false;
        if (action == Action::Negate)
            for (int bin = 0; bin < frame.bin_count; ++bin)
                frame.channels[0][bin] = -frame.channels[0][bin];
        if (action == Action::Mute)
            std::fill_n(frame.channels[0], frame.bin_count, std::complex<float>{});
        if (action == Action::NonFinite)
            frame.channels[0][1] = {std::numeric_limits<float>::quiet_NaN(), 0};
        return true;
    }
};

struct Result {
    std::vector<float> samples;
    std::vector<std::uint64_t> sequences;
    std::vector<std::int64_t> origins;
    SpectralFrameError error = SpectralFrameError::None;
};

Result render(Stage::Action action, int block_size, bool double_negate = false) {
    pulp::signal::SpectralFrameEngineConfig config;
    config.fft_size = 256;
    config.analysis_hop = 64;
    config.channels = 1;
    auto processor = std::make_unique<SpectralFrameProcessor>(config);
    auto stage = std::make_unique<Stage>(action);
    auto* stage_ptr = stage.get();
    REQUIRE(processor->add_stage(std::move(stage)));
    if (double_negate)
        REQUIRE(processor->add_stage(std::make_unique<Stage>(Stage::Action::Negate)));
    auto* processor_ptr = processor.get();

    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto spectral = graph.add_processor_node(std::move(processor));
    const auto output = graph.add_output_node(1);
    REQUIRE(input != 0);
    REQUIRE(spectral != 0);
    REQUIRE(output != 0);
    REQUIRE(graph.connect(input, 0, spectral, 0));
    REQUIRE(graph.connect(spectral, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, block_size));
    REQUIRE(processor_ptr->latency_samples() == 320);

    constexpr int total = 1024;
    std::vector<float> input_samples(total);
    std::vector<float> output_samples(total);
    for (int i = 0; i < total; ++i)
        input_samples[i] = 0.4f * std::sin(2.0f * 3.14159265358979323846f * i / 37.0f);
    for (int offset = 0; offset < total; offset += block_size) {
        const int count = std::min(block_size, total - offset);
        std::array<const float*, 1> in{input_samples.data() + offset};
        std::array<float*, 1> out{output_samples.data() + offset};
        pulp::audio::BufferView<const float> in_view(in.data(), 1, count);
        pulp::audio::BufferView<float> out_view(out.data(), 1, count);
        graph.process(out_view, in_view, count);
    }
    return {std::move(output_samples), stage_ptr->sequences, stage_ptr->origins,
            processor_ptr->error()};
}

} // namespace

TEST_CASE("typed spectral stages compose in a graph independent of host partition") {
    const auto a = render(Stage::Action::Identity, 64);
    const auto b = render(Stage::Action::Identity, 128);
    REQUIRE(a.error == SpectralFrameError::None);
    REQUIRE(b.error == SpectralFrameError::None);
    REQUIRE(a.samples == b.samples);
    REQUIRE(a.sequences == b.sequences);
    REQUIRE(a.origins == b.origins);
    REQUIRE(a.sequences.size() > 4);
    for (std::size_t i = 0; i < a.sequences.size(); ++i) {
        REQUIRE(a.sequences[i] == i);
        REQUIRE(a.origins[i] == -192 + static_cast<std::int64_t>(i) * 64);
    }
    REQUIRE(std::any_of(a.samples.begin() + 320, a.samples.end(),
                        [](float x) { return std::abs(x) > 0.01f; }));
}

TEST_CASE("two typed spectral stages compose within one graph node") {
    const auto identity = render(Stage::Action::Identity, 64);
    const auto doubled = render(Stage::Action::Negate, 64, true);
    REQUIRE(doubled.error == SpectralFrameError::None);
    REQUIRE(doubled.samples == identity.samples);
    REQUIRE(doubled.sequences == identity.sequences);
}

TEST_CASE("typed spectral stage failures mute the frame with a machine-readable error") {
    const auto identity = render(Stage::Action::Identity, 64);
    for (const auto action :
         {Stage::Action::Mute, Stage::Action::NonFinite, Stage::Action::Refuse}) {
        const auto result = render(action, 64);
        REQUIRE(result.sequences == identity.sequences);
        REQUIRE(std::all_of(result.samples.begin(), result.samples.end(),
                            [](float x) { return x == 0.0f; }));
        const auto expected = action == Stage::Action::NonFinite
                                  ? SpectralFrameError::NonFiniteFrame
                              : action == Stage::Action::Refuse ? SpectralFrameError::StageFailure
                                                                : SpectralFrameError::None;
        REQUIRE(result.error == expected);
    }
}

TEST_CASE("typed spectral adapter rejects incompatible prepared geometry") {
    pulp::signal::SpectralFrameEngineConfig config;
    config.fft_size = 300;
    config.channels = 1;
    SpectralFrameProcessor processor(config);
    pulp::format::PrepareContext context;
    context.input_channels = 1;
    context.output_channels = 1;
    REQUIRE_THROWS_AS(processor.prepare(context), std::invalid_argument);
    REQUIRE(processor.error() == SpectralFrameError::InvalidGeometry);
}
