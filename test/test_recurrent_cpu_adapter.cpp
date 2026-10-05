#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "detail/neural_processor.hpp"
#include "detail/recurrent_cpu_adapter.hpp"
#include "harness/rt_contract_probe.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

using namespace pulp::gpu_audio::detail;

namespace {

constexpr std::size_t kHidden = 2;

float sigmoid(float value) noexcept {
    return 1.0f / (1.0f + std::exp(-value));
}

struct SyntheticKernel {
    RecurrentFamily family;
    std::array<float, kHidden> hidden{};
    std::array<float, kHidden> cell{};
    std::array<float, 4 * kHidden> scratch{};
    bool prepared = false;
    bool fail_once = false;
    bool fail_release_once = false;
    std::size_t prepare_calls = 0;
    std::size_t reset_calls = 0;
    std::size_t release_calls = 0;

    void reset_state() noexcept {
        hidden.fill(0.0f);
        cell.fill(0.0f);
        scratch.fill(0.0f);
    }
};

bool prepare_kernel(void* opaque, const StreamingPrepareContext&) noexcept {
    auto& kernel = *static_cast<SyntheticKernel*>(opaque);
    ++kernel.prepare_calls;
    if (kernel.fail_once) {
        kernel.fail_once = false;
        kernel.prepared = false;
        return false;
    }
    kernel.reset_state();
    kernel.prepared = true;
    return true;
}

void process_kernel(void* opaque, const float* input, float* output, std::uint32_t frames) noexcept {
    auto& kernel = *static_cast<SyntheticKernel*>(opaque);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const float x = input[frame];
        if (kernel.family == RecurrentFamily::Lstm) {
            for (std::size_t i = 0; i < kHidden; ++i) {
                const float h = kernel.hidden[i];
                kernel.scratch[i] = sigmoid(0.4f * x + 0.1f * h + 0.2f);
                kernel.scratch[kHidden + i] = sigmoid(-0.2f * x + 0.3f * h + 0.1f);
                kernel.scratch[2 * kHidden + i] = std::tanh(0.3f * x + 0.2f * h - 0.1f);
                kernel.scratch[3 * kHidden + i] = sigmoid(0.1f * x + 0.4f * h);
                kernel.cell[i] = kernel.scratch[kHidden + i] * kernel.cell[i] +
                                 kernel.scratch[i] * kernel.scratch[2 * kHidden + i];
                kernel.hidden[i] = kernel.scratch[3 * kHidden + i] * std::tanh(kernel.cell[i]);
            }
        } else {
            for (std::size_t i = 0; i < kHidden; ++i) {
                const float h = kernel.hidden[i];
                const float z = sigmoid(0.2f * x + 0.1f * h + 0.1f);
                const float r = sigmoid(-0.3f * x + 0.2f * h);
                const float n = std::tanh(0.4f * x + 0.3f * r * h + 0.05f);
                kernel.hidden[i] = (1.0f - z) * n + z * h;
                kernel.scratch[i] = z;
                kernel.scratch[kHidden + i] = r;
                kernel.scratch[2 * kHidden + i] = n;
            }
        }
        output[frame] = kernel.family == RecurrentFamily::Lstm
                             ? 0.7f * kernel.hidden[0] - 0.2f * kernel.hidden[1] + 0.05f
                             : 0.6f * kernel.hidden[0] + 0.1f * kernel.hidden[1] - 0.03f;
    }
}

void reset_kernel(void* opaque) noexcept {
    auto& kernel = *static_cast<SyntheticKernel*>(opaque);
    ++kernel.reset_calls;
    kernel.reset_state();
}

bool quiesce_kernel(void*) noexcept {
    return true;
}

bool release_kernel(void* opaque) noexcept {
    auto& kernel = *static_cast<SyntheticKernel*>(opaque);
    ++kernel.release_calls;
    if (kernel.fail_release_once) {
        kernel.fail_release_once = false;
        return false;
    }
    kernel.prepared = false;
    kernel.reset_state();
    return true;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::byte* planted_allocation() {
    return new std::byte[17];
}

StreamingModelSpec make_spec(RecurrentFamily family, std::uint32_t sample_rate = 48000,
                             std::uint32_t block_size = 32) {
    const RecurrentCpuShape shape{.family = family,
                                  .input_size = 1,
                                  .hidden_size = kHidden,
                                  .layers = 1,
                                  .directions = 1,
                                  .output_size = 1};
    return {.model_id = family == RecurrentFamily::Lstm ? "synthetic.lstm" : "synthetic.gru",
            .architecture = family == RecurrentFamily::Lstm ? "recurrent.lstm" : "recurrent.gru",
            .model_version = "synthetic-v1",
            .weights_hash = "synthetic-weights",
            .runtime_hash = "private-recurrent-cpu-v1",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = sample_rate,
            .block_size = block_size,
            .feature_rate = sample_rate,
            .intrinsic_latency_samples = 0,
            .receptive_field_samples = 1,
            .state_bytes = recurrent_state_bytes(shape),
            .state_schema = recurrent_state_schema(family),
            .state_schema_version = 1,
            .deterministic = true};
}

RecurrentCpuShape make_shape(RecurrentFamily family) {
    return {.family = family,
            .input_size = 1,
            .hidden_size = kHidden,
            .layers = 1,
            .directions = 1,
            .output_size = 1};
}

RecurrentCpuKernel make_kernel(SyntheticKernel& kernel) {
    return {.state = &kernel,
            .prepare = prepare_kernel,
            .process = process_kernel,
            .reset = reset_kernel,
            .quiesce = quiesce_kernel,
            .release = release_kernel};
}

struct OracleState {
    std::array<float, kHidden> hidden{};
    std::array<float, kHidden> cell{};
};

float oracle_sample(RecurrentFamily family, OracleState& state, float x) {
    if (family == RecurrentFamily::Lstm) {
        for (std::size_t i = 0; i < kHidden; ++i) {
            const float h = state.hidden[i];
            const float input_gate = sigmoid(0.4f * x + 0.1f * h + 0.2f);
            const float forget_gate = sigmoid(-0.2f * x + 0.3f * h + 0.1f);
            const float candidate = std::tanh(0.3f * x + 0.2f * h - 0.1f);
            const float output_gate = sigmoid(0.1f * x + 0.4f * h);
            state.cell[i] = forget_gate * state.cell[i] + input_gate * candidate;
            state.hidden[i] = output_gate * std::tanh(state.cell[i]);
        }
        return 0.7f * state.hidden[0] - 0.2f * state.hidden[1] + 0.05f;
    }
    for (std::size_t i = 0; i < kHidden; ++i) {
        const float h = state.hidden[i];
        const float z = sigmoid(0.2f * x + 0.1f * h + 0.1f);
        const float r = sigmoid(-0.3f * x + 0.2f * h);
        const float n = std::tanh(0.4f * x + 0.3f * r * h + 0.05f);
        state.hidden[i] = (1.0f - z) * n + z * h;
    }
    return 0.6f * state.hidden[0] + 0.1f * state.hidden[1] - 0.03f;
}

std::array<float, 128> input_fixture() {
    std::array<float, 128> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(static_cast<float>(i) * 0.071f) +
                   0.25f * std::cos(static_cast<float>(i) * 0.013f);
    return input;
}

void require_oracle(RecurrentFamily family) {
    const auto spec = make_spec(family);
    SyntheticKernel kernel{.family = family};
    RecurrentCpuAdapter adapter(spec, make_shape(family), make_kernel(kernel));
    const auto context = StreamingPrepareContext{.spec = &adapter.spec(),
                                                 .artifact_id = "synthetic-recurrent",
                                                 .artifact_hash = "synthetic-weights",
                                                 .max_frames = 128};
    REQUIRE(adapter.prepare(context));

    const auto input = input_fixture();
    std::array<float, 128> actual{};
    OracleState oracle;
    std::array<float, 128> expected{};
    for (std::size_t i = 0; i < input.size(); ++i)
        expected[i] = oracle_sample(family, oracle, input[i]);

    std::size_t offset = 0;
    for (const auto frames : {std::uint32_t{32}, std::uint32_t{64}, std::uint32_t{32}}) {
        const float* input_channels[] = {input.data() + offset};
        float* output_channels[] = {actual.data() + offset};
        const auto in = pulp::audio::BufferView<const float>(input_channels, 1, frames);
        auto out = pulp::audio::BufferView<float>(output_channels, 1, frames);
        adapter.process_cpu(in, out, frames, {.epoch = 1, .sequence = offset});
        offset += frames;
    }
    float max_error = 0.0f;
    for (std::size_t i = 0; i < actual.size(); ++i)
        max_error = std::max(max_error, std::abs(actual[i] - expected[i]));
    CHECK(max_error <= 1.0e-6f);

    adapter.reset(2, StreamingResetReason::TransportRestart);
    actual.fill(0.0f);
    const float* replay_input_channels[] = {input.data()};
    float* replay_output_channels[] = {actual.data()};
    const auto replay_in = pulp::audio::BufferView<const float>(replay_input_channels, 1, 128);
    auto replay_out = pulp::audio::BufferView<float>(replay_output_channels, 1, 128);
    adapter.process_cpu(replay_in, replay_out, 128, {.epoch = 2, .sequence = 0});
    for (std::size_t i = 0; i < actual.size(); ++i)
        CHECK(actual[i] == Catch::Approx(expected[i]).margin(1.0e-6f));
    REQUIRE(adapter.quiesce());
    REQUIRE(adapter.release());
}

} // namespace

TEST_CASE("recurrent CPU adapter LSTM callback safety and oracle parity",
          "[gpu_audio][neural][recurrent][lstm][realtime]") {
    require_oracle(RecurrentFamily::Lstm);
}

TEST_CASE("recurrent CPU adapter GRU callback safety and oracle parity",
          "[gpu_audio][neural][recurrent][gru][realtime]") {
    require_oracle(RecurrentFamily::Gru);
}

TEST_CASE("recurrent CPU adapter retries failed preparation and releases idempotently",
          "[gpu_audio][neural][recurrent][lifecycle]") {
    const auto spec = make_spec(RecurrentFamily::Lstm);
    SyntheticKernel kernel{.family = RecurrentFamily::Lstm, .fail_once = true};
    RecurrentCpuAdapter adapter(spec, make_shape(RecurrentFamily::Lstm), make_kernel(kernel));
    const auto context = StreamingPrepareContext{.spec = &adapter.spec(),
                                                 .artifact_id = "synthetic-recurrent",
                                                 .artifact_hash = "synthetic-weights",
                                                 .max_frames = 64};
    CHECK_FALSE(adapter.prepare(context));
    CHECK(kernel.release_calls == 1);
    REQUIRE(adapter.prepare(context));
    REQUIRE(adapter.quiesce());
    REQUIRE(adapter.release());
    CHECK(adapter.release());
}

TEST_CASE("recurrent CPU adapter keeps active state across failed replacement and changed reprepare",
          "[gpu_audio][neural][recurrent][lifecycle]") {
    const auto old_spec = make_spec(RecurrentFamily::Lstm, 48000);
    SyntheticKernel old_kernel{.family = RecurrentFamily::Lstm};
    RecurrentCpuAdapter old_adapter(old_spec, make_shape(RecurrentFamily::Lstm), make_kernel(old_kernel));
    const auto old_context = StreamingPrepareContext{.spec = &old_adapter.spec(),
                                                      .artifact_id = "synthetic-recurrent-old",
                                                      .artifact_hash = "synthetic-weights-old",
                                                      .max_frames = 32};
    REQUIRE(old_adapter.prepare(old_context));
    const auto prepare_calls = old_kernel.prepare_calls;

    auto malformed_context = old_context;
    malformed_context.artifact_hash = {};
    CHECK_FALSE(old_adapter.prepare(malformed_context));
    CHECK(old_kernel.prepared);
    CHECK(old_kernel.prepare_calls == prepare_calls);

    std::array<float, 1> input{1.0f};
    std::array<float, 1> output{};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 1);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 1);
    old_adapter.process_cpu(in, out, 1, {.epoch = 1, .sequence = 0});
    CHECK(output[0] != 0.0f);

    const auto new_spec = make_spec(RecurrentFamily::Lstm, 96000, 64);
    SyntheticKernel new_kernel{.family = RecurrentFamily::Lstm};
    RecurrentCpuAdapter new_adapter(new_spec, make_shape(RecurrentFamily::Lstm), make_kernel(new_kernel));
    const auto new_context = StreamingPrepareContext{.spec = &new_adapter.spec(),
                                                      .artifact_id = "synthetic-recurrent-new",
                                                      .artifact_hash = "synthetic-weights-new",
                                                      .max_frames = 64};
    CHECK_FALSE(old_adapter.prepare(new_context));
    CHECK(old_kernel.prepared);
    REQUIRE(old_adapter.quiesce());
    REQUIRE(old_adapter.release());
    CHECK_FALSE(old_kernel.prepared);
    REQUIRE(new_adapter.prepare(new_context));
    CHECK(new_kernel.prepared);
    CHECK_FALSE(old_kernel.prepared);
    REQUIRE(new_adapter.release());
}

TEST_CASE("recurrent CPU adapter release preserves state for a retry",
          "[gpu_audio][neural][recurrent][lifecycle]") {
    const auto spec = make_spec(RecurrentFamily::Gru);
    SyntheticKernel kernel{.family = RecurrentFamily::Gru, .fail_release_once = true};
    RecurrentCpuAdapter adapter(spec, make_shape(RecurrentFamily::Gru), make_kernel(kernel));
    const auto context = StreamingPrepareContext{.spec = &adapter.spec(),
                                                 .artifact_id = "synthetic-recurrent",
                                                 .artifact_hash = "synthetic-weights",
                                                 .max_frames = 32};
    REQUIRE(adapter.prepare(context));
    CHECK_FALSE(adapter.release());
    CHECK(kernel.prepared);

    std::array<float, 1> input{1.0f};
    std::array<float, 1> output{};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 1);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 1);
    adapter.process_cpu(in, out, 1, {.epoch = 1, .sequence = 0});
    CHECK(kernel.hidden != std::array<float, kHidden>{});
    REQUIRE(adapter.release());
    CHECK_FALSE(kernel.prepared);
    CHECK(adapter.release());
}

TEST_CASE("recurrent CPU adapter reset and NeuralProcessor reprepare lifecycle",
          "[gpu_audio][neural][recurrent][lifecycle]") {
    const auto spec = make_spec(RecurrentFamily::Gru);
    SyntheticKernel kernel{.family = RecurrentFamily::Gru};
    RecurrentCpuAdapter adapter(spec, make_shape(RecurrentFamily::Gru), make_kernel(kernel));
    NeuralProcessor processor(adapter);
    const auto context = StreamingPrepareContext{.spec = &adapter.spec(),
                                                 .artifact_id = "synthetic-recurrent",
                                                 .artifact_hash = "synthetic-weights",
                                                 .max_frames = 64};
    REQUIRE(processor.prepare(context));
    REQUIRE(processor.publish());
    CHECK(processor.snapshot().generation == 1);
    REQUIRE(processor.reset(StreamingResetReason::SampleRateChange));
    CHECK(processor.snapshot().generation == 2);
    REQUIRE(processor.release());
    CHECK_FALSE(processor.snapshot().prepared);
    REQUIRE(processor.prepare(context));
    REQUIRE(processor.publish());
    CHECK(processor.snapshot().generation == 3);
    REQUIRE(processor.release());
}

TEST_CASE("recurrent CPU adapter rejects noncausal and malformed shapes",
          "[gpu_audio][neural][recurrent][validation]") {
    const auto spec = make_spec(RecurrentFamily::Lstm);
    SyntheticKernel kernel{.family = RecurrentFamily::Lstm};
    const auto context_for = [&](RecurrentCpuAdapter& adapter) {
        return StreamingPrepareContext{.spec = &adapter.spec(),
                                       .artifact_id = "synthetic-recurrent",
                                       .artifact_hash = "synthetic-weights",
                                       .max_frames = 64};
    };

    auto bidirectional = make_shape(RecurrentFamily::Lstm);
    bidirectional.directions = 2;
    RecurrentCpuAdapter noncausal(spec, bidirectional, make_kernel(kernel));
    CHECK_FALSE(noncausal.prepare(context_for(noncausal)));

    auto too_large = make_shape(RecurrentFamily::Lstm);
    too_large.hidden_size = 257;
    RecurrentCpuAdapter oversized(spec, too_large, make_kernel(kernel));
    CHECK_FALSE(oversized.prepare(context_for(oversized)));

    auto wrong_state = spec;
    wrong_state.state_bytes -= sizeof(float);
    RecurrentCpuAdapter malformed(wrong_state, make_shape(RecurrentFamily::Lstm), make_kernel(kernel));
    CHECK_FALSE(malformed.prepare(context_for(malformed)));
}

TEST_CASE("recurrent CPU adapter rejects alias, partial overlap, and oversized buffers without advancing state",
          "[gpu_audio][neural][recurrent][validation]") {
    const auto spec = make_spec(RecurrentFamily::Gru);
    SyntheticKernel kernel{.family = RecurrentFamily::Gru};
    RecurrentCpuAdapter adapter(spec, make_shape(RecurrentFamily::Gru), make_kernel(kernel));
    const auto context = StreamingPrepareContext{.spec = &adapter.spec(),
                                                 .artifact_id = "synthetic-recurrent",
                                                 .artifact_hash = "synthetic-weights",
                                                 .max_frames = 32};
    REQUIRE(adapter.prepare(context));

    std::array<float, 32> samples{};
    const float* alias_input[] = {samples.data()};
    float* alias_output[] = {samples.data()};
    const auto alias_in = pulp::audio::BufferView<const float>(alias_input, 1, 32);
    auto alias_out = pulp::audio::BufferView<float>(alias_output, 1, 32);
    adapter.process_cpu(alias_in, alias_out, 32, {.epoch = 1, .sequence = 0});
    CHECK(kernel.hidden == std::array<float, kHidden>{});

    std::array<float, 33> partially_overlapped{};
    const float* partial_input[] = {partially_overlapped.data()};
    float* partial_output[] = {partially_overlapped.data() + 1};
    const auto partial_in = pulp::audio::BufferView<const float>(partial_input, 1, 32);
    auto partial_out = pulp::audio::BufferView<float>(partial_output, 1, 32);
    adapter.process_cpu(partial_in, partial_out, 32, {.epoch = 1, .sequence = 1});
    CHECK(kernel.hidden == std::array<float, kHidden>{});
    CHECK(kernel.cell == std::array<float, kHidden>{});

    std::array<float, 33> input{};
    std::array<float, 33> output;
    output.fill(9.0f);
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 33);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 33);
    adapter.process_cpu(in, out, 33, {.epoch = 1, .sequence = 1});
    CHECK(kernel.hidden == std::array<float, kHidden>{});
    CHECK(output == std::array<float, 33>{});
    REQUIRE(adapter.release());
}

TEST_CASE("recurrent CPU adapter process has no realtime allocations",
          "[gpu_audio][neural][recurrent][realtime]") {
    const auto spec = make_spec(RecurrentFamily::Lstm);
    SyntheticKernel kernel{.family = RecurrentFamily::Lstm};
    RecurrentCpuAdapter adapter(spec, make_shape(RecurrentFamily::Lstm), make_kernel(kernel));
    const auto context = StreamingPrepareContext{.spec = &adapter.spec(),
                                                 .artifact_id = "synthetic-recurrent",
                                                 .artifact_hash = "synthetic-weights",
                                                 .max_frames = 64};
    REQUIRE(adapter.prepare(context));
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    const auto in = pulp::audio::BufferView<const float>(input_channels, 1, 64);
    auto out = pulp::audio::BufferView<float>(output_channels, 1, 64);
    {
        pulp::test::RtContractProbe events;
        pulp::test::ScopedRtProcessProbe probe;
        adapter.process_cpu(in, out, 64, {.epoch = 1, .sequence = 0});
        CHECK(probe.allocation_count() == 0);
        CHECK(events.allocation_count() == 0);
        CHECK(events.allocated_bytes() == 0);
        CHECK(events.lock_events() == 0);
        CHECK(events.blocking_events() == 0);
    }
    REQUIRE(adapter.release());
}

TEST_CASE("recurrent CPU adapter RT probe catches planted negatives",
          "[gpu_audio][neural][recurrent][realtime][negative]") {
    pulp::test::RtContractProbe events;
    auto* planted = planted_allocation();
    pulp::test::rt_contract_probe_record_allocation(17);
    delete[] planted;
    pulp::test::rt_contract_probe_record_lock();
    pulp::test::rt_contract_probe_record_blocking();
    CHECK(events.allocation_count() == 1);
    CHECK(events.allocated_bytes() == 17);
    CHECK(events.lock_events() == 1);
    CHECK(events.blocking_events() == 1);
}
