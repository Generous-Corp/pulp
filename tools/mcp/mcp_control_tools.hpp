// mcp_control_tools.hpp -- typed MCP projection of capability control.

#pragma once

#include <pulp/inspect/control_client.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp_mcp {

/// One long-lived, authenticated MCP client session. Implementations own
/// discovery, peer verification, enrollment, and the connection-bound client
/// identity. MCP never accepts those authority fields from tool arguments.
class ControlMcpSession {
  public:
    using ProgressSink = std::function<void(const pulp::inspect::ControlProgressEnvelope&)>;

    virtual ~ControlMcpSession() = default;
    virtual pulp::inspect::ControlClientTransport& transport() = 0;
    virtual pulp::inspect::ControlManagementResult
    manage(std::string_view command, std::string_view params_json = "{}") = 0;
    virtual std::string_view client_id() const = 0;
    virtual void set_progress_sink(ProgressSink sink) = 0;
};

struct ControlMcpOpenResult {
    std::unique_ptr<ControlMcpSession> session;
    std::string error_code;
    std::string explanation;
};

using ControlMcpSessionFactory = std::function<ControlMcpOpenResult()>;
using ControlMcpNotificationSink = std::function<void(std::string)>;

/// Adapter instance used directly by focused conformance tests and by the
/// process-wide stdio server. It deliberately contains projection logic only;
/// control negotiation, requests, cancellation, progress validation, artifact
/// ACLs, grants, consent, and receipts stay in the canonical client/broker.
class ControlMcpAdapter {
  public:
    explicit ControlMcpAdapter(ControlMcpSessionFactory factory,
                               ControlMcpNotificationSink notifications = {});
    ~ControlMcpAdapter();
    ControlMcpAdapter(const ControlMcpAdapter&) = delete;
    ControlMcpAdapter& operator=(const ControlMcpAdapter&) = delete;

    std::string tools_json_fragment() const;
    bool owns_tool(std::string_view name) const;
    std::string call_tool(std::string_view name, std::string_view arguments_json,
                          std::string_view progress_token = {});

    std::string resource_templates_payload() const;
    std::string resources_list_payload();
    std::string resource_read_payload(std::string_view uri);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Process-wide server hooks. Tests may replace the factory; the production
/// factory trusts only the broker installed beside the running pulp-mcp.
void configure_control_mcp_executable(std::string executable_path);
void set_control_mcp_session_factory_for_test(ControlMcpSessionFactory factory);
void reset_control_mcp_session_factory_for_test();
void set_control_mcp_notification_sink(ControlMcpNotificationSink sink);

std::string control_mcp_tools_json_fragment();
bool is_control_mcp_tool(std::string_view name);
std::string handle_control_mcp_tool(std::string_view name, std::string_view arguments_json,
                                    std::string_view progress_token = {});
std::string control_mcp_resource_templates_payload();
std::string control_mcp_resources_list_payload();
std::string control_mcp_resource_read_payload(std::string_view uri);

} // namespace pulp_mcp
