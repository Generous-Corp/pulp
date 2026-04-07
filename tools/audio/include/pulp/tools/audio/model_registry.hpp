#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::tools::audio {

struct RegisteredModel {
    std::string model_id;
    std::string display_name;
    std::string backend;
    std::string checkpoint_ref;       // e.g., "hf://user/repo/file.pt"
    std::vector<std::string> task_tags;
    std::uint64_t size_bytes = 0;

    // Phase 4 additions
    std::string download_url;         // Direct HTTPS URL (resolved from checkpoint_ref)
    std::string sha256;               // Expected hash of the checkpoint file
    bool auto_downloadable = true;    // false if manual steps required
};

/// Resolve a checkpoint_ref to a direct download URL.
/// "hf://user/repo/file" → "https://huggingface.co/user/repo/resolve/main/file"
std::string resolve_checkpoint_url(const std::string& checkpoint_ref);

const std::vector<RegisteredModel>& registered_models();
const RegisteredModel* find_registered_model(std::string_view model_id);

} // namespace pulp::tools::audio
