#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "detail/compact_ssm_cpu_adapter.hpp"
#include "harness/rt_contract_probe.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <array>
#include <cmath>

using namespace pulp::gpu_audio::detail;

namespace {
struct Kernel {
    std::array<float, 2> state{};
    bool prepared = false;
    bool fail_once = false;
    bool plant_faults = false;
    int releases = 0;
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
    k.state.fill(0.0f);
    k.prepared = true;
    return true;
}
void process(void* p, const float* in, float* out, std::uint32_t frames) noexcept {
    auto& k = *static_cast<Kernel*>(p);
    if (k.plant_faults)
        planted_rt_faults();
    for (std::uint32_t i = 0; i < frames; ++i) {
        k.state[0] = 0.8f * k.state[0] + 0.2f * in[i];
        k.state[1] = 0.6f * k.state[1] + 0.1f * in[i];
        out[i] = 0.7f * k.state[0] - 0.2f * k.state[1];
    }
}
void reset(void* p) noexcept {
    static_cast<Kernel*>(p)->state.fill(0.0f);
}
bool quiesce(void*) noexcept {
    return true;
}
bool release(void* p) noexcept {
    auto& k = *static_cast<Kernel*>(p);
    ++k.releases;
    k.prepared = false;
    return true;
}

CompactSsmCpuShape shape() {
    return {.state_size = 2, .input_size = 1, .output_size = 1, .layers = 1};
}
StreamingModelSpec spec() {
    return {.model_id = "fixture.compact-ssm",
            .architecture = "compact-ssm-diagonal",
            .model_version = "fixture-v1",
            .weights_hash = "sha256:fixture-weights",
            .runtime_hash = "sha256:private-ssm-runtime",
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = 48000,
            .block_size = 32,
            .feature_rate = 48000,
            .state_bytes = compact_ssm_cpu_state_bytes(shape()),
            .state_schema = "compact-ssm-diagonal-v1",
            .state_schema_version = 1};
}
CompactSsmCpuAdapter make(Kernel& k, StreamingModelSpec& s) {
    return {s,
            shape(),
            {"fixture.compact-ssm", "sha256:fixture-weights", "MIT", "compact-ssm-diagonal-v1", 1},
            {&k, prepare, process, reset, quiesce, release}};
}
} // namespace

TEST_CASE("compact SSM CPU adapter matches independent scalar oracle and reset replay",
          "[gpu_audio][neural][ssm][realtime]") {
    auto s = spec();
    Kernel k;
    auto adapter = make(k, s);
    const StreamingPrepareContext c{&adapter.spec(), "fixture.compact-ssm",
                                    "sha256:fixture-weights", StreamingFallbackStrategy::NoFallback,
                                    64};
    REQUIRE(adapter.prepare(c));
    std::array<float, 64> in{}, out{}, expected{};
    float a = 0, b = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = std::sin(float(i) * .17f);
        a = .8f * a + .2f * in[i];
        b = .6f * b + .1f * in[i];
        expected[i] = .7f * a - .2f * b;
    }
    const float* ip[] = {in.data()};
    float* op[] = {out.data()};
    auto iv = pulp::audio::BufferView<const float>(ip, 1, 64);
    auto ov = pulp::audio::BufferView<float>(op, 1, 64);
    pulp::test::RtContractProbe planted;
    k.plant_faults = true;
    {
        pulp::test::ScopedRtProcessProbe rt_probe;
        adapter.process_cpu(iv, ov, 64, {});
        CHECK(rt_probe.allocation_count() == 0);
    }
    k.plant_faults = false;
    CHECK(planted.allocation_count() == 1);
    CHECK(planted.lock_events() == 1);
    CHECK(planted.blocking_events() == 1);
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == Catch::Approx(expected[i]).margin(1e-6f));
    adapter.reset(2, StreamingResetReason::TransportRestart);
    out.fill(0);
    adapter.process_cpu(iv, ov, 64, {});
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == Catch::Approx(expected[i]).margin(1e-6f));
    REQUIRE(adapter.quiesce());
    REQUIRE(adapter.release());
    CHECK(adapter.release());
}

TEST_CASE(
    "compact SSM CPU adapter fails closed on identity, shape, alias, and RT lifecycle controls",
    "[gpu_audio][neural][ssm][lifecycle]") {
    auto s = spec();
    Kernel k{.fail_once = true};
    auto adapter = make(k, s);
    const StreamingPrepareContext c{&adapter.spec(), "fixture.compact-ssm",
                                    "sha256:fixture-weights", StreamingFallbackStrategy::NoFallback,
                                    64};
    auto wrong_id = c;
    wrong_id.artifact_id = "fixture.other";
    CHECK_FALSE(adapter.prepare(wrong_id));
    CHECK(adapter.last_prepare_error() == CompactSsmCpuPrepareError::ArtifactMismatch);
    auto wrong_hash = c;
    wrong_hash.artifact_hash = "sha256:other";
    CHECK_FALSE(adapter.prepare(wrong_hash));
    CHECK(adapter.last_prepare_error() == CompactSsmCpuPrepareError::ArtifactMismatch);
    auto oversized = c;
    oversized.max_frames = 4097;
    CHECK_FALSE(adapter.prepare(oversized));
    CHECK(adapter.last_prepare_error() == CompactSsmCpuPrepareError::InvalidContract);
    auto invalid_shape = shape();
    invalid_shape.layers = 2;
    CompactSsmCpuAdapter malformed(
        s, invalid_shape,
        {"fixture.compact-ssm", "sha256:fixture-weights", "MIT", "compact-ssm-diagonal-v1", 1},
        {&k, prepare, process, reset, quiesce, release});
    const StreamingPrepareContext malformed_context{&malformed.spec(), "fixture.compact-ssm",
                                                    "sha256:fixture-weights",
                                                    StreamingFallbackStrategy::NoFallback, 64};
    CHECK_FALSE(malformed.prepare(malformed_context));
    CHECK(malformed.last_prepare_error() == CompactSsmCpuPrepareError::InvalidContract);
    CHECK_FALSE(adapter.prepare(c));
    CHECK(k.releases == 1);
    REQUIRE(adapter.prepare(c));
    std::array<float, 4> samples{1, 2, 3, 4};
    const float* ip[] = {samples.data()};
    float* op[] = {samples.data()};
    auto iv = pulp::audio::BufferView<const float>(ip, 1, 4);
    auto ov = pulp::audio::BufferView<float>(op, 1, 4);
    adapter.process_cpu(iv, ov, 4, {});
    CHECK(samples == std::array<float, 4>{0, 0, 0, 0});
    REQUIRE(adapter.quiesce());
    REQUIRE(adapter.release());
    auto bad = shape();
    bad.layers = 2;
    CHECK_FALSE(valid_compact_ssm_cpu_shape(bad));
}
