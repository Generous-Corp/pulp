#include "../inspect/src/control_installed_host_test_access.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

TEST_CASE("installed host joins its heartbeat worker when owner construction fails",
          "[inspect][control][host][lifecycle]") {
    CHECK_THROWS_WITH(
        pulp::inspect::detail::ControlInstalledHostTestAccess::
            fail_owner_construction_after_heartbeat_start(),
        "installed host owner construction failed");
}

TEST_CASE("installed host state destruction synchronizes heartbeat shutdown",
          "[inspect][control][host][lifecycle]") {
    CHECK(pulp::inspect::detail::ControlInstalledHostTestAccess::
              state_destruction_waits_for_heartbeat_mutex());
}
