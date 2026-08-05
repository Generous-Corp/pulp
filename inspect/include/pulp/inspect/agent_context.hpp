#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::inspect {

/// Host-owned snapshot returned by Inspector.getAgentContext. The protocol
/// layer owns serialization; hosts only report facts from their live instance.
struct InspectorAgentContext {
    std::string binary_path;
    std::string binary_build_id;
    std::int64_t binary_mtime_unix_ms = 0;
    std::string plugin_id;
    std::string session_id;
    std::string instance_id;
    bool editor_open = false;
    bool window_visible = false;
    bool processing = false;
    std::uint64_t xrun_count = 0;
    bool hot_reload_available = false;
    bool hot_reload_enabled = false;
    bool hot_reload_pending = false;
    std::uint64_t unsaved_tweak_count = 0;
    std::vector<std::string> actionable_issues;
};

/// Composition seam for host state that does not belong in protocol dispatch.
class InspectorAgentContextSource {
  public:
    virtual ~InspectorAgentContextSource() = default;
    virtual InspectorAgentContext snapshot() const = 0;
};

} // namespace pulp::inspect
