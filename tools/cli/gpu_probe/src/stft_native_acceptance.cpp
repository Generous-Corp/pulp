#include <pulp_tooling/gpu_probe/recipes.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
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

bool same_artifacts(const probe::RecipeRun& first, const probe::RecipeRun& second) {
    if (first.payloads.size() != second.payloads.size()) return false;
    for (std::size_t i = 0; i < first.payloads.size(); ++i) {
        if (first.payloads[i].artifact.name != second.payloads[i].artifact.name ||
            first.payloads[i].artifact.sha256 != second.payloads[i].artifact.sha256 ||
            first.payloads[i].bytes != second.payloads[i].bytes)
            return false;
    }
    return true;
}

float little_endian_float(const probe::ArtifactPayload& payload, std::size_t index) {
    const auto at = index * sizeof(float);
    const std::uint32_t bits = static_cast<std::uint32_t>(payload.bytes[at]) |
        (static_cast<std::uint32_t>(payload.bytes[at + 1]) << 8u) |
        (static_cast<std::uint32_t>(payload.bytes[at + 2]) << 16u) |
        (static_cast<std::uint32_t>(payload.bytes[at + 3]) << 24u);
    return std::bit_cast<float>(bits);
}

bool stage_scaling_accumulates(const probe::RecipeRun& baseline,
                               const probe::RecipeRun& mutation) {
    if (baseline.payloads.size() != 6 || mutation.payloads.size() != 6 ||
        baseline.payloads[4].artifact.name != "observed-magnitude.f32" ||
        mutation.payloads[4].artifact.name != "observed-magnitude.f32" ||
        baseline.payloads[4].bytes.size() != 1024 * sizeof(float) ||
        mutation.payloads[4].bytes.size() != 1024 * sizeof(float))
        return false;
    for (std::size_t i = 0; i < 1024; ++i) {
        const float expected = little_endian_float(baseline.payloads[4], i) / 1024.0f;
        const float observed = little_endian_float(mutation.payloads[4], i);
        if (std::abs(observed - expected) >
            1.0e-5f * (1.0f + std::abs(expected)))
            return false;
    }
    return true;
}

} // namespace

int main() {
    const auto baseline = probe::run_gpu_audio_stft_recipe();
    const auto repeat = probe::run_gpu_audio_stft_recipe();
    probe::RunOptions mutation_options;
    mutation_options.apply_negative_mutation = true;
    const auto mutation = probe::run_gpu_audio_stft_recipe(mutation_options);

    const bool baseline_ok = valid(baseline, "stft_baseline") &&
        baseline.result.verdict == probe::Verdict::pass &&
        baseline.result.adapter.status == probe::IdentityStatus::authentic &&
        baseline.result.adapter.classification == probe::AdapterClass::hardware &&
        baseline.result.adapter.backend == "Metal" &&
        baseline.result.passes.back().code == "cpu_fft_oracle_match";
    const bool repeat_ok = valid(repeat, "stft_repeat") &&
        repeat.result.verdict == probe::Verdict::pass &&
        repeat.result.source_digest == baseline.result.source_digest &&
        repeat.result.signature_digest == baseline.result.signature_digest &&
        same_artifacts(baseline, repeat);
    const bool mutation_ok = valid(mutation, "stft_mutation") &&
        mutation.result.verdict == probe::Verdict::fail &&
        mutation.result.passes.size() == 4 &&
        mutation.result.passes[0].verdict == probe::Verdict::pass &&
        mutation.result.passes[1].verdict == probe::Verdict::pass &&
        mutation.result.passes[2].verdict == probe::Verdict::pass &&
        mutation.result.passes[3].verdict == probe::Verdict::fail &&
        mutation.result.passes[3].code == "cpu_fft_oracle_mismatch" &&
        mutation.result.source_digest != baseline.result.source_digest &&
        mutation.result.signature_digest != baseline.result.signature_digest &&
        stage_scaling_accumulates(baseline, mutation);

    std::cout << "gpu_audio_stft_baseline=" << (baseline_ok ? "pass" : "fail")
              << " gpu_audio_stft_repeat=" << (repeat_ok ? "pass" : "fail")
              << " gpu_audio_stft_mutation=" << (mutation_ok ? "pass" : "fail")
              << '\n';
    return baseline_ok && repeat_ok && mutation_ok ? 0 : 1;
}
