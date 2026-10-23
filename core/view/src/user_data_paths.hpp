// user_data_paths.hpp - per-user settings root shared by AppSettings and the
// widget bridge's persisted stores. Source-private: the resolver lives in
// app_framework.cpp and is not part of the public view API.
#pragma once

#include <filesystem>

namespace pulp::view::detail {

// Per-user application-data root: ~/Library/Application Support on macOS,
// %APPDATA% on Windows, $XDG_CONFIG_HOME or ~/.config elsewhere. Empty when
// the environment names no home, so callers fail closed instead of falling
// back to a world-shared location.
std::filesystem::path platform_settings_root();

} // namespace pulp::view::detail
