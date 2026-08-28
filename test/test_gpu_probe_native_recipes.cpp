#include <catch2/catch_test_macros.hpp>

#include <pulp_tooling/gpu_probe/recipes.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace probe = pulp::tooling::gpu_probe;

namespace {

constexpr const char* kEvidenceId = "00112233445566778899aabbccddeeff";

void require_valid(const probe::RecipeRun& run) {
    std::string error;
    INFO(error);
    REQUIRE(probe::validate(run, &error));
    REQUIRE(run.result.gpu_evidence_id == kEvidenceId);
    REQUIRE(run.result.artifacts.size() == run.payloads.size());
    std::set<std::string> names;
    for (const auto& payload : run.payloads) {
        REQUIRE(payload.artifact.bytes == payload.bytes.size());
        REQUIRE(names.insert(payload.artifact.name).second);
    }
}

void require_work_or_skip(const probe::RecipeRun& run) {
    if (run.result.verdict != probe::Verdict::unavailable) return;
#if defined(PULP_GPU_PROBE_REQUIRE_WORK)
    FAIL("required real-adapter recipe was unavailable");
#else
    SKIP("real-adapter recipe unavailable; required acceptance target rejects this state");
#endif
}

const probe::ArtifactPayload& payload_named(const probe::RecipeRun& run,
                                            std::string_view name) {
    const auto found = std::find_if(run.payloads.begin(), run.payloads.end(),
                                    [&](const auto& payload) {
                                        return payload.artifact.name == name;
                                    });
    REQUIRE(found != run.payloads.end());
    return *found;
}

float little_endian_float(const probe::ArtifactPayload& payload, std::size_t index) {
    REQUIRE((index + 1) * sizeof(float) <= payload.bytes.size());
    const auto at = index * sizeof(float);
    const std::uint32_t bits = static_cast<std::uint32_t>(payload.bytes[at]) |
        (static_cast<std::uint32_t>(payload.bytes[at + 1]) << 8u) |
        (static_cast<std::uint32_t>(payload.bytes[at + 2]) << 16u) |
        (static_cast<std::uint32_t>(payload.bytes[at + 3]) << 24u);
    return std::bit_cast<float>(bits);
}

} // namespace

TEST_CASE("Renderer3D recipe produces bounded typed evidence", "[gpu][gpu-probe]") {
    const auto run = probe::run_renderer3d_recipe({false, kEvidenceId});
    require_valid(run);
    require_work_or_skip(run);
    REQUIRE(run.result.verdict == probe::Verdict::pass);
    REQUIRE_FALSE(run.payloads.empty());
}

TEST_CASE("Renderer3D planted framebuffer regression reaches GPU readback",
          "[gpu][gpu-probe][mutation]") {
    const auto run = probe::run_renderer3d_recipe({true, kEvidenceId});
    require_valid(run);
    require_work_or_skip(run);
    REQUIRE(run.result.verdict == probe::Verdict::fail);
    REQUIRE(run.result.mutation == "pre-submit-framebuffer-downscale");
    CHECK(run.result.dimensions.width == 32);
    CHECK(run.result.dimensions.height == 32);
    CHECK(run.result.dimensions.work_items == 1'024);
    REQUIRE(run.result.passes[0].verdict == probe::Verdict::pass);
    REQUIRE(run.result.passes[1].verdict == probe::Verdict::pass);
    REQUIRE(run.result.passes[2].verdict == probe::Verdict::pass);
    REQUIRE(run.result.passes[3].verdict == probe::Verdict::fail);
    REQUIRE(run.result.passes[3].code == "portable_structure_mismatch");
    REQUIRE(payload_named(run, "observed.rgba8").bytes.size() ==
            run.result.dimensions.work_items * 4);
}

TEST_CASE("GpuCompute magnitude recipe agrees with an independent CPU oracle",
          "[gpu][gpu-probe]") {
    const auto run = probe::run_gpu_compute_magnitude_recipe({false, kEvidenceId});
    require_valid(run);
    require_work_or_skip(run);
    REQUIRE((run.result.verdict == probe::Verdict::pass ||
             run.result.verdict == probe::Verdict::unverified));
    REQUIRE(run.result.numeric_sample_count == 256);
    REQUIRE(run.payloads.size() == 3);
}

TEST_CASE("GpuCompute WGSL mutation fails only the numeric oracle",
          "[gpu][gpu-probe][mutation]") {
    const auto run = probe::run_gpu_compute_magnitude_recipe({true, kEvidenceId});
    require_valid(run);
    require_work_or_skip(run);
    REQUIRE(run.result.verdict == probe::Verdict::fail);
    REQUIRE((run.result.passes[0].verdict == probe::Verdict::pass ||
             run.result.passes[0].verdict == probe::Verdict::unverified));
    REQUIRE(run.result.passes[1].verdict == probe::Verdict::pass);
    REQUIRE(run.result.passes[2].verdict == probe::Verdict::fail);
    REQUIRE(run.result.passes[2].code == "cpu_oracle_mismatch");
}

TEST_CASE("Offline GPU STFT agrees with the double-precision CPU oracle",
          "[gpu][gpu-probe][gpu-audio]") {
    const auto run = probe::run_gpu_audio_stft_recipe({false, kEvidenceId});
    require_valid(run);
    require_work_or_skip(run);
    REQUIRE((run.result.verdict == probe::Verdict::pass ||
             run.result.verdict == probe::Verdict::unverified));
    REQUIRE(run.result.numeric_sample_count == 1024);
    REQUIRE(run.payloads.size() == 6);
    REQUIRE(run.result.passes.size() == 4);
    REQUIRE((run.result.passes[0].verdict == probe::Verdict::pass ||
             run.result.passes[0].verdict == probe::Verdict::unverified));
    REQUIRE(run.result.passes[1].code == "stft_forward_completed");
    REQUIRE(run.result.passes[2].code == "stft_magnitude_finite_nonzero");
    REQUIRE(run.result.passes[3].code == "cpu_fft_oracle_match");
}

TEST_CASE("GPU STFT Stockham mutation fails only the CPU oracle",
          "[gpu][gpu-probe][gpu-audio][mutation]") {
    const auto baseline = probe::run_gpu_audio_stft_recipe({false, kEvidenceId});
    require_work_or_skip(baseline);
    require_valid(baseline);

    const auto mutation = probe::run_gpu_audio_stft_recipe({true, kEvidenceId});
    require_valid(mutation);
    require_work_or_skip(mutation);
    REQUIRE(mutation.result.verdict == probe::Verdict::fail);
    REQUIRE((mutation.result.passes[0].verdict == probe::Verdict::pass ||
             mutation.result.passes[0].verdict == probe::Verdict::unverified));
    REQUIRE(mutation.result.passes[1].verdict == probe::Verdict::pass);
    REQUIRE(mutation.result.passes[2].verdict == probe::Verdict::pass);
    REQUIRE(mutation.result.passes[3].verdict == probe::Verdict::fail);
    REQUIRE(mutation.result.passes[3].code == "cpu_fft_oracle_mismatch");
    REQUIRE(mutation.result.mutation == "stockham-stage-output-half");
    REQUIRE(mutation.result.source_digest != baseline.result.source_digest);
    REQUIRE(mutation.result.signature_digest != baseline.result.signature_digest);

    const auto& baseline_magnitude = payload_named(baseline, "observed-magnitude.f32");
    const auto& mutation_magnitude = payload_named(mutation, "observed-magnitude.f32");
    for (std::size_t i = 0; i < 1024; ++i) {
        const float expected = little_endian_float(baseline_magnitude, i) / 1024.0f;
        const float observed = little_endian_float(mutation_magnitude, i);
        INFO("bin " << i);
        REQUIRE(std::abs(observed - expected) <= 1.0e-5f * (1.0f + std::abs(expected)));
    }
}

TEST_CASE("Offline GPU STFT evidence is deterministic on one adapter",
          "[gpu][gpu-probe][gpu-audio][determinism]") {
    const auto first = probe::run_gpu_audio_stft_recipe({false, kEvidenceId});
    require_valid(first);
    require_work_or_skip(first);
    const auto second = probe::run_gpu_audio_stft_recipe({false, kEvidenceId});
    require_valid(second);
    require_work_or_skip(second);

    REQUIRE(second.result.verdict == first.result.verdict);
    REQUIRE(second.result.source_digest == first.result.source_digest);
    REQUIRE(second.result.signature_digest == first.result.signature_digest);
    REQUIRE(second.result.adapter.backend == first.result.adapter.backend);
    REQUIRE(second.result.adapter.name == first.result.adapter.name);
    REQUIRE(second.result.passes.size() == first.result.passes.size());
    for (std::size_t i = 0; i < first.result.passes.size(); ++i) {
        REQUIRE(second.result.passes[i].name == first.result.passes[i].name);
        REQUIRE(second.result.passes[i].verdict == first.result.passes[i].verdict);
        REQUIRE(second.result.passes[i].work_completed ==
                first.result.passes[i].work_completed);
        REQUIRE(second.result.passes[i].code == first.result.passes[i].code);
        REQUIRE(second.result.passes[i].observed == first.result.passes[i].observed);
    }
    REQUIRE(second.payloads.size() == first.payloads.size());
    for (std::size_t i = 0; i < first.payloads.size(); ++i) {
        REQUIRE(second.payloads[i].artifact.name == first.payloads[i].artifact.name);
        REQUIRE(second.payloads[i].artifact.sha256 == first.payloads[i].artifact.sha256);
        REQUIRE(second.payloads[i].bytes == first.payloads[i].bytes);
    }
}

#if PULP_GPU_PROBE_THREEJS_CALLABLE
TEST_CASE("Three.js multi-pass recipe matches independent color regions",
          "[gpu][gpu-probe][threejs]") {
    const auto run = probe::run_threejs_multi_pass_recipe({false, kEvidenceId});
    require_valid(run);
    require_work_or_skip(run);
    if (run.result.verdict == probe::Verdict::unverified) {
        REQUIRE(probe::exit_code(run.result) == 2);
        return;
    }
    REQUIRE(run.result.verdict == probe::Verdict::pass);
    REQUIRE(run.result.adapter.status == probe::IdentityStatus::authentic);
    REQUIRE(run.result.adapter.classification == probe::AdapterClass::hardware);
    REQUIRE(run.result.numeric_sample_count == 5);
    REQUIRE(run.payloads.size() == 4);
    REQUIRE(run.result.passes[4].code == "final_swatch_readback_completed");
    REQUIRE(run.result.passes[5].code == "cpu_region_color_oracle_match");
}

TEST_CASE("Three.js multi-pass recipe reports a missing runtime as unavailable",
          "[gpu][gpu-probe][threejs][contract]") {
    const auto run = probe::run_threejs_multi_pass_recipe(
        {false, kEvidenceId}, "/pulp-test/missing-threejs-runtime");
    require_valid(run);
    REQUIRE(run.result.verdict == probe::Verdict::unavailable);
    REQUIRE(probe::exit_code(run.result) == 2);
    REQUIRE((run.result.passes[1].code == "threejs_runtime_missing" ||
             run.result.passes[1].code == "threejs_runtime_not_compiled"));
    REQUIRE(run.payloads.empty());
}

TEST_CASE("Three.js seeded mutation preserves GPU work and fails the color oracle",
          "[gpu][gpu-probe][threejs][mutation]") {
    const auto baseline = probe::run_threejs_multi_pass_recipe({false, kEvidenceId});
    require_valid(baseline);
    require_work_or_skip(baseline);
    const auto mutation = probe::run_threejs_multi_pass_recipe({true, kEvidenceId});
    require_valid(mutation);
    require_work_or_skip(mutation);

    REQUIRE(mutation.result.verdict == probe::Verdict::fail);
    REQUIRE(mutation.result.mutation == "seeded-final-swatch-channel");
    REQUIRE(mutation.result.passes[0].verdict == probe::Verdict::pass);
    REQUIRE(mutation.result.passes[1].verdict == probe::Verdict::pass);
    REQUIRE(mutation.result.passes[2].verdict == probe::Verdict::pass);
    REQUIRE(mutation.result.passes[3].verdict == probe::Verdict::pass);
    REQUIRE(mutation.result.passes[4].verdict == probe::Verdict::pass);
    REQUIRE(mutation.result.passes[5].verdict == probe::Verdict::fail);
    REQUIRE(mutation.result.passes[5].code == "cpu_region_color_oracle_mismatch");
    REQUIRE(mutation.result.source_digest != baseline.result.source_digest);
    REQUIRE(payload_named(mutation, "final.rgba8").bytes !=
            payload_named(baseline, "final.rgba8").bytes);
}

TEST_CASE("Three.js multi-pass evidence is deterministic on one adapter",
          "[gpu][gpu-probe][threejs][determinism]") {
    const auto first = probe::run_threejs_multi_pass_recipe({false, kEvidenceId});
    require_valid(first);
    require_work_or_skip(first);
    const auto second = probe::run_threejs_multi_pass_recipe({false, kEvidenceId});
    require_valid(second);
    require_work_or_skip(second);

    REQUIRE(second.result.verdict == first.result.verdict);
    REQUIRE(second.result.source_digest == first.result.source_digest);
    REQUIRE(second.result.signature_digest == first.result.signature_digest);
    REQUIRE(second.result.adapter.backend == first.result.adapter.backend);
    REQUIRE(second.result.adapter.name == first.result.adapter.name);
    REQUIRE(second.result.passes.size() == first.result.passes.size());
    for (std::size_t i = 0; i < first.result.passes.size(); ++i) {
        REQUIRE(second.result.passes[i].name == first.result.passes[i].name);
        REQUIRE(second.result.passes[i].verdict == first.result.passes[i].verdict);
        REQUIRE(second.result.passes[i].code == first.result.passes[i].code);
        REQUIRE(second.result.passes[i].observed == first.result.passes[i].observed);
    }
    REQUIRE(second.payloads.size() == first.payloads.size());
    for (std::size_t i = 0; i < first.payloads.size(); ++i) {
        REQUIRE(second.payloads[i].artifact.name == first.payloads[i].artifact.name);
        REQUIRE(second.payloads[i].artifact.sha256 == first.payloads[i].artifact.sha256);
        REQUIRE(second.payloads[i].bytes == first.payloads[i].bytes);
    }
}
#else
TEST_CASE("Three.js multi-pass recipe rejects a build without V8 and its pinned runtime",
          "[gpu][gpu-probe][threejs][contract]") {
    bool rejected = false;
    try {
        (void)probe::run_threejs_multi_pass_recipe({false, kEvidenceId});
    } catch (const std::runtime_error& error) {
        rejected = true;
        REQUIRE(std::string(error.what()) ==
                "threejs.multi-pass.v1 requires a build with the pinned Three.js runtime and V8");
    }
    REQUIRE(rejected);
}
#endif

TEST_CASE("RecipeRun validation binds artifact descriptors to exact payload bytes",
          "[gpu][gpu-probe][contract]") {
    auto run = probe::run_gpu_compute_magnitude_recipe({false, kEvidenceId});
    require_work_or_skip(run);
    require_valid(run);
    REQUIRE_FALSE(run.payloads.empty());

    std::string error;
    run.payloads.front().bytes.front() ^= 0x01u;
    REQUIRE_FALSE(probe::validate(run, &error));
    REQUIRE(error.find("sha256") != std::string::npos);

    run = probe::run_gpu_compute_magnitude_recipe({false, kEvidenceId});
    require_work_or_skip(run);
    run.result.artifacts.front().bytes++;
    REQUIRE_FALSE(probe::validate(run, &error));
    REQUIRE(error.find("descriptor") != std::string::npos);
}
