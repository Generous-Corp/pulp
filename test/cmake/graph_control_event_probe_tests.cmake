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
                 "-DPULP_PARENT_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                 "-DPULP_PARENT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
                 "-DPULP_PARENT_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
                 "-DPULP_PARENT_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
                 "-DPULP_PARENT_SANITIZER=${PULP_SANITIZER}"
                 "-DPULP_PARENT_INSTRUMENTATION_CXX_FLAGS=${_sdk_consumer_instrumentation_compile_flags}"
                 "-DPULP_PARENT_INSTRUMENTATION_LINKER_FLAGS=${_sdk_consumer_instrumentation_link_flags}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_graph_control_event_probe_sdk_consumer.cmake")
    set_tests_properties(cmake-graph-control-event-probe-sdk-consumer PROPERTIES
                         LABELS "cmake;sdk;graph;g7" TIMEOUT 600)
endif()

pulp_add_test_suite(pulp-test-graph-control-event-probe
    SOURCES test_graph_control_event_probe.cpp
    LIBRARIES pulp::host)
