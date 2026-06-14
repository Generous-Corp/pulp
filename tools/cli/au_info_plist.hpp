#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pulp::cli::au_info_plist {

std::string parse_unique_id_from_text(std::string_view plist);
std::string unique_id_from_bundle(const std::filesystem::path& bundle);

}  // namespace pulp::cli::au_info_plist
