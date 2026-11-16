if(TARGET pulp-host)
    pulp_add_test_suite(pulp-test-g5-control-index
        SOURCES test_g5_control_index.cpp
        LIBRARIES pulp::host
        PROPERTIES RESOURCE_LOCK pulp_realtime_worker_pool)
    pulp_add_test_suite(pulp-test-control-scalar-parity
        SOURCES test_control_scalar_parity.cpp
        LIBRARIES pulp::host)
endif()

# The scalar Control headers through an installed include tree: canonical
# names, deprecated spellings, and the deprecation reaching the consumer.
add_test(NAME control-scalar-installed-headers
    COMMAND ${CMAKE_COMMAND}
        -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        "-DPULP_PARENT_GENERATOR=${CMAKE_GENERATOR}"
        "-DPULP_PARENT_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_control_scalar_consumer.cmake)
set_tests_properties(control-scalar-installed-headers PROPERTIES
    LABELS "signal;control-index;installed-headers"
    PASS_REGULAR_EXPRESSION "control_scalar_installed_consumer_verified=true"
    TIMEOUT 300)
