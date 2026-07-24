// mcp_tools_internal.hpp — Shared implementation details for MCP tool handlers.

#pragma once

#include <filesystem>
#include <string>

namespace pulp_mcp {

struct ProbeJsonTemp {
    std::filesystem::path directory;
    std::filesystem::path json_path;
};

std::filesystem::path source_build_cli_path(const std::filesystem::path& root);
ProbeJsonTemp make_private_probe_json_temp(std::string& error);
std::string read_text_file(const std::filesystem::path& path);
bool normalize_structured_json(const std::string& text, std::string& normalized,
                               std::string& error);
std::string import_design_defaults_line();

} // namespace pulp_mcp
