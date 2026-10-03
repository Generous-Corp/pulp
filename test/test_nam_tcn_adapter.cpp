#include <catch2/catch_test_macros.hpp>

#include "detail/nam_tcn_adapter.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <array>

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
