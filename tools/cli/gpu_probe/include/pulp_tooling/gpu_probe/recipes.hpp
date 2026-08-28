#pragma once

#include <pulp_tooling/gpu_probe/probe_result.hpp>

#include <optional>
#include <string>
#include <vector>

namespace pulp::tooling::gpu_probe {

struct ArtifactPayload {
    Artifact artifact;
    std::vector<std::uint8_t> bytes;
};

struct RunOptions {
    bool apply_negative_mutation = false;
    // Tests and trace-correlated callers may supply the run identity. Normal
    // callers leave this empty and receive a fresh cryptographic identifier.
    std::optional<std::string> gpu_evidence_id;
};

struct RecipeRun {
    ProbeResult result;
    std::vector<ArtifactPayload> payloads;
};

bool validate(const RecipeRun& run, std::string* error = nullptr);

RecipeRun run_renderer3d_recipe(const RunOptions& options = {});
RecipeRun run_gpu_compute_magnitude_recipe(const RunOptions& options = {});

/// Runs one blocking GPU STFT frame and its CPU oracle. This diagnostic owns no
/// audio device or Processor and must run from an offline tool/worker thread.
RecipeRun run_gpu_audio_stft_recipe(const RunOptions& options = {});

/// The optional runtime root supports installed CLI dispatch while direct
/// source tests use the configured pinned runtime.
RecipeRun run_threejs_multi_pass_recipe(
    const RunOptions& options = {},
    std::optional<std::string> threejs_runtime_root = std::nullopt);

} // namespace pulp::tooling::gpu_probe
