# The same public consumer runs against the source target in normal CI.
add_executable(pulp-test-trigger-admission
    "${CMAKE_CURRENT_SOURCE_DIR}/../examples/g5-trigger-admission-consumer/main.cpp")
target_link_libraries(pulp-test-trigger-admission PRIVATE pulp::graph)
add_test(NAME trigger-admission-public-consumer COMMAND pulp-test-trigger-admission)
set_tests_properties(trigger-admission-public-consumer PROPERTIES LABELS "graph;unit")
