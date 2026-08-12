#pragma once

namespace pulp::inspect::detail {

struct ControlInstalledHostTestAccess {
    static void fail_owner_construction_after_heartbeat_start();
    static bool state_destruction_waits_for_heartbeat_mutex();
};

} // namespace pulp::inspect::detail
