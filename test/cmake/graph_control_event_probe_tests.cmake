if(TARGET pulp-graph-control-event-probe)
    add_test(NAME graph-control-event-probe-consumer
             COMMAND pulp-graph-control-event-probe)
    set_tests_properties(graph-control-event-probe-consumer PROPERTIES LABELS "graph;g7")
endif()
if(UNIX AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME cmake-graph-control-event-probe-sdk-consumer
             COMMAND ${CMAKE_COMMAND}
                 "-DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                 "-DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}"
                 "-DPULP_PARENT_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_graph_control_event_probe_sdk_consumer.cmake")
    set_tests_properties(cmake-graph-control-event-probe-sdk-consumer PROPERTIES
                         LABELS "cmake;sdk;graph;g7" TIMEOUT 600)
endif()
