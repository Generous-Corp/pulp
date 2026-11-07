// user_data_paths.hpp - per-user settings root shared by AppSettings and the
// widget bridge's persisted stores. Source-private: not part of the public view API.
#pragma once

#include <filesystem>

namespace pulp::view::detail {

// Per-user settings root, resolved by platform::special_dir(SpecialDir::config)
// (~/Library/Application Support, %APPDATA%, or $XDG_CONFIG_HOME / ~/.config).
// Empty when the environment names no home, so callers fail closed instead of
// falling back to a world-shared location.
std::filesystem::path platform_settings_root();

} // namespace pulp::view::detail
