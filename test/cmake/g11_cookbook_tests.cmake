# Installed-SDK gates for the graph cookbooks. Each example is configured,
# built and ctested out of tree against the installed package by the shared
# consumer driver; the example's own CMakeLists.txt owns its positive and
# marker-checked planted-control tests.
foreach(_entry
        "graph-named-bus-cookbook|2"
        "graph-spatial-cookbook|1"
        "g11-tuning-cookbook|3")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _example)
    list(GET _parts 1 _count)
    if(EXISTS "${CMAKE_SOURCE_DIR}/examples/${_example}/CMakeLists.txt")
        add_test(NAME ${_example}-installed-sdk
            COMMAND ${CMAKE_COMMAND}
                -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DPULP_EXAMPLE_NAME=${_example}
                -DPULP_EXPECTED_TESTS=${_count}
                -DPULP_PARENT_BUILD_TYPE=$<CONFIG>
                "-DPULP_PARENT_GENERATOR=${CMAKE_GENERATOR}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_g11_recording_publication_consumer.cmake)
        set_tests_properties(${_example}-installed-sdk PROPERTIES
            LABELS "examples;installed-sdk;graph;g11;slow"
            TIMEOUT 1800)
    endif()
endforeach()
