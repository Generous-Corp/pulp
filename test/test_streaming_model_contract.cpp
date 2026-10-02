#include <catch2/catch_test_macros.hpp>

#include "detail/streaming_model.hpp"

#include <array>
#include <optional>

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

class EchoBackend final : public StreamingBackend {
  public:
    bool prepare(const StreamingPrepareContext& context) noexcept override {
        prepared_ = valid_streaming_prepare_context(context);
        epoch_ = prepared_ ? context.spec->sample_rate : 0;
        return prepared_;
    }

    StreamingAdmission enqueue(const StreamingBlock& block) noexcept override {
        if (!prepared_ || pending_ || block.stamp.epoch != epoch_)
            return StreamingAdmission::Rejected;
        pending_ = &block;
        return StreamingAdmission::Accepted;
    }

    std::size_t service_until(std::uint64_t) noexcept override {
        if (!pending_)
            return 0;
        for (std::size_t i = 0; i < pending_->input.size() && i < pending_->output.size(); ++i)
            pending_->output[i] = pending_->input[i];
        terminal_ = StreamingTerminal{.stamp = pending_->stamp,
                                      .disposition = StreamingBackendTerminalDisposition::Completed};
        pending_ = nullptr;
        return 1;
    }

    bool receive(StreamingTerminal& terminal) noexcept override {
        if (!terminal_)
            return false;
        terminal = *terminal_;
        terminal_.reset();
        return true;
    }

    bool begin_epoch(std::uint64_t epoch, StreamingResetReason) noexcept override {
        if (pending_) {
            terminal_ = StreamingTerminal{.stamp = pending_->stamp,
                                          .disposition = StreamingBackendTerminalDisposition::Stale};
            pending_ = nullptr;
        }
        epoch_ = epoch;
        return true;
    }

    bool reprepare_after_device_loss(std::uint64_t epoch) noexcept override {
        return begin_epoch(epoch, StreamingResetReason::DeviceLoss);
    }
    bool quiesce() noexcept override { pending_ = nullptr; return true; }
    bool release() noexcept override { prepared_ = false; pending_ = nullptr; return true; }

  private:
    bool prepared_ = false;
    std::uint64_t epoch_ = 0;
    const StreamingBlock* pending_ = nullptr;
    std::optional<StreamingTerminal> terminal_;
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

TEST_CASE("streaming backend carries audio leases and fences epochs",
          "[gpu_audio][streaming_model]") {
    IdentityModel model;
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "test.identity",
                                                 .artifact_hash = "test",
                                                 .max_frames = 4};
    EchoBackend backend;
    REQUIRE(backend.prepare(context));
    std::array<float, 4> input{1, 2, 3, 4};
    std::array<float, 4> output{};
    const StreamingBlock block{.stamp = {.epoch = 48000, .sequence = 9},
                               .input = input,
                               .output = output,
                               .channels = 1,
                               .frames = 4};
    CHECK(backend.enqueue(block) == StreamingAdmission::Accepted);
    CHECK(backend.service_until(0) == 1);
    StreamingTerminal terminal;
    REQUIRE(backend.receive(terminal));
    CHECK(terminal.stamp == block.stamp);
    CHECK(terminal.disposition == StreamingBackendTerminalDisposition::Completed);
    CHECK(output == input);
    CHECK(backend.begin_epoch(12, StreamingResetReason::ModelSwap));
    CHECK(!backend.receive(terminal));
    const StreamingBlock queued{.stamp = {.epoch = 12, .sequence = 10},
                                .input = input,
                                .output = output,
                                .channels = 1,
                                .frames = 4};
    CHECK(backend.enqueue(queued) == StreamingAdmission::Accepted);
    CHECK(backend.begin_epoch(13, StreamingResetReason::TransportRestart));
    REQUIRE(backend.receive(terminal));
    CHECK(terminal.stamp == queued.stamp);
    CHECK(terminal.disposition == StreamingBackendTerminalDisposition::Stale);
    CHECK(backend.enqueue(block) == StreamingAdmission::Rejected);
}

TEST_CASE("micro TCN carries causal convolution state across blocks",
          "[gpu_audio][streaming_model][tcn]") {
    using Model = MicroTcnModel<1, 3>;
    Model::Weights weights;
    weights.taps[0] = {1.0f, 0.5f, 0.25f};
    Model model(weights);
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "micro-tcn",
                                                 .artifact_hash = "embedded-test-weights",
                                                 .max_frames = 64};
    REQUIRE(model.prepare(context));

    std::array<float, 2> first_input{1.0f, 2.0f};
    std::array<float, 2> first_output{};
    const float* first_input_channels[] = {first_input.data()};
    float* first_output_channels[] = {first_output.data()};
    const auto first_in = pulp::audio::BufferView<const float>(first_input_channels, 1, 2);
    auto first_out = pulp::audio::BufferView<float>(first_output_channels, 1, 2);
    model.process_cpu(first_in, first_out, 2, {.epoch = 1, .sequence = 0});
    const std::array<float, 2> expected_first{1.0f, 2.5f};
    CHECK(first_output == expected_first);

    std::array<float, 2> second_input{3.0f, 4.0f};
    std::array<float, 2> second_output{};
    const float* second_input_channels[] = {second_input.data()};
    float* second_output_channels[] = {second_output.data()};
    const auto second_in = pulp::audio::BufferView<const float>(second_input_channels, 1, 2);
    auto second_out = pulp::audio::BufferView<float>(second_output_channels, 1, 2);
    model.process_cpu(second_in, second_out, 2, {.epoch = 1, .sequence = 1});
    const std::array<float, 2> expected_second{4.25f, 6.0f};
    CHECK(second_output == expected_second);

    model.reset(2, StreamingResetReason::TransportRestart);
    std::array<float, 1> reset_input{3.0f};
    std::array<float, 1> reset_output{};
    const float* reset_input_channels[] = {reset_input.data()};
    float* reset_output_channels[] = {reset_output.data()};
    const auto reset_in = pulp::audio::BufferView<const float>(reset_input_channels, 1, 1);
    auto reset_out = pulp::audio::BufferView<float>(reset_output_channels, 1, 1);
    model.process_cpu(reset_in, reset_out, 1, {.epoch = 2, .sequence = 0});
    CHECK(reset_output[0] == 3.0f);
}

TEST_CASE("micro TCN clears output for incompatible audio shape",
          "[gpu_audio][streaming_model][tcn]") {
    MicroTcnModel<2, 2> model;
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "micro-tcn",
                                                 .artifact_hash = "embedded-test-weights",
                                                 .max_frames = 64};
    REQUIRE(model.prepare(context));

    std::array<float, 1> input{1.0f};
    std::array<float, 1> output{9.0f};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 1);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 1);
    model.process_cpu(in, out, 1, {.epoch = 1, .sequence = 0});
    CHECK(output[0] == 0.0f);
}
