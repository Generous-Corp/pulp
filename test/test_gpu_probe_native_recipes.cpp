#include <catch2/catch_test_macros.hpp>

#include <pulp_tooling/gpu_probe/recipes.hpp>

#include <set>

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

} // namespace

TEST_CASE("Renderer3D recipe produces bounded typed evidence", "[gpu][gpu-probe]") {
    const auto run = probe::run_renderer3d_recipe({false, kEvidenceId});
    require_valid(run);
    require_work_or_skip(run);
    REQUIRE(run.result.verdict == probe::Verdict::pass);
    REQUIRE_FALSE(run.payloads.empty());
}

TEST_CASE("Renderer3D post-readback mutation localizes to content",
          "[gpu][gpu-probe][mutation]") {
    const auto run = probe::run_renderer3d_recipe({true, kEvidenceId});
    require_valid(run);
    require_work_or_skip(run);
    REQUIRE(run.result.verdict == probe::Verdict::fail);
    REQUIRE(run.result.passes[0].verdict == probe::Verdict::pass);
    REQUIRE(run.result.passes[1].verdict == probe::Verdict::pass);
    REQUIRE(run.result.passes[2].verdict == probe::Verdict::pass);
    REQUIRE(run.result.passes[3].verdict == probe::Verdict::fail);
    REQUIRE(run.result.passes[3].code == "portable_structure_mismatch");
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
