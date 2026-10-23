// The agent-capability status line `pulp status`, `pulp doctor` and the MCP
// status tool print from `agent_capability_manifest.py --status`.

#include "../tools/cli/agent_capability_status.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using pulp::cli::agent_capability_status_line;
using pulp::cli::any_agent_capability_counter_pending;
using pulp::cli::parse_agent_capability_status;

namespace {

// The tool prints json.dumps(report, indent=2, sort_keys=True).
std::string status_json(const char* manifest_state, int manifest, const char* surface_state, int surface) {
    return std::string("{\n  \"inventory_version\": {\n    \"state\": \"") + surface_state +
           "\",\n    \"value\": " + std::to_string(surface) +
           "\n  },\n  \"manifest_revision\": {\n    \"state\": \"" + manifest_state +
           "\",\n    \"value\": " + std::to_string(manifest) + "\n  }\n}\n";
}

}  // namespace

TEST_CASE("Agent-capability status: assigned counters read as assigned", "[cli][agent-capabilities]") {
    const auto counters = parse_agent_capability_status(status_json("assigned", 77, "assigned", 148));
    REQUIRE(counters.has_value());
    CHECK_FALSE(any_agent_capability_counter_pending(*counters));
    CHECK(agent_capability_status_line(*counters) ==
          "manifest_revision 77 (assigned), inventory_version 148 (assigned)");
}

TEST_CASE("Agent-capability status: a pending counter is named, never a bare number", "[cli][agent-capabilities]") {
    const auto counters = parse_agent_capability_status(status_json("pending", 77, "assigned", 148));
    REQUIRE(counters.has_value());
    CHECK(any_agent_capability_counter_pending(*counters));
    const auto line = agent_capability_status_line(*counters);
    CHECK(line.find("manifest_revision 77 (pending: not a published revision; the next SDK "
                    "release assigns 78)") != std::string::npos);
    CHECK(line.find("inventory_version 148 (assigned)") != std::string::npos);
}

TEST_CASE("Agent-capability status: malformed or partial output is not reported", "[cli][agent-capabilities]") {
    CHECK_FALSE(parse_agent_capability_status("").has_value());
    CHECK_FALSE(parse_agent_capability_status("Traceback (most recent call last):").has_value());
    CHECK_FALSE(parse_agent_capability_status(
                    "{\"manifest_revision\": {\"state\": \"assigned\", \"value\": 77}}")
                    .has_value());
    CHECK_FALSE(parse_agent_capability_status(status_json("unknown", 77, "assigned", 148)).has_value());
}
