add_executable(pulp-test-spectral-graph-persistence test_spectral_graph_persistence.cpp)
target_link_libraries(pulp-test-spectral-graph-persistence PRIVATE pulp::host Catch2::Catch2WithMain)
pulp_test_spawns(pulp-test-spectral-graph-persistence NONE
    REASON "GraphSerializer inputs are generated in memory from closed built-in spectral and Audio nodes with no plugin or custom callback registered or loaded")
catch_discover_tests(pulp-test-spectral-graph-persistence)

# This records the pinned fixture using the same public consumer source. The
# installed-SDK gate separately builds it with find_package(Pulp), without
# access to source-tree include directories or targets.
add_executable(pulp-a3-replay-fixture-writer
    ../examples/a3-spectral-replay-consumer/main.cpp)
target_link_libraries(pulp-a3-replay-fixture-writer PRIVATE pulp::host)
target_compile_features(pulp-a3-replay-fixture-writer PRIVATE cxx_std_20)

if(UNIX AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME cmake-a3-spectral-replay-sdk-consumer
        COMMAND ${CMAKE_COMMAND}
            "-DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DPULP_PARENT_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DPULP_PARENT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DPULP_PARENT_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            "-DPULP_PARENT_INSTRUMENTATION_CXX_FLAGS=${_sdk_consumer_instrumentation_compile_flags}"
            "-DPULP_PARENT_INSTRUMENTATION_LINKER_FLAGS=${_sdk_consumer_instrumentation_link_flags}"
            -P "${CMAKE_CURRENT_LIST_DIR}/test_spectral_replay_consumer.cmake")
    set_tests_properties(cmake-a3-spectral-replay-sdk-consumer PROPERTIES
        LABELS "cmake;sdk;host;spectral;needs-build" TIMEOUT 600)
endif()
