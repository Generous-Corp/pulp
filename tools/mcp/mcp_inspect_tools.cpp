// mcp_inspect_tools.cpp — Design inspection request MCP handlers.

#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools.hpp"

#include <pulp/inspect/agent_request_queue.hpp>

#include <cstddef>
#include <string>

namespace pulp_mcp {

// Read the pull-based agent-request queue (.pulp-design-requests.json) for a
// design project and return its not-yet-consumed requests as a JSON array.
// This is an in-process read of the pulp::inspect queue core — no CLI shell-out
// and no audio device — so it degrades honestly: an absent or empty queue is an
// empty array, never an error. `project_dir` locates the queue; when omitted we
// fall back to the enclosing Pulp project root, matching the cwd-based
// resolution the neighboring pulp_inspect_* tools use.
std::string handle_inspect_pending_requests(const std::string& params_json) {
    auto project_dir = extract_string(params_json, "project_dir");
    if (project_dir.empty()) {
        auto root = find_project_root();
        if (root.empty()) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
        }
        project_dir = root.string();
    }

    const auto path = pulp::inspect::queue_path(project_dir);
    const auto pending = pulp::inspect::read_pending_file(path);

    std::string arr = "[";
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& r = pending[i];
        if (i != 0)
            arr += ",";
        arr += "{\"id\":" + json_string(r.id);
        arr += ",\"text\":" + json_string(r.text);
        arr += ",\"design\":" + json_string(r.design);
        arr += ",\"screen\":" + json_string(r.screen);
        arr += ",\"editmode_state\":" + json_string(r.editmode_state);
        arr += ",\"screenshot_path\":" + json_string(r.screenshot_path);
        arr += ",\"created_at\":" + json_string(r.created_at);
        arr += ",\"consumed\":";
        arr += r.consumed ? "true" : "false";
        arr += "}";
    }
    arr += "]";
    return json_tool_payload(arr);
}

} // namespace pulp_mcp
