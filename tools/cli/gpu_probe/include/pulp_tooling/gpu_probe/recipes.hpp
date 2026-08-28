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

} // namespace pulp::tooling::gpu_probe
