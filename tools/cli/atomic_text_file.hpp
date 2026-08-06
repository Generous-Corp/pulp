#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pulp::cli {

/// Replace a text file through a same-directory temporary and durable rename.
///
/// On failure the destination is left intact unless the platform reports that
/// replacement completed but its parent-directory sync did not.
bool write_text_file_atomically(const std::filesystem::path& destination,
                                std::string_view contents,
                                std::string& error) noexcept;

} // namespace pulp::cli
