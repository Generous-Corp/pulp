#pragma once

#include <pulp/inspect/control_manifest.hpp>

#include <string_view>

namespace pulp_mcp::detail {

const pulp::inspect::ControlOperationDescriptor* control_operation_for_tool(
    std::string_view name);

} // namespace pulp_mcp::detail
