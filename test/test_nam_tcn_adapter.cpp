#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "detail/nam_tcn_adapter.hpp"
#include "detail/nam_tcn_artifact.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "support/fixture_root.hpp"

using namespace pulp::gpu_audio::detail;

namespace {
struct TestKernel {
    float previous = 0.0f;
    bool prepared = false;
};

bool prepare(void* opaque, const StreamingPrepareContext&) noexcept {
    auto& state = *static_cast<TestKernel*>(opaque);
    state.previous = 0.0f;
    state.prepared = true;
    return true;
}

void process(void* opaque, const float* input, float* output, std::uint32_t frames) noexcept {
    auto& state = *static_cast<TestKernel*>(opaque);
    for (std::uint32_t i = 0; i < frames; ++i) {
        output[i] = input[i] + state.previous;
        state.previous = input[i];
    }
}

void reset(void* opaque) noexcept {
    static_cast<TestKernel*>(opaque)->previous = 0.0f;
}
bool quiesce(void*) noexcept {
    return true;
}
bool release(void* opaque) noexcept {
    static_cast<TestKernel*>(opaque)->prepared = false;
    return true;
}

StreamingModelSpec spec() {
    return {.model_id = "nam.fixture",
            .architecture = "nam.tcn",
            .model_version = "fixture-1",
            .weights_hash = "weights",
            .runtime_hash = "oracle-bridge",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = 48000,
            .block_size = 2,
            .feature_rate = 48000,
            .intrinsic_latency_samples = 0,
            .receptive_field_samples = 2,
            .state_bytes = sizeof(float),
            .state_schema = "fixture-state-v1",
            .state_schema_version = 1,
            .deterministic = true};
}
} // namespace

TEST_CASE("NAM/TCN adapter delegates stateful CPU blocks without allocation",
          "[gpu_audio][neural][nam][streaming_model]") {
    TestKernel state;
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));

    std::array<float, 2> input{1.0f, 2.0f};
    std::array<float, 2> output{};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 2);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 2);
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 2, {.epoch = 1, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(output == std::array<float, 2>{1.0f, 3.0f});

    model.reset(2, StreamingResetReason::ModelSwap);
    output.fill(0.0f);
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 2, {.epoch = 2, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(output == std::array<float, 2>{1.0f, 3.0f});
    REQUIRE(model.quiesce());
    REQUIRE(model.release());
    CHECK_FALSE(state.prepared);
}

TEST_CASE("NAM/TCN adapter rejects blocks larger than prepared capacity",
          "[gpu_audio][neural][nam][streaming_model]") {
    TestKernel state;
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));

    std::array<float, 3> input{1.0f, 2.0f, 3.0f};
    std::array<float, 3> output{9.0f, 9.0f, 9.0f};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 3);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 3);
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 3, {.epoch = 1, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(output == std::array<float, 3>{0.0f, 0.0f, 0.0f});
    CHECK(state.previous == 0.0f);
}

TEST_CASE("NAM/TCN adapter rejects non-mono buffers at the private boundary",
          "[gpu_audio][neural][nam][streaming_model]") {
    TestKernel state;
    NamTcnStreamingAdapter model(spec(), {.state = &state,
                                          .prepare = prepare,
                                          .process = process,
                                          .reset = reset,
                                          .quiesce = quiesce,
                                          .release = release});
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "fixture.nam",
                                                 .artifact_hash = "fixture-hash",
                                                 .max_frames = 2};
    REQUIRE(model.prepare(context));
    std::array<float, 4> input{1, 2, 3, 4};
    std::array<float, 4> output{9, 9, 9, 9};
    const float* input_channels[] = {input.data(), input.data() + 2};
    float* output_channels[] = {output.data(), output.data() + 2};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 2, 2);
    auto out = pulp::audio::BufferView<float>(output_channels, 2, 2);
    model.process_cpu(in, out, 2, {.epoch = 1, .sequence = 0});
    CHECK(output == std::array<float, 4>{0, 0, 0, 0});
}

namespace {
std::string fixture_path() {
    return (pulp_test::fixture_root() / "test/fixtures/neural/example.nam").string();
}

StreamingModelSpec artifact_spec() {
    return {.model_id = "nam.example",
            .architecture = "nam.wavenet.a1",
            .model_version = "0.5.4",
            .weights_hash = "66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d",
            .runtime_hash = "private-cpu-artifact-bridge-v1",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = 48000,
            .block_size = 64,
            .feature_rate = 48000,
            .intrinsic_latency_samples = 0,
            .receptive_field_samples = 22,
            .state_bytes = 232,
            .state_schema = "nam-wavenet-a1-causal-v1",
            .state_schema_version = 1,
            .deterministic = true};
}

} // namespace

TEST_CASE("serialized NAM artifact matches CPU oracle at 64 and 128 frames with reset parity",
          "[gpu_audio][neural][nam][artifact]") {
    auto spec_value = artifact_spec();
    NamTcnArtifactAdapter model(spec_value, fixture_path());
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "example.nam",
                                                 .artifact_hash = spec_value.weights_hash,
                                                 .max_frames = 128};
    REQUIRE(model.prepare(context));
    std::array<float, 128> input128{};
    for (std::size_t i = 0; i < input128.size(); ++i)
        input128[i] = std::sin(static_cast<float>(i) * 0.071f) +
                      0.25f * std::cos(static_cast<float>(i) * 0.013f);
    std::array<float, 128> output{};
    const float* in_channels[] = {input128.data()};
    float* out_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(in_channels, 1, 128);
    auto out = pulp::audio::BufferView<float>(out_channels, 1, 128);
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 64, {.epoch = 1, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(output[0] == Catch::Approx(-0.01314363f).margin(1.0e-6f));
    CHECK(output[1] == Catch::Approx(-0.012158165f).margin(1.0e-6f));
    CHECK(output[2] == Catch::Approx(-0.014329166f).margin(1.0e-6f));
    CHECK(output[3] == Catch::Approx(-0.016419834f).margin(1.0e-6f));
    {
        pulp::test::ScopedRtProcessProbe probe;
        model.process_cpu(in, out, 64, {.epoch = 1, .sequence = 1});
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(std::isfinite(output[127]));
    model.reset(2, StreamingResetReason::ModelSwap);
    std::array<float, 128> replay{};
    float* replay_channels[] = {replay.data()};
    auto replay_view = pulp::audio::BufferView<float>(replay_channels, 1, 128);
    model.process_cpu(in, replay_view, 128, {.epoch = 2, .sequence = 0});
    CHECK(replay[0] == Catch::Approx(-0.01314363f).margin(1.0e-6f));
    CHECK(replay[1] == Catch::Approx(-0.012158165f).margin(1.0e-6f));
    REQUIRE(model.quiesce());
    REQUIRE(model.release());
}

TEST_CASE("serialized NAM loader rejects unsupported layer state offsets",
          "[gpu_audio][neural][nam]") {
    std::ifstream source(fixture_path());
    std::stringstream contents;
    contents << source.rdbuf();
    const auto marker = std::string("\"input_size\": 1");
    const auto position = contents.str().find(marker);
    REQUIRE(position != std::string::npos);
    auto text = contents.str();
    text.replace(position, marker.size(), "\"state_offset\": 0, \"input_size\": 1");
    const auto path = std::filesystem::temp_directory_path() / "pulp-nam-state-offset.nam";
    {
        std::ofstream output(path);
        output << text;
    }
    NamTcnArtifact artifact;
    std::string error;
    CHECK_FALSE(artifact.load(path.string(), &error));
    CHECK(error.find("state offsets") != std::string::npos);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("serialized NAM loader rejects divergent runtime head scale",
          "[gpu_audio][neural][nam]") {
    std::ifstream source(fixture_path());
    std::stringstream contents;
    contents << source.rdbuf();
    const auto marker = std::string("\"head_scale\": 0.02");
    const auto position = contents.str().find(marker);
    REQUIRE(position != std::string::npos);
    auto text = contents.str();
    text.replace(position, marker.size(), "\"head_scale\": 0.5");
    const auto path = std::filesystem::temp_directory_path() / "pulp-nam-head-scale-mismatch.nam";
    {
        std::ofstream output(path);
        output << text;
    }
    NamTcnArtifact artifact;
    std::string error;
    CHECK_FALSE(artifact.load(path.string(), &error));
    CHECK(error.find("head_scale") != std::string::npos);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
