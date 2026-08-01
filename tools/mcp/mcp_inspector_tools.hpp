#pragma once

#include <choc/containers/choc_Value.h>

#include <optional>
#include <string>
#include <string_view>

namespace pulp_mcp {

/// JSON tool definitions for every live inspector operation, without a
/// surrounding array.
std::string live_inspector_tools_json();

/// Validate and execute a registered live inspector operation. The arguments
/// view must be the top-level MCP params.arguments object from the already
/// parsed request envelope. Unknown tools return nullopt.
std::optional<std::string> handle_live_inspector_tool(
    std::string_view name,
    const choc::value::ValueView& arguments);

} // namespace pulp_mcp
