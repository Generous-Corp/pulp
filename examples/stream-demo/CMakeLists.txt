# Stream feature demo — file I/O, EventLoop-routed async reads, and optional HTTP GET.
add_executable(pulp-stream-demo main.cpp)
target_link_libraries(pulp-stream-demo PRIVATE pulp::runtime pulp::events)

if(PULP_BUILD_TESTS)
    add_test(NAME stream-demo-offline COMMAND pulp-stream-demo)
    set_tests_properties(stream-demo-offline PROPERTIES
        LABELS "streams;examples"
        PASS_REGULAR_EXPRESSION "closed=1"
        TIMEOUT 10)
endif()
