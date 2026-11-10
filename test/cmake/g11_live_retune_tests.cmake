# Installed public-header consumer; installation is owned by the parent SDK gate.
if(EXISTS "${CMAKE_SOURCE_DIR}/examples/g11-live-retune-consumer/CMakeLists.txt")
    add_test(NAME g11-live-retune-installed-sdk
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/examples/g11-live-retune-consumer/installed_gate.cmake)
    set_tests_properties(g11-live-retune-installed-sdk PROPERTIES
        LABELS "examples;installed-sdk;graph;g11;slow" TIMEOUT 1800)
endif()
