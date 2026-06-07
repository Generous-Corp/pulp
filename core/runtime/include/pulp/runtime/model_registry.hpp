#pragma once

// Generic model registry primitive (promoted from tools/audio so plugins/apps —
// not just the CLI — can use it). A `ModelEntry` describes a downloadable ML model
// (HuggingFace or any URL); `resolve_checkpoint_url` turns an `hf://` ref into a
// direct HTTPS URL with path hardening. The available-models list (the catalog) is
// supplied by the consumer (e.g. tools/audio's audio catalog, or the Magenta demo's
// mrt2 catalog) — this module owns the type + resolution + lookup, not the data.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::runtime {

struct ModelAsset {
    std::string role;          ///< e.g. "weights", "vocab", "encoder" (multi-file bundles)
    std::string checkpoint_ref;///< "hf://user/repo/file" or "https://…"
    std::string sha256;        ///< Expected hash of this asset (empty = unverified)
    std::uint64_t size_bytes = 0;
};

struct ModelEntry {
    std::string model_id;             ///< Stable id (e.g. "mrt2_base"), NOT the HF repo.
    std::string display_name;
    std::string description;
    std::string backend;              ///< e.g. "clap", "mlx".
    std::string checkpoint_ref;       ///< Primary asset, e.g. "hf://user/repo/file.pt".
    std::vector<std::string> task_tags;
    std::uint64_t size_bytes = 0;

    std::string download_url;         ///< Direct HTTPS URL (resolved from checkpoint_ref).
    std::string sha256;               ///< Expected hash of the primary checkpoint.
    bool auto_downloadable = true;    ///< false ⇒ manual steps required.

    // Multi-asset bundles (e.g. Magenta: weights + vocab + encoder). When empty the
    // entry is single-file (checkpoint_ref). The downloader fetches all assets.
    std::vector<ModelAsset> assets;

    bool is_recommended = false;      ///< ⭐ in the model manager.
    std::string license;              ///< e.g. "CC-BY-4.0".
    std::string attribution;          ///< e.g. "Google DeepMind".
    std::string min_device;           ///< Optional hint, e.g. "Pro/Max".
};

/// Resolve a checkpoint_ref to a direct download URL.
/// "hf://user/repo/file" → "https://huggingface.co/user/repo/resolve/main/file".
/// Rejects control bytes, `..` segments (in both the user/repo and file parts),
/// and percent-encoded path content. Passes http(s):// URLs through.
std::string resolve_checkpoint_url(const std::string& checkpoint_ref);

/// Find a model by id in a caller-supplied registry/catalog. Returns nullptr if absent.
const ModelEntry* find_model(const std::vector<ModelEntry>& registry, std::string_view model_id);

}  // namespace pulp::runtime
