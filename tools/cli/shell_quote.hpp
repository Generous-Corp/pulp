#pragma once

#include <filesystem>
#include <string>

std::string shell_quote(const std::string& s);
std::string shell_quote(const std::filesystem::path& p);
