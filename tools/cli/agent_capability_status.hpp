// agent_capability_status.hpp — report the agent-capability counters.
//
// `tools/scripts/agent_capability_manifest.py --status` classifies each counter
// as assigned or pending. A pending counter means the checkout's capability
// material changed after the counter was last assigned; the version-at-land
// bump that releases the change assigns the next value. Until then the number
// names no published contract, so `pulp status`, `pulp doctor` and the MCP
// status tool say so rather than printing a bare integer an agent would read as
// a released revision. Header-only so the CLI and the MCP server share it.
#pragma once

#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace pulp::cli {

struct AgentCapabilityCounter {
    std::string name;   // "manifest_revision" or "inventory_version"
    long value = 0;
    bool pending = false;
};

// Parse `--status` JSON ({"<name>": {"state": "...", "value": N}, ...}).
// Returns nullopt unless both counters are present and well formed.
inline std::optional<std::vector<AgentCapabilityCounter>>
parse_agent_capability_status(const std::string& json) {
    std::vector<AgentCapabilityCounter> counters;
    for (const char* name : {"manifest_revision", "inventory_version"}) {
        const std::regex entry(std::string("\"") + name +
                               "\"\\s*:\\s*\\{\\s*\"state\"\\s*:\\s*\"(assigned|pending)\""
                               "\\s*,\\s*\"value\"\\s*:\\s*([0-9]+)\\s*\\}");
        std::smatch match;
        if (!std::regex_search(json, match, entry)) return std::nullopt;
        counters.push_back({name, std::stol(match[2].str()), match[1].str() == "pending"});
    }
    return counters;
}

// One line, e.g. "manifest_revision 77 (assigned), inventory_version 148
// (pending: not a published revision; the next SDK release assigns 149)".
inline std::string agent_capability_status_line(const std::vector<AgentCapabilityCounter>& counters) {
    std::string line;
    for (const auto& counter : counters) {
        if (!line.empty()) line += ", ";
        line += counter.name + " " + std::to_string(counter.value);
        line += counter.pending
            ? " (pending: not a published revision; the next SDK release assigns " +
                  std::to_string(counter.value + 1) + ")"
            : " (assigned)";
    }
    return line;
}

inline bool any_agent_capability_counter_pending(const std::vector<AgentCapabilityCounter>& counters) {
    for (const auto& counter : counters) {
        if (counter.pending) return true;
    }
    return false;
}

}  // namespace pulp::cli
