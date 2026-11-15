if(EXISTS "${CMAKE_SOURCE_DIR}/examples/a2-modular-instrument-consumer/CMakeLists.txt")
    add_test(NAME a2-modular-instrument-installed-sdk
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DPULP_PARENT_BUILD_TYPE=$<CONFIG>
            "-DPULP_PARENT_GENERATOR=${CMAKE_GENERATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_a2_modular_instrument_consumer.cmake)
    set_tests_properties(a2-modular-instrument-installed-sdk PROPERTIES
        LABELS "examples;installed-sdk;graph;a2;slow"
        TIMEOUT 1800)
endif()
