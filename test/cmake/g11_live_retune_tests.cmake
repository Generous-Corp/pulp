# Installed public-header consumer; installation is owned by the parent SDK gate.
if(TARGET pulp-host AND UNIX AND NOT CMAKE_CROSSCOMPILING
   AND EXISTS "${CMAKE_SOURCE_DIR}/examples/g11-live-retune-consumer/CMakeLists.txt"
   AND (CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Release"))
    add_test(NAME g11-live-retune-installed-sdk
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            "-DPULP_PARENT_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DPULP_PARENT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DPULP_PARENT_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            "-DPULP_PARENT_INSTRUMENTATION_CXX_FLAGS=${_sdk_consumer_instrumentation_compile_flags}"
            "-DPULP_PARENT_INSTRUMENTATION_LINKER_FLAGS=${_sdk_consumer_instrumentation_link_flags}"
            "-DPULP_PARENT_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
            -P ${CMAKE_SOURCE_DIR}/examples/g11-live-retune-consumer/installed_gate.cmake
        CONFIGURATIONS Release)
    set_tests_properties(g11-live-retune-installed-sdk PROPERTIES
        LABELS "examples;installed-sdk;graph;g11;slow" TIMEOUT 1800)
endif()
