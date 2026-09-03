#include <pulp_tooling/gpu_probe/recipes.hpp>

#include <iostream>
#include <string>

namespace probe = pulp::tooling::gpu_probe;

namespace {

bool valid(const probe::RecipeRun& run, std::string_view label) {
    std::string error;
    if (probe::validate(run, &error)) return true;
    std::cerr << label << "_contract=fail error=" << error << '\n';
    return false;
}

bool renderer_acceptance() {
    const auto baseline = probe::run_renderer3d_recipe();
    probe::RunOptions mutation_options;
    mutation_options.apply_negative_mutation = true;
    const auto mutation = probe::run_renderer3d_recipe(mutation_options);
    const bool baseline_ok = valid(baseline, "renderer_baseline") &&
        baseline.result.verdict == probe::Verdict::pass &&
        baseline.result.passes.back().code == "metal_fingerprint_match";
    const bool mutation_ok = valid(mutation, "renderer_mutation") &&
        mutation.result.verdict == probe::Verdict::fail &&
        mutation.result.passes.back().code == "portable_structure_mismatch";
    std::cout << "renderer3d_baseline=" << (baseline_ok ? "pass" : "fail")
              << " renderer3d_mutation=" << (mutation_ok ? "pass" : "fail")
              << '\n';
    return baseline_ok && mutation_ok;
}

bool compute_acceptance() {
    const auto baseline = probe::run_gpu_compute_magnitude_recipe();
    probe::RunOptions mutation_options;
    mutation_options.apply_negative_mutation = true;
    const auto mutation = probe::run_gpu_compute_magnitude_recipe(mutation_options);
    const bool baseline_ok = valid(baseline, "compute_baseline") &&
        baseline.result.verdict == probe::Verdict::pass &&
        baseline.result.passes.back().code == "cpu_oracle_match";
    const bool mutation_ok = valid(mutation, "compute_mutation") &&
        mutation.result.verdict == probe::Verdict::fail &&
        mutation.result.passes.back().code == "cpu_oracle_mismatch";
    std::cout << "gpu_compute_baseline=" << (baseline_ok ? "pass" : "fail")
              << " gpu_compute_mutation=" << (mutation_ok ? "pass" : "fail")
              << '\n';
    return baseline_ok && mutation_ok;
}

} // namespace

int main() {
    const bool renderer_ok = renderer_acceptance();
    const bool compute_ok = compute_acceptance();
    const bool ok = renderer_ok && compute_ok;
    std::cout << "gpu_probe_native_acceptance=" << (ok ? "pass" : "fail") << '\n';
    return ok ? 0 : 1;
}
