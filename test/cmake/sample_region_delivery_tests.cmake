# The same example builds against source targets or an installed SDK.
if(TARGET pulp::host AND NOT TARGET sample-region-allpass-core)
    add_subdirectory("${PULP_ROOT_DIR}/examples/sample-region-allpass"
                     "${CMAKE_BINARY_DIR}/examples/sample-region-allpass")
endif()
if(TARGET sample-region-allpass-consumer)
    add_test(NAME sample-region-allpass-consumer
        COMMAND sample-region-allpass-consumer
            "${CMAKE_CURRENT_BINARY_DIR}/sample-region-allpass-artifacts")
    set_tests_properties(sample-region-allpass-consumer PROPERTIES
        LABELS "audio;host;sample-region" TIMEOUT 120)
endif()

if(TARGET pulp-test-agent-capability-compile AND TARGET pulp::host)
    target_link_libraries(pulp-test-agent-capability-compile PRIVATE pulp::host)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME sample-region-catalog-contract
        COMMAND "${Python3_EXECUTABLE}"
            "${PULP_ROOT_DIR}/tools/scripts/test_sample_region_catalogs.py")
    set_tests_properties(sample-region-catalog-contract PROPERTIES
        LABELS "catalog;sample-region" TIMEOUT 60)
endif()

if(TARGET pulp-test-lv2-adapter AND TARGET sample-region-allpass-core)
    target_link_libraries(pulp-test-lv2-adapter PRIVATE sample-region-allpass-core)
endif()

# Inspector sample-region control proofs share this already included manifest so
# the test/CMakeLists hub remains untouched while its path is owned elsewhere.
include("${CMAKE_CURRENT_LIST_DIR}/sample_region_control_tests.cmake")
