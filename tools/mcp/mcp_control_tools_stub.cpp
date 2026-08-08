// Inspector-disabled builds intentionally contain no live control adapter.

#include "mcp_control_tools.hpp"

namespace pulp_mcp {
namespace {
std::string unavailable() {
    return "{\"content\":[{\"type\":\"text\",\"text\":\"Capability control is not built into this server\"}],\"isError\":true,\"structuredContent\":{\"ok\":false,\"error\":{\"code\":\"component-unavailable\",\"message\":\"Capability control is not built into this server\",\"data\":{}}}}";
}
} // namespace

void configure_control_mcp_executable(std::string) {}
void set_control_mcp_session_factory_for_test(ControlMcpSessionFactory) {}
void reset_control_mcp_session_factory_for_test() {}
void set_control_mcp_notification_sink(ControlMcpNotificationSink) {}
std::string control_mcp_tools_json_fragment() { return {}; }
bool is_control_mcp_tool(std::string_view) { return false; }
std::string handle_control_mcp_tool(std::string_view, std::string_view, std::string_view) {
    return unavailable();
}
std::string control_mcp_resource_templates_payload() {
    return R"({"resourceTemplates":[]})";
}
std::string control_mcp_resources_list_payload() { return R"({"resources":[]})"; }
std::string control_mcp_resource_read_payload(std::string_view) { return unavailable(); }
} // namespace pulp_mcp
