if(TARGET pulp-host)
    add_executable(pulp-test-trigger-runtime
        "${CMAKE_CURRENT_SOURCE_DIR}/../examples/g5-trigger-runtime-consumer/main.cpp")
    target_link_libraries(pulp-test-trigger-runtime PRIVATE pulp::host)
    add_test(NAME trigger-runtime-public-consumer COMMAND pulp-test-trigger-runtime)
    set_tests_properties(trigger-runtime-public-consumer PROPERTIES LABELS "graph;unit")
endif()
if(TARGET pulp-host)
    pulp_add_test_group(pulp-test-group-trigger-graph LIBRARIES pulp::host)
    pulp_add_test_suite(pulp-test-trigger-graph GROUP pulp-test-group-trigger-graph
        SOURCES test_trigger_graph.cpp test_g5_host_event_contract.cpp
                harness/rt_allocation_probe.cpp LIBRARIES pulp::host
        PROPERTIES RESOURCE_LOCK pulp_realtime_worker_pool)
endif()
if(TARGET pulp-host AND TARGET clap)
    # The VST3 SDK, when present, lets the VST3/AU timing test check its
    # mirrored constants against the real ProcessContext flags.
    set(_g5_host_timestamp_libs pulp::host clap)
    if(TARGET vst3-sdk)
        list(APPEND _g5_host_timestamp_libs vst3-sdk)
    endif()
    pulp_add_test_suite(pulp-test-g5-host-event-timestamps
        SOURCES test_g5_host_event_timestamps.cpp test_g5_host_event_timestamps_vst3_au.cpp
        LIBRARIES ${_g5_host_timestamp_libs})
endif()
