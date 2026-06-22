#pragma once

// Generic model store/state primitive (promoted from tools/audio). Owns the on-disk
// layout under PULP_HOME (`<home>/<subsystem>/model-state.json` for the active model,
// `<home>/<subsystem>/models/<id>.json` for per-model install metadata), the
// installed/active queries, list (over a caller-supplied registry), and activate.
// Parameterized by `subsystem` (e.g. "audio", "magenta") so multiple consumers share
// one mechanism. Install/remove stream checkpoints through the runtime downloader.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pulp/runtime/model_registry.hpp>

namespace pulp::runtime {
class CancellationToken;  // async_stream.hpp
}

namespace pulp::runtime {

struct InstalledModelRecord {
    std::filesystem::path metadata_path;
    bool metadata_found = false;
    std::string model_id;
    std::string backend;
    std::string checkpoint_ref;
    std::filesystem::path resolved_checkpoint_path;
    bool checkpoint_exists = false;

    [[nodiscard]] bool loadable() const { return metadata_found && checkpoint_exists; }
};

struct ListedModel {
    ModelEntry model;
    std::string status = "not_installed";  // installed | missing_checkpoint | partial | not_installed
    bool active = false;
    std::filesystem::path resolved_checkpoint_path;
    float partial_fraction = -1.0f;  // >= 0 when a resumable .part exists (downloaded/total)
};

struct ModelListResult {
    std::filesystem::path pulp_home;
    std::string active_model_id;
    std::vector<ListedModel> models;
    std::string error;
};

struct ActivateModelResult {
    bool ok = false;
    std::filesystem::path state_path;
    std::string active_model_id;
    std::string backend;
    std::string checkpoint_ref;
    std::filesystem::path resolved_checkpoint_path;
    std::string error;
};

// PULP_HOME resolution: explicit override → $PULP_HOME → $HOME/.pulp (USERPROFILE on
// Windows). NOTE (path policy, see plan): a sandboxed AUv3/iOS host may not have HOME
// access — callers in those contexts should pass an explicit container path.
std::filesystem::path resolve_pulp_home(const std::filesystem::path& override_path = {});

std::filesystem::path model_state_path(std::string_view subsystem,
                                       const std::filesystem::path& pulp_home_override = {});
std::filesystem::path model_install_path(std::string_view subsystem, std::string_view model_id,
                                         const std::filesystem::path& pulp_home_override = {});
InstalledModelRecord read_installed_model(std::string_view subsystem, std::string_view model_id,
                                          const std::filesystem::path& pulp_home_override = {});
std::string read_active_model_id(std::string_view subsystem,
                                 const std::filesystem::path& pulp_home_override = {});
ModelListResult list_models(const std::vector<ModelEntry>& registry, std::string_view subsystem,
                            const std::filesystem::path& pulp_home_override = {});
ActivateModelResult activate_model(const std::vector<ModelEntry>& registry, std::string_view subsystem,
                                   std::string_view model_id,
                                   const std::filesystem::path& pulp_home_override = {});

std::string to_json(const ModelListResult& result);
std::string to_json(const ActivateModelResult& result);

// ---- install / remove (streaming, resumable, sha256-verified downloader) -----------

struct InstallModelResult {
    bool ok = false;
    std::string error;
    bool cancelled = false;
    std::filesystem::path checkpoint_path;  // downloaded file
    std::filesystem::path metadata_path;    // <home>/<subsystem>/models/<id>.json
    std::string sha256;
};

/// Download `model`'s checkpoint into `<home>/<subsystem>/models/<id>/` (streaming,
/// resumable, sha256-verified), then write the install metadata so read_installed_model/
/// activate_model see it. `on_progress` returning false (or a cancelled token) aborts and
/// keeps the partial for a later resume. Auth headers (e.g. HuggingFace token) optional.
InstallModelResult install_model(const ModelEntry& model, std::string_view subsystem,
                                 const std::function<bool(const struct DownloadProgress&)>& on_progress = {},
                                 const CancellationToken* cancel = nullptr,
                                 const std::vector<std::pair<std::string, std::string>>& headers = {},
                                 const std::filesystem::path& pulp_home_override = {});

/// Delete an installed model's files + metadata. Clears the active selection if it pointed
/// here. Returns false (with `error` set) on a filesystem error or invalid input, and false
/// (no error) when nothing was present to remove; true only when files/metadata were deleted.
bool remove_model(std::string_view subsystem, std::string_view model_id, std::string& error,
                  const std::filesystem::path& pulp_home_override = {});

}  // namespace pulp::runtime
