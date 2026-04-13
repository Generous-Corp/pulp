# view-bridge-demo — exercises Processor::create_view() and multi-view attach
#
# This demo is a small executable (not a plugin) that drives ViewBridge
# headlessly: it constructs a processor with a custom view, opens and
# resizes an editor, attaches an inspector as a secondary view, and
# prints the resulting lifecycle trace. It's the canonical "custom view
# + secondary attach" recipe referenced from docs/guides/view-bridge.md.

add_executable(pulp-view-bridge-demo main.cpp)
target_link_libraries(pulp-view-bridge-demo PRIVATE pulp::format pulp::view)

if(PULP_BUILD_TESTS)
    add_test(NAME view-bridge-demo COMMAND pulp-view-bridge-demo)
endif()
