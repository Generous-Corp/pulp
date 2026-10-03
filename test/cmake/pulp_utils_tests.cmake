# Compatibility smoke for the PulpUtils shim and every helper it includes.
add_test(NAME cmake-pulp-utils-compat
    COMMAND ${CMAKE_COMMAND}
        -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        "-DPULP_PARENT_BUILD_TYPE=$<CONFIG>"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pulp_utils_compat.cmake)
set_tests_properties(cmake-pulp-utils-compat PROPERTIES
    LABELS "cmake;sdk;compat"
    TIMEOUT 180)
