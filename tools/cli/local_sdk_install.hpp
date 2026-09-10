#pragma once

#include <filesystem>
#include <string>

// Build an immutable, development-only Apple Silicon SDK from an exact clean
// checkout for rapid Forge iteration. The returned prefix carries
// sdk-provenance.json with distribution_eligible=false.
std::filesystem::path ensure_forge_dev_sdk(const std::filesystem::path& repo_root);

// The same immutable flow with Perfetto compiled in, published under its own
// prefix root. The staged install is rejected unless the built archives really
// contain Perfetto, so a prefix belonging to this profile always traces.
std::filesystem::path ensure_trace_sdk(const std::filesystem::path& repo_root);

// Shared implementation of both profiles above.
std::filesystem::path ensure_dev_profile_sdk(const std::filesystem::path& repo_root, bool tracing);
