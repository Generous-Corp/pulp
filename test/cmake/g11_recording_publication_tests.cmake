# Focused G-11 installed-SDK graph recording/publication consumer gate.
# The consumer owns its recorder, publication bank, graph, and oracle so this
# lane remains independent of the static sample-region cookbook. The driver
# installs the SDK and configures/builds/ctests the standalone example project.
if(EXISTS "${CMAKE_SOURCE_DIR}/examples/g11-recording-publication/CMakeLists.txt")
    add_test(NAME g11-recording-publication-installed-sdk
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DPULP_PARENT_BUILD_TYPE=$<CONFIG>
            "-DPULP_PARENT_GENERATOR=${CMAKE_GENERATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_g11_recording_publication_consumer.cmake)
    set_tests_properties(g11-recording-publication-installed-sdk PROPERTIES
        LABELS "examples;installed-sdk;graph;g11;slow"
        TIMEOUT 1800)
endif()

# The graph cookbooks reuse this manifest's installed-SDK consumer driver.
include("${CMAKE_CURRENT_LIST_DIR}/g11_cookbook_tests.cmake")
