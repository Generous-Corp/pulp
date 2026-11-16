if(TARGET pulp-host)
    add_executable(pulp-test-g5-host-event-consumer
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/g5-host-event-consumer/main.cpp")
    target_link_libraries(pulp-test-g5-host-event-consumer PRIVATE pulp::host)
    target_compile_features(pulp-test-g5-host-event-consumer PRIVATE cxx_std_20)
    add_test(NAME g5-host-event-public-consumer COMMAND pulp-test-g5-host-event-consumer)
    set_tests_properties(g5-host-event-public-consumer PROPERTIES
        LABELS "graph;unit" TIMEOUT 10)
endif()

# The installed-SDK P1 event-admission consumer, built in-tree from the same
# source so the required gate runs its combined four-family control on every
# change to the runtime it consumes.
if(TARGET pulp-host)
    add_executable(pulp-test-p1-event-admission-consumer
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p1-event-admission-consumer/main.cpp")
    target_link_libraries(pulp-test-p1-event-admission-consumer PRIVATE pulp::host)
    target_compile_features(pulp-test-p1-event-admission-consumer PRIVATE cxx_std_20)
    add_test(NAME p1-event-admission-in-tree-consumer
        COMMAND pulp-test-p1-event-admission-consumer)
    set_tests_properties(p1-event-admission-in-tree-consumer PROPERTIES
        LABELS "graph;events;unit" TIMEOUT 120
        PASS_REGULAR_EXPRESSION "p1-event-admission PASS checks=[0-9]+"
        FAIL_REGULAR_EXPRESSION "p1-event-admission FAIL")
endif()

# The P1 event-admission consumer's RTSan lane; registers nothing unless the
# tree is configured with PULP_SANITIZER=realtime.
include("${CMAKE_CURRENT_LIST_DIR}/p1_event_admission_rtsan_tests.cmake")
