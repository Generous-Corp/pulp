#include <catch2/catch_test_macros.hpp>

#include "detail/streaming_model.hpp"

#include <array>

namespace {

using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio::detail;

class IdentityModel final : public StreamingModel {
  public:
    IdentityModel()
        : spec_{.model_id = "test.identity",
                .architecture = "test",
                .model_version = "1",
                .weights_hash = "test",
                .runtime_hash = "test",
                .input_channels = 1,
                .output_channels = 1,
                .sample_rate = 48000,
                .block_size = 4,
                .feature_rate = 48000,
                .intrinsic_latency_samples = 0,
                .receptive_field_samples = 1,
                .state_bytes = 0,
                .state_schema = {},
                .deterministic = true} {}

    const StreamingModelSpec& spec() const noexcept override { return spec_; }
    bool prepare(const StreamingPrepareContext& context) noexcept override {
        prepared_ = context.spec == &spec_ && context.max_frames >= spec_.block_size;
        return prepared_;
    }

    void process_cpu(const pulp::audio::BufferView<const float>& input,
                     pulp::audio::BufferView<float>& output, std::uint32_t frames,
                     StreamingBlockStamp stamp) noexcept override {
        if (!prepared_) {
            output.clear();
            return;
        }
        last_stamp_ = stamp;
        for (std::uint32_t i = 0; i < frames; ++i)
            output.channel_ptr(0)[i] = input.channel_ptr(0)[i];
    }

    bool quiesce() noexcept override { return true; }

    void reset(std::uint64_t epoch, StreamingResetReason) noexcept override {
        last_stamp_ = {.epoch = epoch, .sequence = 0};
    }

    bool release() noexcept override { prepared_ = false; return true; }

    StreamingBlockStamp last_stamp() const noexcept { return last_stamp_; }

  private:
    StreamingModelSpec spec_;
    StreamingBlockStamp last_stamp_{};
    bool prepared_ = false;
};

} // namespace

TEST_CASE("streaming model contract classifies callback and worker methods",
          "[gpu_audio][streaming_model]") {
    CHECK(streaming_method_safety(StreamingModelMethod::ProcessCpu) ==
          pulp::audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
    CHECK(streaming_method_safety(StreamingModelMethod::Prepare) ==
          pulp::audio::RtSafetyClass::ControlThreadOnly);
    CHECK(streaming_method_safety(StreamingModelMethod::ServiceBackend) ==
          pulp::audio::RtSafetyClass::BackgroundThreadOnly);
}

TEST_CASE("streaming model spec fails closed before preparation",
          "[gpu_audio][streaming_model]") {
    IdentityModel model;
    auto spec = model.spec();
    CHECK(validate_streaming_model_spec(spec).accepted());
    spec.model_id = {};
    CHECK(validate_streaming_model_spec(spec).error == StreamingModelSpecError::MissingIdentity);
    spec = model.spec();
    spec.block_size = 0;
    CHECK(validate_streaming_model_spec(spec).error == StreamingModelSpecError::InvalidShape);
    spec = model.spec();
    spec.state_bytes = 32;
    CHECK(validate_streaming_model_spec(spec).error == StreamingModelSpecError::InvalidShape);
    const auto valid = model.spec();
    CHECK(!valid_streaming_prepare_context({.spec = &valid,
                                            .artifact_id = "id",
                                            .artifact_hash = "hash",
                                            .max_frames = 1}));
}

TEST_CASE("streaming model owns deterministic CPU state and explicit epochs",
          "[gpu_audio][streaming_model]") {
    IdentityModel model;
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "test.identity",
                                                 .artifact_hash = "test",
                                                 .fallback = StreamingFallbackStrategy::ContinuouslyPrimedCpuShadow,
                                                 .max_frames = 4,
                                                 .lead_blocks = 0,
                                                 .worker_backend_requested = false};
    CHECK(valid_streaming_prepare_context(context));
    REQUIRE(model.prepare(context));
    std::array<float, 4> input{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> output{};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 4);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 4);
    model.process_cpu(in, out, 4, {.epoch = 7, .sequence = 11});
    CHECK(output == input);
    CHECK(model.last_stamp() == StreamingBlockStamp{.epoch = 7, .sequence = 11});
    model.reset(8, StreamingResetReason::ModelSwap);
    CHECK(model.last_stamp() == StreamingBlockStamp{.epoch = 8, .sequence = 0});
    model.release();
}

TEST_CASE("streaming terminal carries the exact admitted stamp",
          "[gpu_audio][streaming_model]") {
    const StreamingTerminal terminal{.stamp = {.epoch = 3, .sequence = 19},
                                     .disposition = StreamingBackendTerminalDisposition::Stale};
    CHECK(terminal.stamp == StreamingBlockStamp{.epoch = 3, .sequence = 19});
    CHECK(terminal.disposition == StreamingBackendTerminalDisposition::Stale);
}
