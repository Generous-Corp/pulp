if(TARGET pulp-host)
    add_executable(pulp-test-g5-host-event-consumer
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/g5-host-event-consumer/main.cpp")
    target_link_libraries(pulp-test-g5-host-event-consumer PRIVATE pulp::host)
    target_compile_features(pulp-test-g5-host-event-consumer PRIVATE cxx_std_20)
    add_test(NAME g5-host-event-public-consumer COMMAND pulp-test-g5-host-event-consumer)
    set_tests_properties(g5-host-event-public-consumer PROPERTIES
        LABELS "graph;unit" TIMEOUT 10)
endif()
