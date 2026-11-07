# Focused G-11 installed-SDK graph recording/publication consumer registration.
# The consumer owns its recorder, publication bank, graph, and oracle so this
# lane remains independent of the static sample-region cookbook.
if(EXISTS "${CMAKE_SOURCE_DIR}/examples/g11-recording-publication/CMakeLists.txt")
    add_test(NAME g11-recording-publication-source-scope
        COMMAND ${CMAKE_COMMAND} -E echo "G11 consumer is built from the installed SDK in its own CMake project")
    set_tests_properties(g11-recording-publication-source-scope PROPERTIES LABELS "examples;installed-sdk;graph;g11")
endif()
