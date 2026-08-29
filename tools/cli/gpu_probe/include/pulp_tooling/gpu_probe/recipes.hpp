#pragma once

#include <pulp_tooling/gpu_probe/probe_result.hpp>

#include <optional>
#include <string>
#include <vector>

namespace pulp::tooling::gpu_probe {

/// Owning artifact descriptor plus its exact payload bytes.
struct ArtifactPayload {
    Artifact artifact;
    std::vector<std::uint8_t> bytes;
};

/// Options that bind one native recipe execution and its negative control.
struct RunOptions {
    bool apply_negative_mutation = false;
    /// Optional trace-correlation identity. When absent, the runner creates a
    /// fresh cryptographic identifier; it never derives one from result data.
    std::optional<std::string> gpu_evidence_id;
};

/// Owning recipe result and byte-exact artifact payloads.
struct RecipeRun {
    ProbeResult result;
    std::vector<ArtifactPayload> payloads;
};

/// Validate result semantics and every descriptor/payload byte and digest binding.
bool validate(const RecipeRun& run, std::string* error = nullptr);

/// Run the blocking deterministic Renderer3D/readback recipe.
RecipeRun run_renderer3d_recipe(const RunOptions& options = {});
/// Run the blocking deterministic GPU magnitude recipe and CPU oracle.
RecipeRun run_gpu_compute_magnitude_recipe(const RunOptions& options = {});

/// Run one blocking GPU STFT frame and its CPU oracle.
///
/// This diagnostic owns no audio device or Processor and runs only from an
/// offline tool or worker thread, never an audio callback.
RecipeRun run_gpu_audio_stft_recipe(const RunOptions& options = {});

/// Run the blocking deterministic multi-pass recipe through a Three.js runtime.
///
/// `threejs_runtime_root` is borrowed for the call. An explicit root supports
/// installed CLI dispatch; absence selects the configured pinned runtime. A
/// missing or unusable runtime produces `unavailable`, not an empty pass.
RecipeRun run_threejs_multi_pass_recipe(
    const RunOptions& options = {},
    std::optional<std::string> threejs_runtime_root = std::nullopt);

} // namespace pulp::tooling::gpu_probe
