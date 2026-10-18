#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "detail/tcn_cpu_adapter.hpp"
#include "harness/rt_contract_probe.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <array>
#include <cmath>

using namespace pulp::gpu_audio::detail;

namespace {
struct Kernel {
    float history = 0.0f;
    int releases = 0;
    bool fail_once = false;
    bool plant_faults = false;
};
void planted_rt_faults() noexcept {
    pulp::test::rt_contract_probe_record_allocation(7);
    pulp::test::rt_contract_probe_record_lock();
    pulp::test::rt_contract_probe_record_blocking();
}
bool prepare(void* p, const StreamingPrepareContext&) noexcept {
    auto& k = *static_cast<Kernel*>(p);
    if (k.fail_once) {
        k.fail_once = false;
        return false;
    }
    k.history = 0.0f;
    return true;
}
void process(void* p, const float* in, float* out, std::uint32_t frames) noexcept {
    auto& k = *static_cast<Kernel*>(p);
    if (k.plant_faults)
        planted_rt_faults();
    for (std::uint32_t i = 0; i < frames; ++i) {
        out[i] = in[i] + 0.5f * k.history;
        k.history = in[i];
    }
}
void reset(void* p) noexcept {
    static_cast<Kernel*>(p)->history = 0.0f;
}
bool quiesce(void*) noexcept {
    return true;
}
bool release(void* p) noexcept {
    ++static_cast<Kernel*>(p)->releases;
    return true;
}

TcnCpuShape shape() {
    return {.channels = 1, .kernel_size = 3, .dilation = 2, .layers = 1};
}
StreamingModelSpec spec() {
    return {.model_id = "fixture.tcn",
            .architecture = "causal-tcn",
            .model_version = "fixture-v1",
            .weights_hash = "sha256:fixture-weights",
            .runtime_hash = "sha256:private-tcn-runtime",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = 48000,
            .block_size = 32,
            .feature_rate = 48000,
            .state_bytes = tcn_cpu_state_bytes(shape()),
            .state_schema = "causal-tcn-v1",
            .state_schema_version = 1};
}
TcnCpuAdapter make(Kernel& k, StreamingModelSpec& s) {
    return {s,
            shape(),
            {"fixture.tcn", "sha256:fixture-weights", "MIT", "causal-tcn-v1", 1},
            {&k, prepare, process, reset, quiesce, release}};
}
} // namespace

TEST_CASE("causal TCN CPU adapter preserves state across blocks and reset replay",
          "[gpu_audio][neural][tcn][realtime]") {
    auto s = spec();
    Kernel k;
    auto adapter = make(k, s);
    const StreamingPrepareContext c{&adapter.spec(), "fixture.tcn", "sha256:fixture-weights",
                                    StreamingFallbackStrategy::NoFallback, 32};
    REQUIRE(adapter.prepare(c));
    std::array<float, 3> in{1, 2, 3}, out{}, expected{1, 2.5f, 4};
    const float* ip[] = {in.data()};
    float* op[] = {out.data()};
    auto iv = pulp::audio::BufferView<const float>(ip, 1, 3);
    auto ov = pulp::audio::BufferView<float>(op, 1, 3);
    pulp::test::RtContractProbe planted;
    k.plant_faults = true;
    {
        pulp::test::ScopedRtProcessProbe rt_probe;
        adapter.process_cpu(iv, ov, 3, {});
        CHECK(rt_probe.allocation_count() == 0);
    }
    k.plant_faults = false;
    CHECK(planted.allocation_count() == 1);
    CHECK(planted.lock_events() == 1);
    CHECK(planted.blocking_events() == 1);
    CHECK(out == expected);
    adapter.reset(2, StreamingResetReason::TransportRestart);
    out.fill(0);
    adapter.process_cpu(iv, ov, 3, {});
    CHECK(out == expected);
    REQUIRE(adapter.quiesce());
    REQUIRE(adapter.release());
    CHECK(adapter.release());
}

TEST_CASE("causal TCN CPU adapter rejects identity, layout, and alias violations",
          "[gpu_audio][neural][tcn][lifecycle]") {
    auto s = spec();
    Kernel k{.fail_once = true};
    auto adapter = make(k, s);
    const StreamingPrepareContext c{&adapter.spec(), "fixture.tcn", "sha256:fixture-weights",
                                    StreamingFallbackStrategy::NoFallback, 32};
    auto wrong = c;
    wrong.artifact_id = "other";
    CHECK_FALSE(adapter.prepare(wrong));
    CHECK(adapter.last_prepare_error() == TcnCpuPrepareError::ArtifactMismatch);
    auto oversized = c;
    oversized.max_frames = 4097;
    CHECK_FALSE(adapter.prepare(oversized));
    CHECK(adapter.last_prepare_error() == TcnCpuPrepareError::InvalidContract);
    CHECK_FALSE(adapter.prepare(c));
    CHECK(k.releases == 1);
    REQUIRE(adapter.prepare(c));
    std::array<float, 2> samples{1, 2};
    const float* ip[] = {samples.data()};
    float* op[] = {samples.data()};
    auto iv = pulp::audio::BufferView<const float>(ip, 1, 2);
    auto ov = pulp::audio::BufferView<float>(op, 1, 2);
    adapter.process_cpu(iv, ov, 2, {});
    CHECK(samples == std::array<float, 2>{0, 0});
    REQUIRE(adapter.release());
    auto malformed = shape();
    malformed.dilation = 0;
    CHECK_FALSE(valid_tcn_cpu_shape(malformed));
    CHECK(tcn_cpu_state_bytes(malformed) == 0);
}
