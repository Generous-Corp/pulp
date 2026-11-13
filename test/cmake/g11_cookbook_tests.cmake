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

# Notation files for the graph cookbooks. Each fixture's graph names
# application Processors the default CLI catalog cannot construct, so the real
# validator must refuse it; the test pins that exact refusal.
if(NOT ANDROID AND NOT IOS AND PULP_ENABLE_GPU AND PROJECT_IS_TOP_LEVEL)
    set(_g11_cli "${CMAKE_BINARY_DIR}/tools/cli/pulp-cpp${CMAKE_EXECUTABLE_SUFFIX}")
    if(CMAKE_CONFIGURATION_TYPES)
        set(_g11_cli "${CMAKE_BINARY_DIR}/tools/cli/$<CONFIG>/pulp-cpp${CMAKE_EXECUTABLE_SUFFIX}")
    endif()
    foreach(_entry
            "graph-named-bus-cookbook|placement|pulp.cookbook.NamedBusPlacement"
            "graph-spatial-cookbook|stage|cookbook.static-stereo.v1"
            "g11-tuning-cookbook|tone|pulp.cookbook.TuningToneProcessor"
            "g11-recording-publication|capture|pulp.g11.CaptureNode"
            "g11-live-retune-consumer|voice|pulp.g11.LiveRetuningInstrument")
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _example)
        list(GET _parts 1 _node)
        list(GET _parts 2 _type)
        add_test(NAME ${_example}-notation-refusal
            COMMAND ${CMAKE_COMMAND}
                "-DPULP_CLI=${_g11_cli}"
                "-DPAT=${CMAKE_SOURCE_DIR}/examples/${_example}/graph.pat"
                -DNODE=${_node} "-DTYPE=${_type}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/g11_notation_refusal.cmake)
        set_tests_properties(${_example}-notation-refusal PROPERTIES
            LABELS "graph;g11;notation" TIMEOUT 60)
    endforeach()
endif()
