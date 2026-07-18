// cli_aax.hpp — AAX SDK / validator discovery + setup-guidance helpers.
//
// The declarations for the AAX helper family (implemented across
// cli_common.cpp / cli_doctor_helpers.cpp). Used by `validate`, `doctor`, and
// `create`. Included by cli_common.hpp, so command files that already include
// that header need no change; include this directly when only the AAX helpers
// are needed.
#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

bool aax_supported_on_host();
std::string aax_download_url();
std::string aax_sdk_download_label();
std::string aax_validator_download_label();
bool looks_like_aax_sdk_root(const fs::path& path);
fs::path find_aax_sdk_root();
fs::path find_aax_validator_root();
void print_aax_setup_guidance(bool need_sdk, bool need_validator);
std::string run_aax_validator_command(const fs::path& validator_root,
                                     const fs::path& plugin_path,
                                     bool run_all);
bool aax_validator_passed(const std::string& output);
