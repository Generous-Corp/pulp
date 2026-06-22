#pragma once

// The generic model store lives in pulp::runtime. tools/audio keeps these
// thin wrappers (subsystem = "audio") so the CLI / MCP surface is unchanged.

#include <filesystem>
#include <string>
#include <string_view>

#include <pulp/runtime/model_store.hpp>
#include <pulp/tools/audio/model_registry.hpp>

namespace pulp::tools::audio {

using pulp::runtime::ActivateModelResult;
using pulp::runtime::InstalledModelRecord;
using pulp::runtime::ListedModel;
using pulp::runtime::ModelListResult;
using pulp::runtime::resolve_pulp_home;
using pulp::runtime::to_json;

inline std::filesystem::path audio_model_state_path(const std::filesystem::path& pulp_home_override = {}) {
    return pulp::runtime::model_state_path("audio", pulp_home_override);
}
inline std::filesystem::path audio_model_install_path(std::string_view model_id,
                                                      const std::filesystem::path& pulp_home_override = {}) {
    return pulp::runtime::model_install_path("audio", model_id, pulp_home_override);
}
inline InstalledModelRecord read_installed_model(std::string_view model_id,
                                                 const std::filesystem::path& pulp_home_override = {}) {
    return pulp::runtime::read_installed_model("audio", model_id, pulp_home_override);
}
inline std::string read_active_model_id(const std::filesystem::path& pulp_home_override = {}) {
    return pulp::runtime::read_active_model_id("audio", pulp_home_override);
}
inline ModelListResult list_models(const std::filesystem::path& pulp_home_override = {}) {
    return pulp::runtime::list_models(registered_models(), "audio", pulp_home_override);
}
inline ActivateModelResult activate_model(std::string_view model_id,
                                          const std::filesystem::path& pulp_home_override = {}) {
    return pulp::runtime::activate_model(registered_models(), "audio", model_id, pulp_home_override);
}

}  // namespace pulp::tools::audio
