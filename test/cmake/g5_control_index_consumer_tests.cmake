if(EXISTS "${CMAKE_SOURCE_DIR}/examples/g5-control-index-filter/CMakeLists.txt")
    add_test(NAME g5-control-index-filter-installed-sdk
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DPULP_PARENT_BUILD_TYPE=$<CONFIG>
            "-DPULP_PARENT_GENERATOR=${CMAKE_GENERATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_g5_control_index_consumer.cmake)
    set_tests_properties(g5-control-index-filter-installed-sdk PROPERTIES
        LABELS "examples;installed-sdk;graph;g5;slow"
        TIMEOUT 1800)
endif()
