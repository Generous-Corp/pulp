#pragma once

#include <string>

namespace pulp_mcp::server {

std::string tools_list_json();
std::string handle_request(const std::string& json);
int run(int argc, char* argv[]);

} // namespace pulp_mcp::server
