if(TARGET pulp-host)
    pulp_add_test_suite(pulp-test-graph-patch LIBRARIES pulp::host)
    pulp_test_data(pulp-test-graph-patch NO_DEFINE PATHS examples/graph-patch-consumer/hello.pat)
    pulp_test_spawns(pulp-test-graph-patch NONE REASON "uses only in-process graph and offline host APIs")
    target_compile_definitions(pulp-test-graph-patch PRIVATE
        PULP_PATCH_HELLO="${CMAKE_SOURCE_DIR}/examples/graph-patch-consumer/hello.pat")
    add_test(NAME graph-patch-grammar-fixture
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rack/generate_signal_graph_grammar.py --check)
endif()

if(UNIX AND NOT CMAKE_CROSSCOMPILING AND TARGET pulp-host)
    add_test(NAME cmake-graph-patch-sdk-consumer COMMAND ${CMAKE_COMMAND}
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
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_graph_patch_sdk_consumer.cmake")
    set_tests_properties(cmake-graph-patch-sdk-consumer PROPERTIES LABELS "cmake;sdk;graph;g2" TIMEOUT 600)
endif()

# Whole-file graph reload is the graph-patch document format applied live.
include("${CMAKE_CURRENT_LIST_DIR}/graph_file_reload_tests.cmake")
