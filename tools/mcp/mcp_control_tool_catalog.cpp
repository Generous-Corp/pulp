// mcp_control_tool_catalog.cpp -- generated MCP catalog for capability control.

#include "mcp_control_tool_catalog.hpp"

#include "mcp_control_tools.hpp"
#include "mcp_json.hpp"

#include <pulp/inspect/capabilities.hpp>

#include <cctype>
#include <string>

namespace pulp_mcp {
namespace {

using namespace pulp::inspect;

constexpr std::string_view kInstancesTool = "pulp_control_instances";
constexpr std::string_view kStatusTool = "pulp_control_status";
constexpr std::string_view kGrantTool = "pulp_control_grant_request";
constexpr std::string_view kRevokeTool = "pulp_control_revoke";
constexpr std::string_view kCancelTool = "pulp_control_cancel";

std::string operation_tool_name(std::string_view id) {
    constexpr std::string_view prefix = "dev.pulp.";
    constexpr std::string_view suffix = "@1";
    if (!id.starts_with(prefix) || !id.ends_with(suffix))
        return {};
    id.remove_prefix(prefix.size());
    id.remove_suffix(suffix.size());
    std::string name = "pulp_control_";
    for (const auto character : id) {
        if (std::isalnum(static_cast<unsigned char>(character)))
            name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        else if (name.back() != '_')
            name.push_back('_');
    }
    return name;
}

std::string quote(std::string_view value) {
    return json_string(std::string(value));
}

std::string management_tool_json() {
    return R"JSON({"name":"pulp_control_instances","description":"List broker-owned live capability-control instances, starting the broker-owned default installed host when none is live.","inputSchema":{"type":"object","additionalProperties":false,"properties":{}},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},{"name":"pulp_control_status","description":"Read one exact broker-owned instance by instance_id.","inputSchema":{"type":"object","additionalProperties":false,"required":["instance_id"],"properties":{"instance_id":{"type":"string","minLength":1}}},"annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},{"name":"pulp_control_grant_request","description":"Ask the broker for a profile or single-operation grant. Any consent challenge is broker-owned; MCP metadata or client UI approval is never authority.","inputSchema":{"type":"object","additionalProperties":false,"required":["instance_id"],"oneOf":[{"required":["profile"]},{"required":["operation_id"]}],"properties":{"instance_id":{"type":"string","minLength":1},"profile":{"type":"string","enum":["inspect-readonly","observe","develop"]},"operation_id":{"type":"string","minLength":1}}},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},{"name":"pulp_control_revoke","description":"Revoke a broker-issued grant.","inputSchema":{"type":"object","additionalProperties":false,"required":["grant_id"],"properties":{"grant_id":{"type":"string","minLength":1}}},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":true,"openWorldHint":false}},{"name":"pulp_control_cancel","description":"Request cooperative cancellation for an operation on one exact instance.","inputSchema":{"type":"object","additionalProperties":false,"required":["instance_id","request_id"],"properties":{"instance_id":{"type":"string","minLength":1},"request_id":{"type":"string","minLength":1},"reason":{"type":"string","maxLength":1024}}},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}})JSON";
}

std::string operation_output_schema(const ControlOperationDescriptor& operation) {
    if (operation.capability == InspectorCapability::ArtifactRead)
        return std::string(operation.output_schema_json);
    return "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"ok\",\"schema\",\"request_id\",\"receipt_id\",\"operation_id\",\"operation_version\",\"state\",\"explanation\",\"result\",\"artifacts\",\"progress\"],\"properties\":{\"ok\":{\"const\":true},\"schema\":{\"const\":\"dev.pulp.control/mcp-receipt@1\"},\"request_id\":{\"type\":\"string\"},\"receipt_id\":{\"type\":\"string\"},\"operation_id\":{\"const\":" +
           quote(operation.id) +
           "},\"operation_version\":{\"const\":" + std::to_string(operation.version) +
           "},\"state\":{\"const\":\"completed\"},\"explanation\":{\"type\":\"string\"},\"result\":" +
           std::string(operation.output_schema_json) +
           ",\"artifacts\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"artifact_id\",\"media_type\",\"byte_size\"],\"properties\":{\"artifact_id\":{\"type\":\"string\"},\"media_type\":{\"type\":\"string\"},\"byte_size\":{\"type\":\"integer\",\"minimum\":0}}}},\"progress\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"sequence\",\"current\",\"total\",\"detail\"],\"properties\":{\"sequence\":{\"type\":\"integer\",\"minimum\":1},\"current\":{\"type\":\"integer\",\"minimum\":0},\"total\":{\"type\":\"integer\",\"minimum\":1},\"detail\":{\"type\":\"object\"}}}}}}";
}

} // namespace

namespace detail {

const ControlOperationDescriptor* control_operation_for_tool(std::string_view name) {
    for (const auto& operation : control_operation_registry())
        if ((capability_is_grantable(operation.capability) ||
             operation.capability == InspectorCapability::ArtifactRead) &&
            operation_tool_name(operation.id) == name)
            return &operation;
    return nullptr;
}

} // namespace detail

std::string ControlMcpAdapter::tools_json_fragment() const {
    std::string out = management_tool_json();
    for (const auto& operation : control_operation_registry()) {
        if (!capability_is_grantable(operation.capability) &&
            operation.capability != InspectorCapability::ArtifactRead)
            continue;
        const auto name = operation_tool_name(operation.id);
        const auto risk = capability_risk(operation.capability);
        const bool read_only = risk == InspectorCapabilityRisk::Observe ||
                               risk == InspectorCapabilityRisk::Sensitive;
        out += ",{\"name\":" + quote(name) + ",\"description\":" +
               quote("Typed capability-control operation " + std::string(operation.id) +
                     ". instance_id selects exactly one broker-owned live instance. Grants and consent are broker authority; MCP annotations are advisory only.") +
               ",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"instance_id\",\"input\"],\"properties\":{\"instance_id\":{\"type\":\"string\",\"minLength\":1},\"request_id\":{\"type\":\"string\",\"minLength\":1},\"grant_id\":{\"type\":\"string\",\"minLength\":1},\"profile\":{\"type\":\"string\",\"enum\":[\"inspect-readonly\",\"observe\",\"develop\"]},\"expected_state_generation\":{\"type\":\"integer\",\"minimum\":0},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":300000},\"input\":" +
               std::string(operation.input_schema_json) + "}},\"outputSchema\":" +
               operation_output_schema(operation) +
               ",\"annotations\":{\"readOnlyHint\":" + (read_only ? "true" : "false") +
               ",\"destructiveHint\":" +
               (risk == InspectorCapabilityRisk::Critical || risk == InspectorCapabilityRisk::HighRisk
                    ? "true"
                    : "false") +
               ",\"idempotentHint\":" + (read_only ? "true" : "false") +
               ",\"openWorldHint\":false}}";
    }
    return out;
}

bool ControlMcpAdapter::owns_tool(std::string_view name) const {
    return name == kInstancesTool || name == kStatusTool || name == kGrantTool ||
           name == kRevokeTool || name == kCancelTool ||
           detail::control_operation_for_tool(name) != nullptr;
}

} // namespace pulp_mcp
