#pragma once

#include <filesystem>
#include <string>

// Build an immutable, development-only Apple Silicon SDK from an exact clean
// checkout for rapid Forge iteration. The returned prefix carries
// sdk-provenance.json with distribution_eligible=false.
std::filesystem::path ensure_forge_dev_sdk(const std::filesystem::path& repo_root);
