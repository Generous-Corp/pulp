#pragma once

#include <memory>
#include <string>

namespace pulp::inspect {
class InspectorControlSessionOpener;
}

namespace pulp_mcp::server {

std::string tools_list_json();
std::string handle_request(const std::string& json);
int run(int argc, char* argv[]);
void set_trace_control_session_opener_for_test(
    std::shared_ptr<pulp::inspect::InspectorControlSessionOpener> opener);

} // namespace pulp_mcp::server
